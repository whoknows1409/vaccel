// SPDX-License-Identifier: MIT
//
// Multi-client benchmark: N threads, each with its own client and channel,
// keep a window of copy jobs in flight for a fixed time. Reports throughput,
// per-job latency (from the driver's completion events) and Jain's fairness
// index across clients. There is one simulated engine, so throughput is not
// expected to scale with threads; fairness and tail latency are the point.
//
//   ./bench_vaccel [--threads N] [--seconds S] [--size BYTES] [--window W]
#include "vaccel/vaccel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace vaccel;
using Clock = std::chrono::steady_clock;

struct Result {
    uint64_t jobs = 0;
    uint64_t bytes = 0;
    std::vector<uint64_t> latency_ns;
};

static uint64_t sysfs_u64(const char* attr)
{
    std::ifstream f(std::string("/sys/class/vaccel/vaccel0/") + attr);
    uint64_t v = 0;
    f >> v;
    return v;
}

static void drain_events(const Device& d, Result& r)
{
    for (;;) {
        auto ev = d.read_events(128);
        if (ev.empty())
            return;
        for (auto& e : ev)
            r.latency_ns.push_back(e.end_ns - e.submit_ns);
    }
}

static void worker(uint64_t size, int window, std::atomic<bool>& stop, Result& r)
{
    Device d("/dev/vaccel0", /*nonblock=*/true);
    Channel ch = d.create_channel();
    Buffer src = d.alloc(size, false), dst = d.alloc(size, false);
    CommandBatch batch;
    batch.copy(dst, 0, src, 0, size);

    std::deque<Fence> inflight;
    while (!stop.load(std::memory_order_relaxed)) {
        while ((int)inflight.size() < window)
            inflight.push_back(ch.submit(batch, /*notify=*/true));
        ch.wait(inflight.front());
        inflight.pop_front();
        r.jobs++;
        r.bytes += size;
        drain_events(d, r);
    }
    while (!inflight.empty()) {
        ch.wait(inflight.front());
        inflight.pop_front();
        r.jobs++;
        r.bytes += size;
    }
    drain_events(d, r);
}

static uint64_t pct(std::vector<uint64_t>& v, double p)
{
    if (v.empty())
        return 0;
    size_t i = std::min(v.size() - 1, (size_t)(p / 100.0 * (double)v.size()));
    std::nth_element(v.begin(), v.begin() + (long)i, v.end());
    return v[i];
}

int main(int argc, char** argv)
{
    int threads = 4, window = 4;
    double seconds = 3;
    uint64_t size = 256 * 1024;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--threads") threads = atoi(argv[i + 1]);
        else if (a == "--seconds") seconds = atof(argv[i + 1]);
        else if (a == "--size") size = strtoull(argv[i + 1], nullptr, 0);
        else if (a == "--window") window = atoi(argv[i + 1]);
        else {
            fprintf(stderr, "usage: %s [--threads N] [--seconds S] [--size BYTES] [--window W]\n", argv[0]);
            return 2;
        }
    }

    std::vector<Result> res(threads);
    std::atomic<bool> stop{false};
    const uint64_t busy0 = sysfs_u64("engine_busy_ms");
    auto t0 = Clock::now();
    std::vector<std::thread> th;
    try {
        for (int t = 0; t < threads; t++)
            th.emplace_back(worker, size, window, std::ref(stop), std::ref(res[t]));
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        stop = true;
        for (auto& x : th)
            x.join();
    } catch (const std::exception& e) {
        fprintf(stderr, "bench failed: %s\n", e.what());
        return 1;
    }
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    const uint64_t busy = sysfs_u64("engine_busy_ms") - busy0;

    uint64_t jobs = 0, bytes = 0;
    double sum = 0, sumsq = 0;
    std::vector<uint64_t> lat;
    for (auto& r : res) {
        jobs += r.jobs;
        bytes += r.bytes;
        sum += (double)r.jobs;
        sumsq += (double)r.jobs * (double)r.jobs;
        lat.insert(lat.end(), r.latency_ns.begin(), r.latency_ns.end());
    }
    const double jain = sumsq > 0 ? sum * sum / (threads * sumsq) : 0;

    printf("vaccel benchmark: %d clients, %llu KiB copy jobs, window %d, %.1f s\n",
           threads, (unsigned long long)(size >> 10), window, wall);
    printf("  throughput      %10.0f jobs/s   %8.1f MiB/s\n", jobs / wall, bytes / wall / (1 << 20));
    printf("  latency p50     %10.1f us\n", pct(lat, 50) / 1e3);
    printf("  latency p99     %10.1f us\n", pct(lat, 99) / 1e3);
    printf("  engine busy     %10.1f %%\n", 100.0 * (double)busy / (wall * 1000));
    printf("  fairness (Jain) %10.3f   (1.000 = perfectly even)\n", jain);
    for (int t = 0; t < threads; t++)
        printf("    client %-2d %8llu jobs\n", t, (unsigned long long)res[t].jobs);
    return 0;
}
