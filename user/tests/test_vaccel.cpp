// SPDX-License-Identifier: MIT
//
// Functional tests for the vaccel driver. Run as root with the module loaded
// and no other process using /dev/vaccel0 (every test is followed by a leak
// check that expects all device memory and channels to be free again).
//
//   ./test_vaccel            run everything
//   ./test_vaccel fence      run tests whose name contains "fence"
//   ./test_vaccel --list
#include "vaccel/vaccel.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vaccel;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

static const char* kDev = "/dev/vaccel0";
static const std::string kSys = "/sys/class/vaccel/vaccel0/";
static const std::string kParam = "/sys/module/vaccel/parameters/";

// ---- minimal test framework ----------------------------------------------

struct Failure {
    std::string msg;
};

struct TestCase {
    const char* name;
    void (*fn)();
};

static std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

#define TEST(name)                                         \
    static void test_##name();                             \
    static Registrar reg_##name(#name, test_##name);       \
    static void test_##name()

#define FAIL_AT(text)                                                       \
    do {                                                                    \
        std::ostringstream os_;                                             \
        os_ << __FILE__ << ":" << __LINE__ << ": " << text;                 \
        throw Failure{os_.str()};                                           \
    } while (0)

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond))                                                        \
            FAIL_AT("CHECK(" #cond ") failed");                             \
    } while (0)

#define CHECK_EQ(a, b)                                                      \
    do {                                                                    \
        auto va_ = (a);                                                     \
        auto vb_ = (b);                                                     \
        if (!(va_ == vb_))                                                  \
            FAIL_AT(#a " == " #b " failed: " << +va_ << " vs " << +vb_);    \
    } while (0)

// Expect `expr` to throw vaccel::Error with errno `e`.
#define CHECK_THROWS_ERRNO(expr, e)                                         \
    do {                                                                    \
        bool thrown_ = false;                                               \
        try {                                                               \
            (void)(expr);                                                   \
        } catch (const Error& err_) {                                       \
            thrown_ = true;                                                 \
            if (err_.err() != (e))                                          \
                FAIL_AT(#expr " threw errno " << err_.err() << " ("         \
                        << strerror(err_.err()) << "), expected " << (e));  \
        }                                                                   \
        if (!thrown_)                                                       \
            FAIL_AT(#expr " did not throw, expected errno " << (e));        \
    } while (0)

// ---- helpers --------------------------------------------------------------

static uint64_t page_size() { return static_cast<uint64_t>(sysconf(_SC_PAGESIZE)); }

static int raw_ioctl(int fd, unsigned long req, void* arg)
{
    return ::ioctl(fd, req, arg) < 0 ? -errno : 0;
}

static int raw_submit(int fd, uint32_t chan, const std::vector<vaccel_cmd>& cmds,
                      uint32_t flags = 0, uint64_t* fence = nullptr)
{
    vaccel_submit s{};
    s.chan = chan;
    s.flags = flags;
    s.cmds = reinterpret_cast<uintptr_t>(cmds.data());
    s.num_cmds = static_cast<uint32_t>(cmds.size());
    int r = raw_ioctl(fd, VACCEL_IOCTL_SUBMIT, &s);
    if (fence)
        *fence = s.fence;
    return r;
}

static vaccel_cmd fill_cmd(uint32_t dst, uint64_t off, uint64_t len, uint32_t v)
{
    vaccel_cmd c{};
    c.op = VACCEL_OP_FILL32;
    c.dst = dst;
    c.dst_off = off;
    c.len = len;
    c.value = v;
    return c;
}

static uint64_t read_u64(const std::string& path)
{
    std::ifstream f(path);
    uint64_t v = 0;
    if (!(f >> v))
        FAIL_AT("cannot read " << path);
    return v;
}

static void write_str(const std::string& path, const std::string& v)
{
    std::ofstream f(path);
    f << v;
    f.close();
    if (!f)
        FAIL_AT("cannot write " << path);
}

static uint64_t sysfs(const char* attr) { return read_u64(kSys + attr); }

// Sets a writable module parameter and restores it when the scope ends.
struct ParamGuard {
    std::string path, old;
    ParamGuard(const std::string& name, const std::string& value) : path(kParam + name)
    {
        old = std::to_string(read_u64(path));
        write_str(path, value);
    }
    ~ParamGuard()
    {
        std::ofstream f(path);
        f << old;
    }
};

// Poll until pred() is true or the timeout expires.
static bool eventually(const std::function<bool()>& pred, std::chrono::milliseconds t = 3000ms)
{
    auto end = Clock::now() + t;
    while (Clock::now() < end) {
        if (pred())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

static uint64_t largest_pow2_le(uint64_t x)
{
    uint64_t p = 1;
    while (p * 2 <= x)
        p *= 2;
    return p;
}

// ---- device and memory ----------------------------------------------------

TEST(info_reports_device)
{
    Device d(kDev);
    auto i = d.info();
    CHECK_EQ(i.api_version, (uint32_t)VACCEL_API_VERSION);
    CHECK_EQ((uint64_t)i.page_size, page_size());
    CHECK(i.vram_size > 0);
    CHECK_EQ(i.vram_free, i.vram_size);
    CHECK_EQ(i.largest_free_block,
             largest_pow2_le(i.vram_size / page_size()) * page_size());
    CHECK_EQ(i.free_channels, i.max_channels);
    CHECK_EQ(i.client_used, 0ull);
    CHECK_EQ(sysfs("vram_total_kb"), i.vram_size / 1024);
    CHECK_EQ(sysfs("channels_max"), (uint64_t)i.max_channels);
}

TEST(alloc_rounds_to_power_of_two_pages)
{
    Device d(kDev);
    const uint64_t pg = page_size();
    Buffer a = d.alloc(1);
    CHECK_EQ(a.size(), pg);
    Buffer b = d.alloc(5 * pg);
    CHECK_EQ(b.size(), 8 * pg);
    CHECK_EQ(a.dev_addr() % a.size(), 0ull);   // naturally aligned blocks
    CHECK_EQ(b.dev_addr() % b.size(), 0ull);
    CHECK_EQ(d.info().client_used, 9 * pg);
    CHECK_EQ(sysfs("vram_used_kb"), 9 * pg / 1024);
}

TEST(alloc_rejects_bad_requests)
{
    Device d(kDev);
    auto i = d.info();
    CHECK_THROWS_ERRNO(d.alloc(0), EINVAL);
    CHECK_THROWS_ERRNO(d.alloc(i.vram_size + 1), ENOMEM);
    CHECK_THROWS_ERRNO(d.alloc(UINT64_MAX), ENOMEM);   // no overflow when rounding

    vaccel_alloc a{};
    a.size = 4096;
    a.flags = 0x80;
    CHECK_EQ(raw_ioctl(d.fd(), VACCEL_IOCTL_ALLOC, &a), -EINVAL);

    vaccel_handle_arg h{12345, 0};
    CHECK_EQ(raw_ioctl(d.fd(), VACCEL_IOCTL_FREE, &h), -ENOENT);
    h = {0, 0};
    CHECK_EQ(raw_ioctl(d.fd(), VACCEL_IOCTL_FREE, &h), -ENOENT);
    CHECK_EQ(raw_ioctl(d.fd(), 0xdead, nullptr), -ENOTTY);
    CHECK_EQ(raw_ioctl(d.fd(), VACCEL_IOCTL_INFO, reinterpret_cast<void*>(1)), -EFAULT);
}

TEST(memory_is_zeroed_between_owners)
{
    Device d(kDev);
    uint64_t addr;
    {
        Buffer a = d.alloc(64 * 1024);
        addr = a.dev_addr();
        memset(a.data(), 0xAB, a.size());
    }
    Buffer b = d.alloc(64 * 1024);
    CHECK_EQ(b.dev_addr(), addr);   // same physical block was reused...
    auto* p = b.as<uint8_t>();
    for (uint64_t i = 0; i < b.size(); i++)
        if (p[i] != 0)
            FAIL_AT("stale byte at " << i);   // ...but none of the old data leaks
}

TEST(buddy_exhaustion_fragmentation_and_coalescing)
{
    Device d(kDev);
    const uint64_t pg = page_size();
    const auto info = d.info();
    const uint64_t pages = info.vram_size / pg;

    std::vector<Buffer> bufs;
    bufs.reserve(pages);
    for (uint64_t i = 0; i < pages; i++)
        bufs.push_back(d.alloc(pg, /*map=*/false));
    CHECK_THROWS_ERRNO(d.alloc(pg, false), ENOMEM);
    CHECK_EQ(d.info().vram_free, 0ull);
    CHECK_EQ(d.info().largest_free_block, 0ull);

    // Free every other page: half the memory is free but no 2-page block exists.
    for (uint64_t i = 0; i < pages; i += 2)
        bufs[i].reset();
    CHECK_EQ(d.info().vram_free, info.vram_size / 2);
    CHECK_EQ(d.info().largest_free_block, pg);
    CHECK_THROWS_ERRNO(d.alloc(2 * pg, false), ENOMEM);   // external fragmentation

    // Free the rest: every buddy pair merges back into the original blocks.
    bufs.clear();
    CHECK_EQ(d.info().vram_free, info.vram_size);
    CHECK_EQ(d.info().largest_free_block, info.largest_free_block);
}

TEST(mmap_rules)
{
    Device d(kDev);
    const uint64_t pg = page_size();
    vaccel_alloc a{};
    a.size = 2 * pg;
    CHECK_EQ(raw_ioctl(d.fd(), VACCEL_IOCTL_ALLOC, &a), 0);
    const off_t off = static_cast<off_t>(a.mmap_offset);

    errno = 0;
    CHECK(mmap(nullptr, 4 * pg, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd(), off) == MAP_FAILED);
    CHECK_EQ(errno, EINVAL);   // larger than the buffer
    errno = 0;
    CHECK(mmap(nullptr, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE, d.fd(), off) == MAP_FAILED);
    CHECK_EQ(errno, EINVAL);   // copy-on-write mapping refused
    errno = 0;
    CHECK(mmap(nullptr, pg, PROT_READ | PROT_EXEC, MAP_SHARED, d.fd(), off) == MAP_FAILED);
    CHECK_EQ(errno, EPERM);    // never executable
    errno = 0;
    CHECK(mmap(nullptr, pg, PROT_READ, MAP_SHARED, d.fd(), off + 777 * (off_t)pg) == MAP_FAILED);
    CHECK_EQ(errno, EINVAL);   // unknown handle

    // A partial mapping is fine, and it cannot be made executable later.
    void* p = mmap(nullptr, pg, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd(), off);
    CHECK(p != MAP_FAILED);
    CHECK(mprotect(p, pg, PROT_READ | PROT_EXEC) != 0);
    munmap(p, pg);

    vaccel_handle_arg h{a.handle, 0};
    CHECK_EQ(raw_ioctl(d.fd(), VACCEL_IOCTL_FREE, &h), 0);
}

TEST(free_while_mapped_keeps_memory_alive)
{
    Device d(kDev);
    const uint64_t pg = page_size();
    vaccel_alloc a{};
    a.size = pg;
    CHECK_EQ(raw_ioctl(d.fd(), VACCEL_IOCTL_ALLOC, &a), 0);
    auto* p = static_cast<uint32_t*>(
        mmap(nullptr, pg, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd(), (off_t)a.mmap_offset));
    CHECK(p != MAP_FAILED);
    p[0] = 0x5A5A5A5A;

    vaccel_handle_arg h{a.handle, 0};
    CHECK_EQ(raw_ioctl(d.fd(), VACCEL_IOCTL_FREE, &h), 0);
    CHECK_EQ(d.info().client_used, 0ull);          // the handle is gone...
    CHECK_EQ(sysfs("vram_used_kb"), pg / 1024);    // ...but the mapping pins the pages
    CHECK_EQ(p[0], 0x5A5A5A5Au);
    p[1] = 7;                                      // still safe to touch

    Buffer other = d.alloc(pg);
    CHECK(other.dev_addr() != a.dev_addr);         // pinned pages are not reused

    munmap(p, pg);
    CHECK_EQ(sysfs("vram_used_kb"), pg / 1024);    // only `other` remains
}

TEST(fork_shares_mapping_and_refcounts_it)
{
    Device d(kDev);
    Buffer b = d.alloc(page_size());
    auto* p = b.as<volatile uint32_t>();
    p[0] = 0x11;

    pid_t pid = fork();
    if (pid == 0) {
        _exit(p[0] == 0x11 ? (p[0] = 0x22, 0) : 1);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK_EQ(p[0], 0x22u);   // MAP_SHARED: the child's write reached device memory
}

// ---- engine and commands --------------------------------------------------

TEST(engine_fill_add_copy)
{
    Device d(kDev);
    Channel ch = d.create_channel();
    const uint64_t n = 1 << 20;
    Buffer a = d.alloc(n), b = d.alloc(n), c = d.alloc(n);

    CommandBatch batch;
    batch.fill32(a, 0, n, 7).fill32(b, 0, n, 35).add32(c, 0, a, 0, b, 0, n);
    CHECK(ch.wait(ch.submit(batch)));
    for (uint64_t i = 0; i < n / 4; i++)
        if (c.as<uint32_t>()[i] != 42)
            FAIL_AT("add32 wrong at word " << i);

    // u32 wrap-around
    batch.clear();
    batch.fill32(a, 0, 64, 0xFFFFFFFFu).fill32(b, 0, 64, 2).add32(c, 0, a, 0, b, 0, 64);
    CHECK(ch.wait(ch.submit(batch)));
    CHECK_EQ(c.as<uint32_t>()[0], 1u);

    // copy with unaligned offsets
    auto* pa = a.as<uint8_t>();
    for (uint64_t i = 0; i < n; i++)
        pa[i] = static_cast<uint8_t>(i * 31 + 7);
    batch.clear();
    batch.copy(b, 4096 + 3, a, 17, 100000);
    CHECK(ch.wait(ch.submit(batch)));
    CHECK(memcmp(b.as<uint8_t>() + 4096 + 3, pa + 17, 100000) == 0);
}

TEST(copy_overlap_matches_memmove)
{
    Device d(kDev);
    Channel ch = d.create_channel();
    Buffer a = d.alloc(512 * 1024);
    const uint64_t len = 300000;   // several 64 KiB chunks
    for (auto [dst, src] : {std::pair<uint64_t, uint64_t>{1000, 0}, {0, 1000}, {5, 5}}) {
        std::vector<uint8_t> ref(a.size());
        for (size_t i = 0; i < ref.size(); i++)
            ref[i] = static_cast<uint8_t>((i * 7) % 251);
        memcpy(a.data(), ref.data(), ref.size());
        memmove(ref.data() + dst, ref.data() + src, len);

        CommandBatch batch;
        batch.copy(a, dst, a, src, len);
        CHECK(ch.wait(ch.submit(batch)));
        if (memcmp(a.data(), ref.data(), ref.size()) != 0)
            FAIL_AT("overlapping copy dst=" << dst << " src=" << src << " differs from memmove");
    }
}

TEST(submit_validation)
{
    Device d(kDev);
    Channel ch = d.create_channel();
    Buffer b = d.alloc(8192);
    const int fd = d.fd();
    const uint32_t c = ch.handle(), h = b.handle();

    vaccel_cmd bad{};
    bad.op = 99;
    CHECK_EQ(raw_submit(fd, c, {bad}), -EINVAL);                                // unknown op
    CHECK_EQ(raw_submit(fd, c, {fill_cmd(h, 0, 0, 1)}), -EINVAL);               // empty
    CHECK_EQ(raw_submit(fd, c, {fill_cmd(h, 2, 8, 1)}), -EINVAL);               // misaligned offset
    CHECK_EQ(raw_submit(fd, c, {fill_cmd(h, 0, 6, 1)}), -EINVAL);               // misaligned length
    CHECK_EQ(raw_submit(fd, c, {fill_cmd(h, 8192 - 4, 8, 1)}), -EINVAL);        // past the end
    CHECK_EQ(raw_submit(fd, c, {fill_cmd(h, UINT64_MAX - 3, 8, 1)}), -EINVAL);  // off+len overflows
    CHECK_EQ(raw_submit(fd, c, {fill_cmd(9999, 0, 8, 1)}), -ENOENT);            // unknown buffer
    CHECK_EQ(raw_submit(fd, 9999, {fill_cmd(h, 0, 8, 1)}), -ENOENT);            // unknown channel
    CHECK_EQ(raw_submit(fd, c, {}), -EINVAL);                                   // no commands
    CHECK_EQ(raw_submit(fd, c, std::vector<vaccel_cmd>(VACCEL_MAX_CMDS + 1, fill_cmd(h, 0, 8, 1))),
             -EINVAL);                                                          // too many
    CHECK_EQ(raw_submit(fd, c, {fill_cmd(h, 0, 8, 1)}, 0x80), -EINVAL);         // unknown flag

    vaccel_cmd del{};
    del.op = VACCEL_OP_DELAY;
    del.value = VACCEL_MAX_DELAY_US + 1;
    CHECK_EQ(raw_submit(fd, c, {del}), -EINVAL);

    vaccel_cmd padded = fill_cmd(h, 0, 8, 1);
    padded.pad = 1;
    CHECK_EQ(raw_submit(fd, c, {padded}), -EINVAL);

    // A bad command anywhere rejects the whole batch, and nothing runs.
    b.as<uint32_t>()[0] = 0;
    CHECK_EQ(raw_submit(fd, c, {fill_cmd(h, 0, 8, 77), bad}), -EINVAL);
    vaccel_submit s{};
    s.chan = c;
    s.num_cmds = 1;
    s.cmds = 1;   // bad user pointer
    CHECK_EQ(raw_ioctl(fd, VACCEL_IOCTL_SUBMIT, &s), -EFAULT);
    CHECK(ch.wait(ch.submit(CommandBatch().fill32(b, 4, 4, 1))));
    CHECK_EQ(b.as<uint32_t>()[0], 0u);
    // The leak check after this test also proves that every error path
    // dropped the buffer references it had taken.
}

TEST(clients_are_isolated)
{
    Device d1(kDev), d2(kDev);
    Buffer b1 = d1.alloc(4096);
    Channel c1 = d1.create_channel();
    Channel c2 = d2.create_channel();

    // Handle numbers are per client, so both clients have a channel 1...
    CHECK_EQ(c1.handle(), c2.handle());
    CHECK(c1.hw_id() != c2.hw_id());   // ...backed by different hardware slots.

    // d2 cannot touch d1's buffer through any entry point.
    CHECK_EQ(raw_submit(d2.fd(), c2.handle(), {fill_cmd(b1.handle(), 0, 4, 1)}), -ENOENT);
    vaccel_handle_arg h{b1.handle(), 0};
    CHECK_EQ(raw_ioctl(d2.fd(), VACCEL_IOCTL_FREE, &h), -ENOENT);
    errno = 0;
    CHECK(mmap(nullptr, 4096, PROT_READ, MAP_SHARED, d2.fd(),
               (off_t)b1.handle() * (off_t)page_size()) == MAP_FAILED);
    CHECK_EQ(errno, EINVAL);
}

TEST(channel_pool_exhaustion_and_reuse)
{
    Device d(kDev);
    const uint32_t max = d.info().max_channels;
    std::vector<Channel> chans;
    std::vector<bool> seen(max, false);
    for (uint32_t i = 0; i < max; i++) {
        chans.push_back(d.create_channel());
        uint32_t id = chans.back().hw_id();
        CHECK(id < max);
        CHECK(!seen[id]);
        seen[id] = true;
    }
    CHECK_THROWS_ERRNO(d.create_channel(), EBUSY);
    CHECK_EQ(d.info().free_channels, 0u);
    CHECK_EQ(sysfs("channels_used"), (uint64_t)max);

    uint32_t freed = chans[3].hw_id();
    chans[3].reset();
    Channel again = d.create_channel();
    CHECK_EQ(again.hw_id(), freed);
}

TEST(per_client_quota)
{
    ParamGuard q("client_quota_mb", "1");
    Device d1(kDev), d2(kDev);
    CHECK_EQ(d1.info().client_quota, 1ull << 20);
    Buffer a = d1.alloc(512 * 1024), b = d1.alloc(512 * 1024);
    CHECK_THROWS_ERRNO(d1.alloc(4096), EDQUOT);
    Buffer c = d2.alloc(512 * 1024);   // the limit is per client
    a.reset();
    Buffer e = d1.alloc(4096);         // freeing gives quota back
}

// ---- fences, ordering and events ------------------------------------------

TEST(fence_wait_timeout_and_errors)
{
    Device d(kDev);
    Channel ch = d.create_channel();
    Fence f = ch.submit(CommandBatch().delay(50000));
    auto t0 = Clock::now();
    CHECK(!ch.wait(f, 1ms));      // still running
    CHECK(!ch.wait(f, 0ns));      // pure test
    CHECK(ch.wait(f));            // infinite wait
    CHECK(Clock::now() - t0 >= 40ms);
    CHECK(ch.wait(f, 0ns));       // signalled stays signalled
    CHECK(ch.wait(Fence{ch.handle(), 0}, 0ns));
    CHECK_THROWS_ERRNO(ch.wait(Fence{ch.handle(), f.seq + 100}), EINVAL);   // never issued
    CHECK_THROWS_ERRNO(ch.wait(Fence{4242, 1}), ENOENT);
}

TEST(jobs_complete_in_order)
{
    Device d(kDev);
    Channel ch = d.create_channel();
    Buffer b = d.alloc(4096);
    std::vector<Fence> fences;
    for (uint32_t i = 0; i < 200; i++) {
        for (;;) {
            try {
                fences.push_back(ch.submit(CommandBatch().fill32(b, 0, 4096, i)));
                break;
            } catch (const Error& e) {
                if (e.err() != EAGAIN)
                    throw;
                ch.wait(fences.back());   // queue full (max_queue_depth): drain, retry
            }
        }
        if (i)
            CHECK_EQ(fences[i].seq, fences[i - 1].seq + 1);
    }
    CHECK(ch.wait(fences.back()));
    for (auto& f : fences)
        CHECK(ch.wait(f, 0ns));
    CHECK_EQ(b.as<uint32_t>()[1023], 199u);
}

TEST(queue_depth_backpressure)
{
    ParamGuard q("max_queue_depth", "8");
    Device d(kDev);
    Channel ch = d.create_channel();
    int accepted = 0;
    Fence last{};
    try {
        for (int i = 0; i < 32; i++) {
            last = ch.submit(CommandBatch().delay(20000));
            accepted++;
        }
        FAIL_AT("queue never filled up");
    } catch (const Error& e) {
        CHECK_EQ(e.err(), EAGAIN);
    }
    CHECK(accepted >= 8 && accepted <= 9);   // 8 queued (+1 if the engine took one)
    CHECK(ch.wait(last));
}

TEST(completion_events_read_and_poll)
{
    Device d(kDev, /*nonblock=*/true);
    Channel ch = d.create_channel();
    Buffer b = d.alloc(4096);
    CHECK(!d.wait_events(0));
    CHECK(d.read_events().empty());   // O_NONBLOCK read -> EAGAIN

    std::vector<Fence> f;
    for (int i = 0; i < 3; i++)
        f.push_back(ch.submit(CommandBatch().fill32(b, 0, 4096, i), /*notify=*/true));
    Fence quiet = ch.submit(CommandBatch().fill32(b, 0, 4096, 9));   // no event
    CHECK(ch.wait(quiet));

    CHECK(d.wait_events(1000));
    auto ev = d.read_events();
    CHECK_EQ(ev.size(), (size_t)3);
    for (size_t i = 0; i < ev.size(); i++) {
        CHECK_EQ(ev[i].chan, ch.handle());
        CHECK_EQ(ev[i].fence, f[i].seq);
        CHECK_EQ(ev[i].status, (uint32_t)VACCEL_STATUS_OK);
        CHECK(ev[i].submit_ns <= ev[i].start_ns && ev[i].start_ns <= ev[i].end_ns);
    }
    char small[8];
    CHECK(::read(d.fd(), small, sizeof(small)) < 0 && errno == EINVAL);
}

TEST(round_robin_prevents_starvation)
{
    Device d(kDev, true);
    Channel hog = d.create_channel();
    Channel victim = d.create_channel();
    const uint32_t quantum = (uint32_t)read_u64(kParam + "sched_quantum");

    for (int i = 0; i < 64; i++)
        hog.submit(CommandBatch().delay(2000), true);        // ~128 ms of work
    Fence v = victim.submit(CommandBatch().delay(2000), true);
    CHECK(victim.wait(v));

    std::vector<vaccel_event> all;
    while (all.size() < 65) {
        CHECK(d.wait_events(5000));
        auto ev = d.read_events(128);
        all.insert(all.end(), ev.begin(), ev.end());
    }
    const vaccel_event* ve = nullptr;
    for (auto& e : all)
        if (e.chan == victim.handle())
            ve = &e;
    CHECK(ve != nullptr);
    auto count_hog = [&](auto pred) {
        return (size_t)std::count_if(all.begin(), all.end(), [&](const vaccel_event& e) {
            return e.chan == hog.handle() && pred(e);
        });
    };
    // Hog jobs that ran while the victim was queued, and hog jobs it overtook.
    size_t ran_while_waiting = count_hog([&](const vaccel_event& e) {
        return e.end_ns > ve->submit_ns && e.end_ns <= ve->end_ns;
    });
    size_t overtaken = count_hog([&](const vaccel_event& e) { return e.end_ns > ve->end_ns; });
    printf("      victim waited behind %zu hog jobs and overtook %zu (quantum %u)\n",
           ran_while_waiting, overtaken, quantum);
    // FIFO would make the victim wait for every queued hog job. Round-robin
    // bounds the wait to the job already running plus one quantum.
    CHECK(ran_while_waiting <= quantum + 1);
    CHECK(overtaken > 0);
}

// ---- teardown paths -------------------------------------------------------

TEST(close_with_pending_work_cancels_and_reclaims)
{
    const uint64_t cancelled0 = sysfs("jobs_cancelled");
    int fd = ::open(kDev, O_RDWR);
    CHECK(fd >= 0);
    vaccel_chan_create cc{};
    CHECK_EQ(raw_ioctl(fd, VACCEL_IOCTL_CHAN_CREATE, &cc), 0);
    vaccel_alloc a{};
    a.size = 1 << 20;
    CHECK_EQ(raw_ioctl(fd, VACCEL_IOCTL_ALLOC, &a), 0);

    vaccel_cmd del{};
    del.op = VACCEL_OP_DELAY;
    del.value = 20000;
    for (int i = 0; i < 50; i++)
        CHECK_EQ(raw_submit(fd, cc.handle, {del, fill_cmd(a.handle, 0, a.size, i)}), 0);

    ::close(fd);   // no explicit free/destroy: the driver must clean up
    CHECK(eventually([] { return sysfs("vram_used_kb") == 0 && sysfs("channels_used") == 0; }));
    CHECK(sysfs("jobs_cancelled") > cancelled0);
}

TEST(killed_process_resources_are_reclaimed)
{
    pid_t pid = fork();
    if (pid == 0) {
        Device d(kDev);
        Channel ch = d.create_channel();
        Buffer b = d.alloc(4 << 20);
        for (int i = 0; i < 20; i++)
            ch.submit(CommandBatch().delay(10000).fill32(b, 0, b.size(), i));
        raise(SIGKILL);   // die with everything still allocated and queued
        _exit(1);
    }
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    CHECK(eventually([] { return sysfs("vram_used_kb") == 0 && sysfs("channels_used") == 0; }));
}

TEST(destroy_channel_wakes_waiters)
{
    Device d(kDev);
    Channel ch = d.create_channel();
    Fence first = ch.submit(CommandBatch().delay(30000));
    Fence last{};
    for (int i = 0; i < 10; i++)
        last = ch.submit(CommandBatch().delay(30000));

    std::atomic<int> result{1};
    std::thread waiter([&] {
        vaccel_wait w{};
        w.chan = last.chan;
        w.fence = last.seq;
        w.timeout_ns = -1;
        result = raw_ioctl(d.fd(), VACCEL_IOCTL_WAIT, &w);
    });
    std::this_thread::sleep_for(50ms);
    ch.reset();   // destroy while a thread is blocked on it
    waiter.join();
    CHECK_EQ(result.load(), -ECANCELED);
    (void)first;
}

// ---- concurrency ----------------------------------------------------------

TEST(concurrent_clients_stress)
{
    constexpr int kIters = 150;
    Device shared(kDev);
    const int kThreads = (int)std::min(8u, shared.info().max_channels);   // one channel each
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&, t] {
            try {
                // Half the threads share one client, half have their own.
                Device own(kDev);
                const Device& d = (t % 2) ? shared : own;
                Channel ch = d.create_channel();
                for (int i = 0; i < kIters; i++) {
                    uint64_t sz = 4096ull << ((t + i) % 6);
                    Buffer b = d.alloc(sz);
                    uint32_t v = static_cast<uint32_t>(t * 100000 + i);
                    CommandBatch batch;
                    batch.fill32(b, 0, sz, v);
                    if (!ch.wait(ch.submit(batch)) ||
                        b.as<uint32_t>()[0] != v || b.as<uint32_t>()[sz / 4 - 1] != v)
                        errors++;
                }
            } catch (const std::exception& e) {
                fprintf(stderr, "thread %d: %s\n", t, e.what());
                errors++;
            }
        });
    }
    // Plus one thread that only churns allocations.
    threads.emplace_back([&] {
        try {
            Device d(kDev);
            for (int i = 0; i < 2000; i++) {
                Buffer b = d.alloc(4096ull << (i % 8), false);
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "churn: %s\n", e.what());
            errors++;
        }
    });
    for (auto& th : threads)
        th.join();
    CHECK_EQ(errors.load(), 0);
}

// ---- runner ----------------------------------------------------------------

int main(int argc, char** argv)
{
    std::string filter = argc > 1 ? argv[1] : "";
    if (filter == "--list") {
        for (auto& t : registry())
            printf("%s\n", t.name);
        return 0;
    }
    if (access(kDev, R_OK | W_OK) != 0) {
        fprintf(stderr, "cannot open %s: %s (is the module loaded? are you root?)\n",
                kDev, strerror(errno));
        return 2;
    }

    int passed = 0, failed = 0;
    for (auto& t : registry()) {
        if (!filter.empty() && std::string(t.name).find(filter) == std::string::npos)
            continue;
        printf("[ RUN    ] %s\n", t.name);
        fflush(stdout);
        auto t0 = Clock::now();
        std::string err;
        try {
            t.fn();
            // Leak check: everything the test created must be gone.
            if (!eventually([] { return sysfs("vram_used_kb") == 0 && sysfs("channels_used") == 0; }))
                err = "leak: vram_used_kb=" + std::to_string(sysfs("vram_used_kb")) +
                      " channels_used=" + std::to_string(sysfs("channels_used"));
        } catch (const Failure& f) {
            err = f.msg;
        } catch (const std::exception& e) {
            err = std::string("unexpected exception: ") + e.what();
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (err.empty()) {
            printf("[     OK ] %s (%lld ms)\n", t.name, (long long)ms);
            passed++;
        } else {
            printf("[ FAILED ] %s\n           %s\n", t.name, err.c_str());
            failed++;
        }
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
