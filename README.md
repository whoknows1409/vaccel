# vaccel: virtual accelerator driver with a hardware resource manager

A Linux kernel character-device driver (C) for a simulated accelerator, plus a C++17 user-space library, CLI, test suite and benchmark. It models the resource-management layer of a GPU driver: device-memory allocation, a pool of hardware channels, a job scheduler, fences, `mmap` of device memory, completion events, per-process isolation and cleanup when a process dies.

There is no physical device. Device memory is a `vmalloc` area and the execution engine is a kernel thread. Everything around them is real kernel code, loaded into a real kernel and tested there. See [DESIGN.md](DESIGN.md) for how it works.

## Features

**Kernel module** (`kernel/`, ~2,000 lines of C)
- `/dev/vaccel0` with `ioctl`, `mmap`, `read`, `poll`, and a versioned uAPI that is 32/64-bit compatible (`include/uapi/vaccel.h`)
- **Device memory manager:** binary buddy allocator (O(log n) alloc/free, buddy coalescing), allocations zeroed between owners, per-client quotas
- **Hardware channel pool:** a fixed number of slots handed out with an IDA; `EBUSY` when exhausted
- **Scheduler:** an engine kthread serving channels round-robin with a tunable quantum, plus per-channel queue limits for back-pressure
- **Fences:** per-channel 64-bit timelines, waits with timeout, release/acquire memory ordering
- **Completion events:** per-client kfifo, delivered through `read()` and `poll()`/`epoll`
- **Safe lifetimes:** `kref` on clients, channels and buffers; buffers stay alive while a job or a mapping uses them; full cleanup on `close()` or process crash
- **Observability:** 10 sysfs counters, debugfs views of per-process usage and allocator fragmentation, runtime-tunable module parameters

**User space** (`user/`, C++17)
- `libvaccel`: RAII wrappers (`Device`, `Buffer`, `Channel`, `CommandBatch`), move-only, exception-safe
- `vaccelctl`: device info, per-client usage, buddy allocator state, and a demo
- `test_vaccel`: 23 functional tests with a leak check after each one
- `bench_vaccel`: multi-client throughput, p50/p99 latency, and Jain's fairness index
- `buddy_fuzz`: the kernel's allocator source compiled into user space and fuzzed under ASan/UBSan

## Layout

```
include/uapi/vaccel.h     ioctl interface shared by kernel and user space
kernel/vaccel_main.c      module init, char device, file operations, sysfs, debugfs
kernel/vaccel_mem.c       device memory: alloc/free ioctls, mmap, buffer lifetimes
kernel/vaccel_sched.c     channels, submission, validation, engine thread, fences
kernel/vaccel_buddy.[ch]  buddy allocator (no kernel dependencies)
user/                     libvaccel, vaccelctl, tests, benchmark, fuzz test
scripts/                  load/unload helpers and the QEMU test harness
DESIGN.md                 architecture, locking, lifetimes, trade-offs
```

## Build and run

Requirements: Linux 5.15 or newer, kernel headers for the running kernel, gcc/g++, make.

```bash
sudo apt install build-essential linux-headers-$(uname -r)   # Ubuntu/Debian

make                     # builds kernel/vaccel.ko and user/build/*
make fuzz                # allocator fuzz test, no root needed

sudo make load           # insmod (optional params: sudo scripts/load.sh vram_mb=128)
./user/build/vaccelctl demo
./user/build/vaccelctl info
sudo ./user/build/test_vaccel          # needs root: it writes module parameters
./user/build/bench_vaccel --threads 4 --seconds 5
sudo ./user/build/vaccelctl buddy
sudo make unload
```

**Secure Boot:** if `insmod` fails with `Key was rejected by service`, the kernel only accepts signed modules. Either sign the module with a Machine Owner Key, or use the QEMU harness below, which never touches your running kernel.

**Run the tests on a throwaway kernel (recommended):**

```bash
sudo apt install qemu-system-x86 busybox-static cpio
KERNEL=/boot/vmlinuz-$(uname -r) KDIR=/lib/modules/$(uname -r)/build make qemu-test
```

The harness boots the kernel in QEMU with a busybox initramfs, loads the module, runs the test suite and benchmark, reloads with different parameters (non-power-of-two memory, 4 channels, quantum 1) and tests again, unloads, and fails if the kernel log contains any warning, lockdep report, KASAN/UBSAN error or kmemleak leak.

## Module parameters

| Parameter | Default | Runtime-writable | Meaning |
|---|---|---|---|
| `vram_mb` | 64 | no | Simulated device memory, 1–1024 MiB |
| `max_channels` | 32 | no | Hardware channel slots |
| `max_queue_depth` | 256 | yes | Pending jobs per channel before `EAGAIN` |
| `sched_quantum` | 4 | yes | Jobs a channel runs before the engine rotates |
| `client_quota_mb` | 0 | yes | Per-client memory limit (0 = unlimited) |

## Using the library

```cpp
#include "vaccel/vaccel.hpp"
using namespace vaccel;

Device dev;                                   // opens /dev/vaccel0
Channel ch = dev.create_channel();
Buffer a = dev.alloc(1 << 20), b = dev.alloc(1 << 20), c = dev.alloc(1 << 20);

for (size_t i = 0; i < a.size() / 4; i++) {   // CPU writes through mmap
    a.as<uint32_t>()[i] = i;
    b.as<uint32_t>()[i] = 2 * i;
}
Fence f = ch.submit(CommandBatch().add32(c, 0, a, 0, b, 0, a.size()));
ch.wait(f);                                   // c[i] == 3 * i, read through mmap
```

## Test results

All results below come from `scripts/qemu-test.sh`. Each kernel runs the full suite twice: once with the defaults (64 MiB, 32 channels, quantum 4) and once with 48 MiB (not a power of two), 4 channels, quantum 1 and queue depth 64, with a module unload and reload in between.

| Kernel | Functional tests | Kernel checks |
|---|---|---|
| Ubuntu 5.15.0-198 (22.04) | 46/46 pass | no warnings |
| Ubuntu 6.8.0-147 (24.04) | 46/46 pass | no warnings |
| Ubuntu 7.0.0-38 (24.04 HWE) | 46/46 pass | no warnings |
| 6.8 debug build: lockdep, KASAN, UBSAN, kmemleak, `DEBUG_ATOMIC_SLEEP`, `DEBUG_LIST` | 46/46 pass | no lockdep, KASAN or UBSAN reports; kmemleak finds no leaks; lockdep still enabled at the end |

Also:
- Builds without warnings at `W=1` against 5.15, 6.8, 6.17 and 7.0; `sparse` reports nothing.
- `buddy_fuzz`: 5.3 million random operations over 8 memory sizes under ASan and UBSan, all invariants hold.
- Fairness: with a 64-job hog on one channel, a job on another channel waited behind 1–5 hog jobs in every run (bound: quantum + 1) and overtook the rest. Jain's fairness index across 4 benchmark clients was 0.96–1.00.

The benchmark runs inside QEMU without hardware virtualisation in these tests, so its absolute throughput and latency are not meaningful. Run `bench_vaccel` on your own machine for real numbers.

## License

Kernel module: GPL-2.0 (required for a Linux kernel module). User-space library and tools: MIT.
