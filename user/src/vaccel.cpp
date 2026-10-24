// SPDX-License-Identifier: MIT
#include "vaccel/vaccel.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace vaccel {

namespace {

// ioctl() that retries on EINTR and throws on any other failure.
int xioctl(int fd, unsigned long req, void* arg, const char* what, bool throw_on_error = true)
{
    int r;
    do {
        r = ::ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    if (r < 0 && throw_on_error)
        throw Error(errno, what);
    return r < 0 ? -errno : r;
}

}  // namespace

detail::Fd::~Fd()
{
    if (fd >= 0)
        ::close(fd);
}

// ---- Buffer ---------------------------------------------------------------

Buffer::~Buffer() { reset(); }

Buffer::Buffer(Buffer&& o) noexcept
    : fd_(std::move(o.fd_)), handle_(o.handle_), size_(o.size_),
      dev_addr_(o.dev_addr_), map_(o.map_)
{
    o.handle_ = 0;
    o.map_ = nullptr;
}

Buffer& Buffer::operator=(Buffer&& o) noexcept
{
    if (this != &o) {
        reset();
        fd_ = std::move(o.fd_);
        handle_ = o.handle_;
        size_ = o.size_;
        dev_addr_ = o.dev_addr_;
        map_ = o.map_;
        o.handle_ = 0;
        o.map_ = nullptr;
    }
    return *this;
}

void Buffer::reset() noexcept
{
    if (map_) {
        ::munmap(map_, size_);
        map_ = nullptr;
    }
    if (handle_ && fd_) {
        vaccel_handle_arg a{handle_, 0};
        xioctl(fd_->fd, VACCEL_IOCTL_FREE, &a, "free", false);
    }
    handle_ = 0;
    fd_.reset();
}

// ---- CommandBatch ---------------------------------------------------------

CommandBatch& CommandBatch::fill32(const Buffer& dst, uint64_t off, uint64_t len, uint32_t value)
{
    vaccel_cmd c{};
    c.op = VACCEL_OP_FILL32;
    c.dst = dst.handle();
    c.dst_off = off;
    c.len = len;
    c.value = value;
    cmds_.push_back(c);
    return *this;
}

CommandBatch& CommandBatch::copy(const Buffer& dst, uint64_t dst_off,
                                 const Buffer& src, uint64_t src_off, uint64_t len)
{
    vaccel_cmd c{};
    c.op = VACCEL_OP_COPY;
    c.dst = dst.handle();
    c.dst_off = dst_off;
    c.src0 = src.handle();
    c.src0_off = src_off;
    c.len = len;
    cmds_.push_back(c);
    return *this;
}

CommandBatch& CommandBatch::add32(const Buffer& dst, uint64_t dst_off,
                                  const Buffer& a, uint64_t a_off,
                                  const Buffer& b, uint64_t b_off, uint64_t len)
{
    vaccel_cmd c{};
    c.op = VACCEL_OP_ADD32;
    c.dst = dst.handle();
    c.dst_off = dst_off;
    c.src0 = a.handle();
    c.src0_off = a_off;
    c.src1 = b.handle();
    c.src1_off = b_off;
    c.len = len;
    cmds_.push_back(c);
    return *this;
}

CommandBatch& CommandBatch::delay(uint32_t usec)
{
    vaccel_cmd c{};
    c.op = VACCEL_OP_DELAY;
    c.value = usec;
    cmds_.push_back(c);
    return *this;
}

CommandBatch& CommandBatch::raw(const vaccel_cmd& cmd)
{
    cmds_.push_back(cmd);
    return *this;
}

// ---- Channel --------------------------------------------------------------

Channel::~Channel() { reset(); }

Channel::Channel(Channel&& o) noexcept
    : fd_(std::move(o.fd_)), handle_(o.handle_), hw_id_(o.hw_id_)
{
    o.handle_ = 0;
}

Channel& Channel::operator=(Channel&& o) noexcept
{
    if (this != &o) {
        reset();
        fd_ = std::move(o.fd_);
        handle_ = o.handle_;
        hw_id_ = o.hw_id_;
        o.handle_ = 0;
    }
    return *this;
}

void Channel::reset() noexcept
{
    if (handle_ && fd_) {
        vaccel_handle_arg a{handle_, 0};
        xioctl(fd_->fd, VACCEL_IOCTL_CHAN_DESTROY, &a, "chan_destroy", false);
    }
    handle_ = 0;
    fd_.reset();
}

Fence Channel::submit(const CommandBatch& batch, bool notify)
{
    vaccel_submit s{};
    s.chan = handle_;
    s.flags = notify ? VACCEL_SUBMIT_NOTIFY : 0;
    s.cmds = reinterpret_cast<uintptr_t>(batch.cmds().data());
    s.num_cmds = static_cast<uint32_t>(batch.size());
    xioctl(fd_->fd, VACCEL_IOCTL_SUBMIT, &s, "submit");
    return Fence{handle_, s.fence};
}

bool Channel::wait(const Fence& f, std::chrono::nanoseconds timeout)
{
    vaccel_wait w{};
    w.chan = f.chan;
    w.fence = f.seq;
    w.timeout_ns = timeout.count();
    int r = xioctl(fd_->fd, VACCEL_IOCTL_WAIT, &w, "wait", false);
    if (r == -ETIME)
        return false;
    if (r < 0)
        throw Error(-r, "wait");
    return true;
}

// ---- Device ---------------------------------------------------------------

Device::Device(const std::string& path, bool nonblock)
{
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | (nonblock ? O_NONBLOCK : 0));
    if (fd < 0)
        throw Error(errno, "open " + path);
    fd_ = std::make_shared<const detail::Fd>(fd);
}

vaccel_info Device::info() const
{
    vaccel_info i{};
    xioctl(fd_->fd, VACCEL_IOCTL_INFO, &i, "info");
    return i;
}

Buffer Device::alloc(uint64_t bytes, bool map) const
{
    vaccel_alloc a{};
    a.size = bytes;
    xioctl(fd_->fd, VACCEL_IOCTL_ALLOC, &a, "alloc");

    Buffer b;
    b.fd_ = fd_;
    b.handle_ = a.handle;
    b.size_ = a.alloc_size;
    b.dev_addr_ = a.dev_addr;
    if (map) {
        void* p = ::mmap(nullptr, a.alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                         fd_->fd, static_cast<off_t>(a.mmap_offset));
        if (p == MAP_FAILED)
            throw Error(errno, "mmap");   // ~Buffer frees the handle
        b.map_ = p;
    }
    return b;
}

Channel Device::create_channel() const
{
    vaccel_chan_create c{};
    xioctl(fd_->fd, VACCEL_IOCTL_CHAN_CREATE, &c, "chan_create");
    Channel ch;
    ch.fd_ = fd_;
    ch.handle_ = c.handle;
    ch.hw_id_ = c.hw_id;
    return ch;
}

bool Device::wait_events(int timeout_ms) const
{
    pollfd p{fd_->fd, POLLIN, 0};
    int r;
    do {
        r = ::poll(&p, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        throw Error(errno, "poll");
    return r > 0 && (p.revents & POLLIN);
}

std::vector<vaccel_event> Device::read_events(size_t max) const
{
    std::vector<vaccel_event> ev(max);
    ssize_t n;
    do {
        n = ::read(fd_->fd, ev.data(), ev.size() * sizeof(vaccel_event));
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN)
            return {};
        throw Error(errno, "read");
    }
    ev.resize(static_cast<size_t>(n) / sizeof(vaccel_event));
    return ev;
}

const char* op_name(uint32_t op)
{
    switch (op) {
    case VACCEL_OP_NOP:    return "nop";
    case VACCEL_OP_FILL32: return "fill32";
    case VACCEL_OP_COPY:   return "copy";
    case VACCEL_OP_ADD32:  return "add32";
    case VACCEL_OP_DELAY:  return "delay";
    default:               return "?";
    }
}

}  // namespace vaccel
