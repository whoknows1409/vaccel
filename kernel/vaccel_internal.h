/* SPDX-License-Identifier: GPL-2.0 */
/*
 * vaccel: internal driver structures.
 *
 * Lifetimes are reference counted with kref:
 *
 *   file ──► client ◄── channel ◄── job (while queued/executing)
 *                          │
 *   handle table ──► buffer ◄── job command, mmap()ed VMA
 *
 * A buffer's device memory goes back to the buddy allocator only when the
 * last reference is dropped, so freeing a handle while a job or a user
 * mapping still uses the memory can never lead to a use-after-free or to
 * another client seeing the pages.
 *
 * Lock order (outer → inner):
 *   client->lock  →  vdev->mem_lock
 *   vdev->sched_lock and client->ev_lock are spinlocks, never held while
 *   sleeping and never held while dropping a kref (release may sleep).
 */
#ifndef VACCEL_INTERNAL_H
#define VACCEL_INTERNAL_H

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/idr.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/xarray.h>

#include "../include/uapi/vaccel.h"
#include "vaccel_buddy.h"

#define VACCEL_NAME          "vaccel"
#define VACCEL_MAX_HANDLES   (1u << 16)
#define VACCEL_EVENT_FIFO    256          /* per client, power of two */

/* Module parameters (vaccel_main.c). */
extern unsigned int vaccel_max_queue_depth;
extern unsigned int vaccel_sched_quantum;
extern unsigned int vaccel_client_quota_mb;

struct vaccel_dev {
	dev_t devt;
	struct cdev cdev;
	struct device *device;
	struct dentry *debugfs;

	/* Device memory ("VRAM"): one vmalloc_user() area managed by a buddy allocator. */
	void *vram;
	u64 vram_size;
	struct mutex mem_lock;          /* protects buddy and vram_used */
	struct vbuddy buddy;
	struct vbuddy_page *buddy_pages;
	u64 vram_used;

	/* Hardware channel slots. */
	struct ida chan_ida;
	u32 max_channels;
	atomic_t channels_used;

	/* Scheduler state, shared by ioctl submitters and the engine thread. */
	spinlock_t sched_lock;
	struct list_head runlist;       /* channels with queued jobs, round-robin */
	wait_queue_head_t engine_wq;
	struct task_struct *engine;

	/* Clients, for debugfs. */
	struct mutex clients_lock;
	struct list_head clients;

	/* Statistics. */
	atomic64_t jobs_submitted;
	atomic64_t jobs_completed;
	atomic64_t jobs_cancelled;
	atomic64_t bytes_processed;
	atomic64_t busy_ns;
};

struct vaccel_client {
	struct kref ref;
	struct vaccel_dev *vdev;

	struct mutex lock;              /* protects the handle tables and usage counters */
	struct xarray bufs;             /* handle -> struct vaccel_buf * */
	struct xarray chans;            /* handle -> struct vaccel_chan * */
	u32 next_buf_handle;
	u32 next_chan_handle;
	u64 vram_used;
	u32 nr_channels;

	pid_t pid;
	char comm[TASK_COMM_LEN];
	struct list_head node;          /* in vdev->clients */

	/* Completion events delivered through read()/poll(). */
	spinlock_t ev_lock;
	struct mutex read_lock;         /* serialises readers */
	DECLARE_KFIFO(events, struct vaccel_event, VACCEL_EVENT_FIFO);
	u64 events_dropped;
	bool closed;
	wait_queue_head_t ev_wq;
};

struct vaccel_buf {
	struct kref ref;
	struct vaccel_dev *vdev;
	u32 first_page;                 /* page index in device memory */
	u32 order;
	u64 size;                       /* PAGE_SIZE << order */
	atomic_t map_count;
};

struct vaccel_chan {
	struct kref ref;
	struct vaccel_dev *vdev;
	struct vaccel_client *client;   /* holds a client reference */
	u32 handle;
	u32 hw_id;

	/* Protected by vdev->sched_lock. */
	struct list_head pending;       /* queued jobs, FIFO */
	u32 nr_pending;
	u32 quantum_used;
	bool on_runlist;
	bool dead;
	struct list_head run_node;
	u64 last_seq;                   /* last fence handed out */

	atomic64_t completed_seq;       /* last fence signalled by the engine */
	wait_queue_head_t fence_wq;
};

struct vaccel_kcmd {
	u32 op;
	u32 value;
	struct vaccel_buf *dst, *src0, *src1;   /* referenced while the job lives */
	u64 dst_off, src0_off, src1_off;
	u64 len;
};

struct vaccel_job {
	struct list_head node;
	struct vaccel_chan *chan;
	u64 seq;
	u32 flags;
	u32 ncmds;
	u64 submit_ns;
	struct vaccel_kcmd cmds[];
};

/* vaccel_mem.c */
int  vaccel_mem_init(struct vaccel_dev *vdev, u64 size);
void vaccel_mem_fini(struct vaccel_dev *vdev);
void vaccel_buf_put(struct vaccel_buf *buf);
struct vaccel_buf *vaccel_buf_lookup(struct vaccel_client *client, u32 handle);
static inline void *vaccel_buf_vaddr(struct vaccel_buf *buf)
{
	return (u8 *)buf->vdev->vram + ((u64)buf->first_page << PAGE_SHIFT);
}
int vaccel_ioctl_alloc(struct vaccel_client *client, void __user *arg);
int vaccel_ioctl_free(struct vaccel_client *client, void __user *arg);
int vaccel_mmap(struct file *file, struct vm_area_struct *vma);
void vaccel_mem_release_client(struct vaccel_client *client);
u64 vaccel_mem_largest_free(struct vaccel_dev *vdev);

/* vaccel_sched.c */
int  vaccel_sched_init(struct vaccel_dev *vdev);
void vaccel_sched_fini(struct vaccel_dev *vdev);
int  vaccel_ioctl_chan_create(struct vaccel_client *client, void __user *arg);
int  vaccel_ioctl_chan_destroy(struct vaccel_client *client, void __user *arg);
int  vaccel_ioctl_submit(struct vaccel_client *client, void __user *arg);
int  vaccel_ioctl_wait(struct vaccel_client *client, void __user *arg);
void vaccel_sched_release_client(struct vaccel_client *client);

/* vaccel_main.c */
void vaccel_client_put(struct vaccel_client *client);
void vaccel_client_push_event(struct vaccel_client *client,
			      const struct vaccel_event *ev);

#endif /* VACCEL_INTERNAL_H */
