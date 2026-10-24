// SPDX-License-Identifier: MIT
//
// vaccelctl: inspect and smoke-test the vaccel device.
//
//   vaccelctl info      device capabilities and live counters
//   vaccelctl clients   per-process usage (debugfs, needs root)
//   vaccelctl buddy     free blocks per order and fragmentation (debugfs)
//   vaccelctl demo      run a vector add on the device and verify it
#include "vaccel/vaccel.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

using namespace vaccel;

static std::string sysfs(const char* attr)
{
    std::ifstream f(std::string("/sys/class/vaccel/vaccel0/") + attr);
    std::string v;
    std::getline(f, v);
    return v.empty() ? "?" : v;
}

static int cat(const char* path)
{
    std::ifstream f(path);
    if (!f) {
        fprintf(stderr, "cannot read %s (mount debugfs and run as root)\n", path);
        return 1;
    }
    std::cout << f.rdbuf();
    return 0;
}

static int cmd_info()
{
    Device d;
    auto i = d.info();
    printf("vaccel0  (uAPI v%u, page %u B)\n", i.api_version, i.page_size);
    printf("  device memory   %8llu KiB total, %8llu KiB free, largest block %llu KiB\n",
           (unsigned long long)(i.vram_size >> 10), (unsigned long long)(i.vram_free >> 10),
           (unsigned long long)(i.largest_free_block >> 10));
    printf("  channels        %8u total, %8u free\n", i.max_channels, i.free_channels);
    printf("  client quota    %s\n",
           i.client_quota ? (std::to_string(i.client_quota >> 20) + " MiB").c_str() : "unlimited");
    printf("  jobs            %s submitted, %s completed, %s cancelled\n",
           sysfs("jobs_submitted").c_str(), sysfs("jobs_completed").c_str(),
           sysfs("jobs_cancelled").c_str());
    printf("  engine          %s ms busy, %s bytes processed\n",
           sysfs("engine_busy_ms").c_str(), sysfs("bytes_processed").c_str());
    return 0;
}

static int cmd_demo()
{
    Device d;
    Channel ch = d.create_channel();
    const uint64_t n = 1 << 20;
    Buffer a = d.alloc(n), b = d.alloc(n), c = d.alloc(n);
    for (uint64_t i = 0; i < n / 4; i++) {
        a.as<uint32_t>()[i] = (uint32_t)i;          // written by the CPU through mmap
        b.as<uint32_t>()[i] = (uint32_t)(3 * i);
    }
    Fence f = ch.submit(CommandBatch().add32(c, 0, a, 0, b, 0, n), /*notify=*/true);
    ch.wait(f);
    for (uint64_t i = 0; i < n / 4; i++)
        if (c.as<uint32_t>()[i] != (uint32_t)(4 * i)) {
            fprintf(stderr, "mismatch at %llu\n", (unsigned long long)i);
            return 1;
        }
    auto ev = d.read_events();
    printf("demo: c = a + b over %llu KiB on hw channel %u, fence %llu: OK\n",
           (unsigned long long)(n >> 10), ch.hw_id(), (unsigned long long)f.seq);
    if (!ev.empty())
        printf("      queued %.1f us, ran %.1f us\n",
               (ev[0].start_ns - ev[0].submit_ns) / 1e3, (ev[0].end_ns - ev[0].start_ns) / 1e3);
    return 0;
}

int main(int argc, char** argv)
{
    std::string cmd = argc > 1 ? argv[1] : "info";
    try {
        if (cmd == "info")
            return cmd_info();
        if (cmd == "clients")
            return cat("/sys/kernel/debug/vaccel/clients");
        if (cmd == "buddy")
            return cat("/sys/kernel/debug/vaccel/buddyinfo");
        if (cmd == "demo")
            return cmd_demo();
    } catch (const Error& e) {
        fprintf(stderr, "vaccelctl: %s\n", e.what());
        return 1;
    }
    fprintf(stderr, "usage: vaccelctl [info|clients|buddy|demo]\n");
    return 2;
}
