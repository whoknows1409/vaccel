/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * vaccel: user-space API (uAPI) of the virtual accelerator driver.
 *
 * This header is shared by the kernel module and user space, so it only
 * uses fixed-width __u32/__u64 types and explicit padding. That keeps the
 * struct layout identical for 32-bit and 64-bit callers and lets the
 * driver use compat_ptr_ioctl() without per-struct translation.
 *
 * Object model (similar in spirit to a GPU resource manager):
 *   - Every open() of /dev/vaccel0 is a *client* with its own handle tables.
 *   - A *buffer* is a block of device memory (VRAM), mmap()-able by its owner.
 *   - A *channel* is a hardware submission queue with its own fence timeline.
 *   - A *job* is a batch of up to VACCEL_MAX_CMDS commands submitted to a
 *     channel. Completing a job signals its *fence* (a 64-bit sequence number).
 */
#ifndef _UAPI_VACCEL_H
#define _UAPI_VACCEL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VACCEL_API_VERSION   1

#define VACCEL_MAX_CMDS      64       /* commands per job */
#define VACCEL_MAX_DELAY_US  100000   /* upper bound for VACCEL_OP_DELAY */

/* Commands executed by the device engine. */
enum vaccel_op {
	VACCEL_OP_NOP    = 0,
	VACCEL_OP_FILL32 = 1,  /* dst[i] = value, len bytes (multiple of 4)        */
	VACCEL_OP_COPY   = 2,  /* memmove(dst, src0, len), overlap-safe              */
	VACCEL_OP_ADD32  = 3,  /* dst[i] = src0[i] + src1[i], u32 wrap-around        */
	VACCEL_OP_DELAY  = 4,  /* busy the engine for `value` microseconds (testing) */
	VACCEL_OP_MAX
};

struct vaccel_cmd {
	__u32 op;        /* enum vaccel_op */
	__u32 value;     /* FILL32 pattern / DELAY microseconds */
	__u32 dst;       /* buffer handles (0 = unused) */
	__u32 src0;
	__u32 src1;
	__u32 pad;       /* must be 0 */
	__u64 dst_off;   /* byte offsets inside each buffer */
	__u64 src0_off;
	__u64 src1_off;
	__u64 len;       /* bytes to process */
};

struct vaccel_info {
	__u32 api_version;
	__u32 page_size;
	__u64 vram_size;          /* total device memory, bytes */
	__u64 vram_free;          /* free device memory, bytes */
	__u64 largest_free_block; /* biggest single allocation that can succeed now */
	__u32 max_channels;
	__u32 free_channels;
	__u64 jobs_completed;
	__u64 client_quota;       /* per-client VRAM limit in bytes, 0 = unlimited */
	__u64 client_used;        /* VRAM held by the calling client's handles */
};

struct vaccel_alloc {
	__u64 size;         /* in:  requested bytes */
	__u32 flags;        /* in:  must be 0 */
	__u32 handle;       /* out: buffer handle */
	__u64 alloc_size;   /* out: bytes actually reserved (power-of-two pages) */
	__u64 mmap_offset;  /* out: offset to pass to mmap() */
	__u64 dev_addr;     /* out: offset of the buffer inside device memory */
};

struct vaccel_handle_arg {
	__u32 handle;
	__u32 pad;          /* must be 0 */
};

struct vaccel_chan_create {
	__u32 flags;        /* in:  must be 0 */
	__u32 handle;       /* out: channel handle */
	__u32 hw_id;        /* out: hardware channel slot */
	__u32 pad;
};

#define VACCEL_SUBMIT_NOTIFY  (1u << 0)  /* queue a vaccel_event for read() */
#define VACCEL_SUBMIT_FLAGS   (VACCEL_SUBMIT_NOTIFY)

struct vaccel_submit {
	__u32 chan;         /* in:  channel handle */
	__u32 flags;        /* in:  VACCEL_SUBMIT_* */
	__u64 cmds;         /* in:  user pointer to struct vaccel_cmd[num_cmds] */
	__u32 num_cmds;     /* in:  1..VACCEL_MAX_CMDS */
	__u32 pad;
	__u64 fence;        /* out: sequence number signalled when the job is done */
};

struct vaccel_wait {
	__u32 chan;
	__u32 pad;
	__u64 fence;
	__s64 timeout_ns;   /* < 0: wait forever, 0: just test, > 0: relative timeout */
};

enum vaccel_status {
	VACCEL_STATUS_OK        = 0,
	VACCEL_STATUS_CANCELLED = 1,  /* channel destroyed before the job ran */
};

/* Records returned by read() on the device fd for VACCEL_SUBMIT_NOTIFY jobs. */
struct vaccel_event {
	__u32 chan;         /* channel handle */
	__u32 status;       /* enum vaccel_status */
	__u64 fence;
	__u64 submit_ns;    /* CLOCK_MONOTONIC timestamps */
	__u64 start_ns;
	__u64 end_ns;
};

/*
 * ioctl numbers are dispatched per file, so a clash with another driver's
 * magic is harmless; 0xAC is used here for "ACcelerator".
 */
#define VACCEL_IOC_MAGIC 0xAC

#define VACCEL_IOCTL_INFO          _IOR(VACCEL_IOC_MAGIC,  0x00, struct vaccel_info)
#define VACCEL_IOCTL_ALLOC         _IOWR(VACCEL_IOC_MAGIC, 0x01, struct vaccel_alloc)
#define VACCEL_IOCTL_FREE          _IOW(VACCEL_IOC_MAGIC,  0x02, struct vaccel_handle_arg)
#define VACCEL_IOCTL_CHAN_CREATE   _IOWR(VACCEL_IOC_MAGIC, 0x03, struct vaccel_chan_create)
#define VACCEL_IOCTL_CHAN_DESTROY  _IOW(VACCEL_IOC_MAGIC,  0x04, struct vaccel_handle_arg)
#define VACCEL_IOCTL_SUBMIT        _IOWR(VACCEL_IOC_MAGIC, 0x05, struct vaccel_submit)
#define VACCEL_IOCTL_WAIT          _IOW(VACCEL_IOC_MAGIC,  0x06, struct vaccel_wait)

#endif /* _UAPI_VACCEL_H */
