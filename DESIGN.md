# vaccel design

This document explains how the driver works and why it is built this way. Read it alongside the code; every section names the file and function that implements it.

## 1. What is simulated and what is real

There is no hardware. Two things stand in for it:

| Real accelerator | vaccel | Where |
|---|---|---|
| On-board memory (VRAM) | One `vmalloc_user()` area, 64 MiB by default | `vaccel_mem_init()` |
| Execution engine (copy/compute unit) | One kernel thread, `vaccel-engine` | `vaccel_engine_fn()` |

Everything else is the same work a real driver does: the character device, the ioctl uAPI, per-process handle tables, the memory allocator, the channel pool, the job scheduler, fences, `mmap`, completion events, cleanup when a process dies, and sysfs/debugfs reporting. On real hardware the engine thread would be replaced by writing to a hardware ring buffer and taking an interrupt on completion; the resource-management layer above it would stay the same.

## 2. Architecture

```
 user space                         kernel (vaccel.ko)
 ───────────                        ─────────────────────────────────────────────────────────
                                    vaccel_main.c       vaccel_mem.c          vaccel_sched.c
 libvaccel (C++ RAII)
   Device  ── open/close ─────────► client (per fd) ──► buffer handles ─┐    channel handles
   Buffer  ── ALLOC/FREE, mmap ───►                     (xarray)       │    (xarray)
   Channel ── CHAN_CREATE/DESTROY ►                                     │       │
           ── SUBMIT ─────────────►  validate cmds, take buffer refs ───┘       ▼
           ── WAIT (fence) ───────►                                  channel FIFO ─► runlist
   events  ── read()/poll() ◄──────  per-client event kfifo ◄──┐                      │
                                                               │       engine kthread ◄┘
 sysfs   /sys/class/vaccel/vaccel0/*   (counters)               └─── run job, signal fence
 debugfs /sys/kernel/debug/vaccel/{clients,buddyinfo}

 device memory: vmalloc_user() area ── managed by ──► buddy allocator (vaccel_buddy.c)
 hardware channel slots: IDA (0 .. max_channels-1)
```

## 3. Object model and the uAPI

`include/uapi/vaccel.h` is the only contract between user space and the kernel.

- **Client**: one per `open()` of `/dev/vaccel0`. It owns two handle tables (buffers and channels). Handles are small integers that mean nothing outside that client, so one process cannot name another process's objects (`clients_are_isolated` test).
- **Buffer**: a block of device memory. `VACCEL_IOCTL_ALLOC` returns a handle, the real size, and an `mmap_offset` (`handle << PAGE_SHIFT`). User space maps the buffer with `mmap(fd, offset)`. This "fake offset" scheme is the same idea DRM drivers use.
- **Channel**: a hardware submission queue. There are `max_channels` slots (32 by default); `CHAN_CREATE` fails with `EBUSY` when all are taken.
- **Job**: up to 64 commands submitted together with `VACCEL_IOCTL_SUBMIT`. Commands are `FILL32`, `COPY`, `ADD32`, `DELAY` (for tests) and `NOP`.
- **Fence**: a per-channel 64-bit sequence number. Job *N* on a channel gets fence *N*. Since a channel runs its jobs in order, "fence F has signalled" is just `completed_seq >= F`. `VACCEL_IOCTL_WAIT` blocks on it, with a timeout.
- **Event**: if a job is submitted with `VACCEL_SUBMIT_NOTIFY`, the driver queues a `struct vaccel_event` (fence, status, submit/start/end timestamps) that the client reads with `read()` and can wait for with `poll()`/`epoll`.

uAPI rules followed:

- Only fixed-width types (`__u32`, `__u64`) and explicit padding, so 32-bit and 64-bit processes see the same layout and `compat_ptr_ioctl` works with no translation.
- User pointers travel as `__u64` (`vaccel_submit.cmds`), never as C pointers in the struct.
- `flags` and `pad` fields must be zero. Unknown bits are rejected with `EINVAL`, so they can be given a meaning later without breaking old binaries.
- `_IOR`/`_IOW`/`_IOWR` encode direction and size, so a mismatched struct size fails with `ENOTTY` instead of corrupting memory.

## 4. Device memory: the buddy allocator

`vaccel_buddy.c` manages device memory in pages. A block of order *k* is 2^k pages and starts at an address that is a multiple of 2^k pages.

- **Alloc**: round the request up to 2^k pages, take the smallest free block of order ≥ k, and split it in half repeatedly, putting the unused halves on the free lists.
- **Free**: the buddy of block `p` at order `k` is `p XOR 2^k`. If the buddy is free and has the same order, remove it from its free list, merge, and repeat one order up.
- Both operations are O(max_order): at most 14 splits or merges for 64 MiB (16,384 pages = order 14).

Design choices:

- **Metadata outside the managed memory.** Free-list links live in a separate per-page array, linked by index rather than pointer. Device memory never stores allocator data, so a client writing through `mmap` cannot corrupt the allocator.
- **No kernel dependencies.** The file includes only `<linux/types.h>` in the kernel and `<stdint.h>` in user space. The same source is compiled into `user/build/buddy_fuzz`, which runs millions of random alloc/free operations under AddressSanitizer and UBSan, checks every page against a shadow ownership map, and calls `vbuddy_check()`, which verifies alignment, exact tiling, free-list links, counters, and that no two free buddies were left unmerged.
- **Non-power-of-two sizes work.** `vbuddy_init()` cuts the area into the largest aligned blocks that fit (48 MiB becomes 32 + 16). This is tested by the second QEMU pass with `vram_mb=48`.
- **Trade-off.** Rounding up to a power of two wastes up to almost half of a block (internal fragmentation). In exchange, merging is cheap and external fragmentation stays low. `debugfs/vaccel/buddyinfo` prints free blocks per order and an external-fragmentation percentage, and the `buddy_exhaustion_fragmentation_and_coalescing` test shows both effects.
- **Zeroing.** Every new allocation is zeroed before its handle is returned, so a client never sees data left behind by another client (`memory_is_zeroed_between_owners`).

## 5. Lifetimes: reference counting

Every object that can be reached from more than one place is reference counted with `struct kref`:

```
file ──► client ◄── channel ◄── job being executed (engine holds a channel ref)
                                     │
handle table ──► buffer ◄──── job commands, mmap()ed VMAs
```

| Reference holder | Takes | Drops |
|---|---|---|
| Handle table | buffer, channel | `FREE`, `CHAN_DESTROY`, fd close |
| Queued/running job | each buffer it reads or writes | job freed after completion or cancellation |
| Engine (while executing) | the job's channel | after signalling the fence |
| Channel | its client | channel release |
| VMA (`mmap`) | the buffer | `vm_ops->close` (munmap, exit). `vm_ops->open` takes another on fork and VMA split |

Consequences that the tests check:

- `FREE` on a buffer that a queued job still uses only drops the handle; the memory goes back to the allocator when the job finishes.
- `FREE` on a buffer that is still mapped keeps the pages reserved and readable until `munmap` (`free_while_mapped_keeps_memory_alive`). Without this, the pages could be handed to another process while the first one can still write to them.
- After `fork()` both processes share the mapping and the refcount covers both (`fork_shares_mapping_and_refcounts_it`).
- A hardware channel ID returns to the pool only when the channel's last reference is dropped, so it is never reused while the engine is still running a job on it.
- `rmmod` is refused while any fd or mapping exists, because the file operations' `owner` pins the module and every VMA holds a file reference.

## 6. Locking

| Lock | Type | Protects |
|---|---|---|
| `vdev->mem_lock` | mutex | buddy allocator, `vram_used` |
| `vdev->sched_lock` | spinlock | runlist, each channel's pending FIFO, `last_seq`, `dead` |
| `client->lock` | mutex | handle-table changes, per-client usage and quota |
| `xa_lock` (inside each xarray) | spinlock | handle lookups that take a reference |
| `client->ev_lock` | spinlock | the event kfifo |
| `client->read_lock` | mutex | serialises concurrent `read()` calls |
| `vdev->clients_lock` | mutex | the client list used by debugfs |

**Lock order:** `client->lock` → `mem_lock`, and `client->lock` → `sched_lock` / `ev_lock`. Spinlocks are innermost and nothing sleeps under them.

Rules that avoid real deadlocks and sleeping-in-atomic bugs:

1. **No `copy_to_user`/`copy_from_user` while holding `client->lock` or `mem_lock`.** A user copy can page-fault and take the process's `mmap_lock`. The mmap path already holds `mmap_lock` and the VMA close path takes `mem_lock`, so faulting under `mem_lock` could deadlock (an ABBA inversion). That is why submit copies the whole command array first (`memdup_user`) and why ALLOC copies its result out after dropping its locks.
2. **Lookups use the xarray lock, not `client->lock`.** `vaccel_buf_lookup()` takes the reference under `xa_lock`. Removal (`xa_erase`) also takes `xa_lock`, so the handle table's reference is guaranteed to still be there while we look. The mmap path therefore never touches `client->lock`.
3. **Never drop a kref under a spinlock.** Releasing a buffer takes `mem_lock` (a mutex, which can sleep). Channel teardown therefore moves the cancelled jobs to a local list under `sched_lock` and frees them after unlocking.
4. **Read shared values inside the lock that protects them.** `SUBMIT` copies the new fence number into a local variable before `spin_unlock`. After the unlock the engine may already have run and freed the job.

The QEMU test can run on a kernel built with lockdep (`CONFIG_PROVE_LOCKING`), `CONFIG_DEBUG_ATOMIC_SLEEP`, KASAN and kmemleak. Lockdep records every lock ordering it sees, including the `mmap_lock` taken by page faults, and reports an inversion even if it never actually deadlocked during the run. `DEBUG_ATOMIC_SLEEP` reports any sleep under a spinlock (rule 3), KASAN reports use-after-free (rule 4), and kmemleak reports objects that were never freed.

## 7. Scheduling

The engine thread (`vaccel_engine_fn`) sleeps on `engine_wq` until a channel has work.

- Channels with pending jobs sit on `vdev->runlist`. The engine takes the next job from the channel at the head. After `sched_quantum` jobs (4 by default, runtime-tunable through `/sys/module/vaccel/parameters/sched_quantum`) the channel moves to the tail. A channel whose queue empties leaves the list, and rejoins at the tail on its next submit.
- This is round-robin with a quantum measured in jobs. One client flooding its queue cannot starve the others: in `round_robin_prevents_starvation`, a second channel's job waits behind at most the hog's running job plus one quantum (1–5 jobs across the test runs) instead of the dozens of hog jobs still queued, which it overtakes.
- **Back-pressure:** each channel holds at most `max_queue_depth` pending jobs (256 by default). Beyond that `SUBMIT` returns `EAGAIN` instead of letting one client consume unbounded kernel memory.
- Long commands call `cond_resched()` every 64 KiB so the engine never monopolises a CPU on a non-preemptible kernel.
- The engine waits with `wait_event_interruptible`. Kernel threads receive no signals, so this only wakes for work or `kthread_stop()`, but an idle engine then sleeps in `S` state instead of `D` and does not trigger the hung-task detector.

**Overlapping copies.** `COPY` must behave like `memmove`. Because it copies in 64 KiB chunks, when the destination overlaps the source from above it walks the chunks from the end. Walking forwards there would overwrite source bytes before a later chunk reads them. `copy_overlap_matches_memmove` compares both directions against libc's `memmove` with a 300 KB copy that spans several chunks.

## 8. Fences and memory ordering

- The engine publishes completion with `atomic64_set_release(&ch->completed_seq, seq)` and then `wake_up_all(&ch->fence_wq)`.
- Waiters test `atomic64_read_acquire(&ch->completed_seq) >= fence`.
- The release/acquire pair guarantees that once a waiter sees the fence signalled, it also sees every byte the engine wrote to device memory for that job. User space can then read results through its mapping with no extra barrier.
- Each channel has its own wait queue, so a completion wakes only the threads waiting on that channel.

WAIT semantics:

| Case | Result |
|---|---|
| Fence already signalled | `0` immediately |
| `timeout_ns == 0` and not signalled | `ETIME` (a non-blocking test) |
| Fence greater than the last one issued | `EINVAL`, since waiting would never end |
| Timeout expires | `ETIME` |
| Channel destroyed while waiting | `ECANCELED` (`destroy_channel_wakes_waiters`) |
| Signal, infinite wait | `ERESTARTSYS`, so the kernel transparently restarts the call |
| Signal, finite timeout | `EINTR`, because restarting would start the full timeout again |

## 9. mmap

`vaccel_mmap()` maps a buffer with `remap_vmalloc_range()`, which inserts every page up front, so there is no fault handler.

- `MAP_SHARED` is required. A private mapping would copy pages on write and silently stop reflecting device memory.
- `PROT_EXEC` is refused and `VM_MAYEXEC` is cleared, so a later `mprotect(PROT_EXEC)` also fails. Device memory is data, never code.
- The mapping may be smaller than the buffer, never larger.
- An unknown or foreign handle gives `EINVAL`.

## 10. Validation of untrusted input

Every ioctl argument comes from an untrusted process. `vaccel_validate_cmd()` and `vaccel_get_operand()` check, per command:

- the opcode is known and `pad` is zero;
- every handle exists **in the caller's own table** (`ENOENT` otherwise);
- `offset + len` does not overflow (`check_add_overflow`) and fits inside the buffer;
- 32-bit operations are 4-byte aligned in offset and length;
- `len` is non-zero and `DELAY` is at most 100 ms.

The whole job is rejected if any command is bad, so nothing runs partially. Error paths release every reference taken so far. The test runner's leak check after `submit_validation` would fail if any error path forgot one.

Structs copied back to user space are zeroed first (`memset` in `vaccel_ioctl_info`), so uninitialised kernel stack bytes never leak through padding.

## 11. Cleanup when a process exits or crashes

`vaccel_release()` runs on the last close of the fd. That includes the implicit close when a process is killed, and it happens only after every mapping of that fd is gone.

1. Mark the client closed, so new events are dropped.
2. Destroy every channel: mark it dead, cancel queued jobs (emitting `CANCELLED` events), and wake fence waiters.
3. Drop every buffer handle.
4. Drop the client's own reference. A job already running finishes, then releases the channel, which releases the client.

`killed_process_resources_are_reclaimed` SIGKILLs a child with 4 MiB allocated and 20 jobs queued, then checks that all memory and channels return. `close_with_pending_work_cancels_and_reclaims` does the same with a plain `close()`.

## 12. Observability

- `/sys/class/vaccel/vaccel0/`: `vram_total_kb`, `vram_used_kb`, `vram_largest_free_kb`, `channels_max`, `channels_used`, `jobs_submitted`, `jobs_completed`, `jobs_cancelled`, `bytes_processed`, `engine_busy_ms` (one value per file, as sysfs expects).
- `/sys/kernel/debug/vaccel/clients`: PID, command, memory and channels per open fd.
- `/sys/kernel/debug/vaccel/buddyinfo`: free blocks per order and the fragmentation percentage.
- `/sys/module/vaccel/parameters/`: `max_queue_depth`, `sched_quantum` and `client_quota_mb` are writable at runtime; `vram_mb` and `max_channels` are fixed at load.

## 13. Testing

| Layer | What | How |
|---|---|---|
| Allocator | 5.3 M random operations over 8 memory sizes | `user/build/buddy_fuzz` with ASan + UBSan, shadow ownership map, full invariant check |
| Driver | 23 functional tests: allocation, mmap rules, isolation, quotas, validation, fences, ordering, events, back-pressure, fairness, crash cleanup, concurrency | `user/build/test_vaccel`, with a leak check after every test |
| Kernel | The whole suite twice (two parameter sets), module reload, then a scan of the kernel log | `scripts/qemu-test.sh`, which boots a real kernel in QEMU |
| Static | No warnings | `make W=1`, `sparse` (`C=2`), `checkpatch.pl` (only the expected version-compatibility notes) |

## 14. Known limitations and what a real driver would add

- **One engine.** A real GPU has several engines (graphics, compute, copy) and the scheduler would pick an engine per job type.
- **No preemption or hang recovery.** A running job always finishes. A real driver uses a timeout per job (DRM's `drm_sched` has one) and resets the engine if it hangs.
- **No priorities.** All channels get equal turns.
- **No real DMA, interrupts, or MMIO.** On hardware, submission would write commands into a ring buffer, ring a doorbell register, and complete through an interrupt handler. Device memory would be mapped with `io_remap_pfn_range` and accessed with DMA through the IOMMU.
- **No buffer sharing between processes** (DRM uses dma-buf for this) and no suspend/resume support.
- **One device instance.**
