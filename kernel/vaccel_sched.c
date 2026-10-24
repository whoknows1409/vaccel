// SPDX-License-Identifier: GPL-2.0
/*
 * vaccel: channels, job submission, the engine thread and fences.
 *
 * Every channel owns a FIFO of pending jobs and a fence timeline: job N on a
 * channel gets fence N, and the engine signals fences in order, so "fence F
 * is done" is simply completed_seq >= F.
 *
 * Channels with pending work sit on vdev->runlist. The engine thread serves
 * them round-robin: it runs up to `sched_quantum` jobs from the channel at
 * the head, then rotates it to the tail. One client flooding its channel
 * therefore cannot starve the others (see the fairness test).
 *
 * Reference rules:
 *   - A queued job is owned by its channel's pending list and holds no
 *     channel reference; channel teardown cancels queued jobs first.
 *   - When the engine dequeues a job it takes a channel reference, so the
 *     channel (and its client) outlive the job even if the channel is
 *     destroyed or the fd is closed while the job is running.
 *   - Every buffer a job touches is referenced from submit until the job is
 *     freed.
 */
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/kernel.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "vaccel_internal.h"

#define VACCEL_CHUNK (64 * 1024)   /* bytes processed between cond_resched() */

/* ---- channels ------------------------------------------------------------ */

static void vaccel_chan_release(struct kref *ref)
{
	struct vaccel_chan *ch = container_of(ref, struct vaccel_chan, ref);
	struct vaccel_dev *vdev = ch->vdev;

	WARN_ON(!list_empty(&ch->pending));
	/* The hardware slot is reused only after the last job on it finished. */
	ida_free(&vdev->chan_ida, ch->hw_id);
	atomic_dec(&vdev->channels_used);
	vaccel_client_put(ch->client);
	kfree(ch);
}

/* May sleep: never call with a spinlock held. */
static void vaccel_chan_put(struct vaccel_chan *ch)
{
	kref_put(&ch->ref, vaccel_chan_release);
}

static struct vaccel_chan *vaccel_chan_lookup(struct vaccel_client *client, u32 handle)
{
	struct vaccel_chan *ch;

	xa_lock(&client->chans);
	ch = xa_load(&client->chans, handle);
	if (ch)
		kref_get(&ch->ref);
	xa_unlock(&client->chans);
	return ch;
}

static void vaccel_job_free(struct vaccel_job *job)
{
	u32 i;

	for (i = 0; i < job->ncmds; i++) {
		vaccel_buf_put(job->cmds[i].dst);
		vaccel_buf_put(job->cmds[i].src0);
		vaccel_buf_put(job->cmds[i].src1);
	}
	kfree(job);
}

static void vaccel_job_event(struct vaccel_job *job, u32 status, u64 start, u64 end)
{
	struct vaccel_event ev = {
		.chan      = job->chan->handle,
		.status    = status,
		.fence     = job->seq,
		.submit_ns = job->submit_ns,
		.start_ns  = start,
		.end_ns    = end,
	};

	vaccel_client_push_event(job->chan->client, &ev);
}

/*
 * Stop a channel: no new submissions, cancel everything still queued and
 * wake fence waiters. A job the engine already dequeued finishes normally.
 */
static void vaccel_chan_kill(struct vaccel_chan *ch)
{
	struct vaccel_dev *vdev = ch->vdev;
	struct vaccel_job *job, *tmp;
	LIST_HEAD(cancelled);

	spin_lock(&vdev->sched_lock);
	WRITE_ONCE(ch->dead, true);
	if (ch->on_runlist) {
		list_del_init(&ch->run_node);
		ch->on_runlist = false;
	}
	list_splice_init(&ch->pending, &cancelled);
	ch->nr_pending = 0;
	spin_unlock(&vdev->sched_lock);

	/* Outside the spinlock: dropping buffer references may sleep. */
	list_for_each_entry_safe(job, tmp, &cancelled, node) {
		list_del(&job->node);
		if (job->flags & VACCEL_SUBMIT_NOTIFY)
			vaccel_job_event(job, VACCEL_STATUS_CANCELLED, 0, 0);
		atomic64_inc(&vdev->jobs_cancelled);
		vaccel_job_free(job);
	}
	wake_up_all(&ch->fence_wq);
}

int vaccel_ioctl_chan_create(struct vaccel_client *client, void __user *uarg)
{
	struct vaccel_dev *vdev = client->vdev;
	struct vaccel_chan_create req;
	struct vaccel_chan *ch;
	u32 handle;
	int ret;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;
	if (req.flags || req.pad)
		return -EINVAL;

	ch = kzalloc(sizeof(*ch), GFP_KERNEL);
	if (!ch)
		return -ENOMEM;

	ret = ida_alloc_max(&vdev->chan_ida, vdev->max_channels - 1, GFP_KERNEL);
	if (ret < 0) {
		kfree(ch);
		return ret == -ENOSPC ? -EBUSY : ret;   /* all hardware slots in use */
	}
	ch->hw_id = ret;
	atomic_inc(&vdev->channels_used);

	kref_init(&ch->ref);
	ch->vdev = vdev;
	INIT_LIST_HEAD(&ch->pending);
	INIT_LIST_HEAD(&ch->run_node);
	atomic64_set(&ch->completed_seq, 0);
	init_waitqueue_head(&ch->fence_wq);
	kref_get(&client->ref);
	ch->client = client;

	/* Reserve the handle first so ch->handle is set before anyone can look it up. */
	mutex_lock(&client->lock);
	ret = xa_alloc_cyclic(&client->chans, &handle, NULL,
			      XA_LIMIT(1, VACCEL_MAX_HANDLES - 1),
			      &client->next_chan_handle, GFP_KERNEL);
	if (ret >= 0) {
		ch->handle = handle;
		xa_store(&client->chans, handle, ch, GFP_KERNEL);
		client->nr_channels++;
	}
	mutex_unlock(&client->lock);
	if (ret < 0) {
		vaccel_chan_put(ch);
		return ret == -EBUSY ? -ENOSPC : ret;
	}

	req.handle = handle;
	req.hw_id = ch->hw_id;
	if (copy_to_user(uarg, &req, sizeof(req))) {
		mutex_lock(&client->lock);
		if (xa_cmpxchg(&client->chans, handle, ch, NULL, 0) == ch) {
			client->nr_channels--;
			mutex_unlock(&client->lock);
			vaccel_chan_kill(ch);
			vaccel_chan_put(ch);
		} else {
			mutex_unlock(&client->lock);
		}
		return -EFAULT;
	}
	return 0;
}

int vaccel_ioctl_chan_destroy(struct vaccel_client *client, void __user *uarg)
{
	struct vaccel_handle_arg req;
	struct vaccel_chan *ch;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;
	if (req.pad)
		return -EINVAL;

	mutex_lock(&client->lock);
	ch = xa_erase(&client->chans, req.handle);
	if (ch)
		client->nr_channels--;
	mutex_unlock(&client->lock);
	if (!ch)
		return -ENOENT;

	vaccel_chan_kill(ch);
	vaccel_chan_put(ch);   /* the handle table's reference */
	return 0;
}

void vaccel_sched_release_client(struct vaccel_client *client)
{
	struct vaccel_chan *ch;
	unsigned long idx;

	mutex_lock(&client->lock);
	xa_for_each(&client->chans, idx, ch) {
		xa_erase(&client->chans, idx);
		vaccel_chan_kill(ch);
		vaccel_chan_put(ch);
	}
	client->nr_channels = 0;
	mutex_unlock(&client->lock);
	xa_destroy(&client->chans);
}

/* ---- submission ---------------------------------------------------------- */

/*
 * Resolve one buffer operand and check [off, off + len) lies inside it. The
 * reference is stored in *out even on a range error so the caller's normal
 * cleanup path (vaccel_job_free) releases it.
 */
static int vaccel_get_operand(struct vaccel_client *client, u32 handle,
			      u64 off, u64 len, u32 align,
			      struct vaccel_buf **out)
{
	struct vaccel_buf *buf;
	u64 end;

	if ((off | len) & (align - 1))
		return -EINVAL;
	buf = vaccel_buf_lookup(client, handle);
	if (!buf)
		return -ENOENT;
	*out = buf;
	if (check_add_overflow(off, len, &end) || end > buf->size)
		return -EINVAL;
	return 0;
}

static int vaccel_validate_cmd(struct vaccel_client *client,
			       const struct vaccel_cmd *uc,
			       struct vaccel_kcmd *kc)
{
	int ret;

	if (uc->pad)
		return -EINVAL;

	kc->op = uc->op;
	kc->value = uc->value;
	kc->len = uc->len;
	kc->dst_off = uc->dst_off;
	kc->src0_off = uc->src0_off;
	kc->src1_off = uc->src1_off;

	switch (uc->op) {
	case VACCEL_OP_NOP:
		return 0;
	case VACCEL_OP_DELAY:
		return uc->value > VACCEL_MAX_DELAY_US ? -EINVAL : 0;
	case VACCEL_OP_FILL32:
		if (!uc->len)
			return -EINVAL;
		return vaccel_get_operand(client, uc->dst, uc->dst_off, uc->len, 4, &kc->dst);
	case VACCEL_OP_COPY:
		if (!uc->len)
			return -EINVAL;
		ret = vaccel_get_operand(client, uc->dst, uc->dst_off, uc->len, 1, &kc->dst);
		if (ret)
			return ret;
		return vaccel_get_operand(client, uc->src0, uc->src0_off, uc->len, 1, &kc->src0);
	case VACCEL_OP_ADD32:
		if (!uc->len)
			return -EINVAL;
		ret = vaccel_get_operand(client, uc->dst, uc->dst_off, uc->len, 4, &kc->dst);
		if (ret)
			return ret;
		ret = vaccel_get_operand(client, uc->src0, uc->src0_off, uc->len, 4, &kc->src0);
		if (ret)
			return ret;
		return vaccel_get_operand(client, uc->src1, uc->src1_off, uc->len, 4, &kc->src1);
	default:
		return -EINVAL;
	}
}

int vaccel_ioctl_submit(struct vaccel_client *client, void __user *uarg)
{
	struct vaccel_dev *vdev = client->vdev;
	struct vaccel_submit req;
	struct vaccel_cmd *ucmds;
	struct vaccel_chan *ch;
	struct vaccel_job *job;
	u64 seq = 0;
	u32 i;
	int ret;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;
	if ((req.flags & ~VACCEL_SUBMIT_FLAGS) || req.pad)
		return -EINVAL;
	if (req.num_cmds == 0 || req.num_cmds > VACCEL_MAX_CMDS)
		return -EINVAL;

	/* Copy the whole batch once, before taking any lock (it may fault). */
	ucmds = memdup_user(u64_to_user_ptr(req.cmds), req.num_cmds * sizeof(*ucmds));
	if (IS_ERR(ucmds))
		return PTR_ERR(ucmds);

	ch = vaccel_chan_lookup(client, req.chan);
	if (!ch) {
		ret = -ENOENT;
		goto out_cmds;
	}

	job = kzalloc(struct_size(job, cmds, req.num_cmds), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out_chan;
	}
	INIT_LIST_HEAD(&job->node);
	job->chan = ch;
	job->flags = req.flags;
	job->ncmds = req.num_cmds;

	for (i = 0; i < req.num_cmds; i++) {
		ret = vaccel_validate_cmd(client, &ucmds[i], &job->cmds[i]);
		if (ret)
			goto out_job;
	}

	spin_lock(&vdev->sched_lock);
	if (ch->dead) {
		ret = -ENOENT;                          /* destroyed concurrently */
	} else if (ch->nr_pending >= READ_ONCE(vaccel_max_queue_depth)) {
		ret = -EAGAIN;                          /* back-pressure */
	} else {
		seq = ++ch->last_seq;
		job->seq = seq;
		job->submit_ns = ktime_get_ns();
		list_add_tail(&job->node, &ch->pending);
		ch->nr_pending++;
		if (!ch->on_runlist) {
			list_add_tail(&ch->run_node, &vdev->runlist);
			ch->on_runlist = true;
		}
		ret = 0;
	}
	spin_unlock(&vdev->sched_lock);
	if (ret)
		goto out_job;

	/* From here on the job may already be running or even freed: use `seq`. */
	atomic64_inc(&vdev->jobs_submitted);
	wake_up(&vdev->engine_wq);
	vaccel_chan_put(ch);
	kfree(ucmds);

	req.fence = seq;
	if (copy_to_user(uarg, &req, sizeof(req)))
		return -EFAULT;   /* the job still runs; only the fence value is lost */
	return 0;

out_job:
	vaccel_job_free(job);
out_chan:
	vaccel_chan_put(ch);
out_cmds:
	kfree(ucmds);
	return ret;
}

/* ---- fences -------------------------------------------------------------- */

static bool vaccel_fence_signaled(struct vaccel_chan *ch, u64 fence)
{
	/* Acquire pairs with the engine's release: results are visible once true. */
	return (u64)atomic64_read_acquire(&ch->completed_seq) >= fence;
}

int vaccel_ioctl_wait(struct vaccel_client *client, void __user *uarg)
{
	struct vaccel_dev *vdev = client->vdev;
	struct vaccel_wait req;
	struct vaccel_chan *ch;
	u64 last;
	long ret;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;
	if (req.pad)
		return -EINVAL;

	ch = vaccel_chan_lookup(client, req.chan);
	if (!ch)
		return -ENOENT;

	spin_lock(&vdev->sched_lock);
	last = ch->last_seq;
	spin_unlock(&vdev->sched_lock);

	if (req.fence > last) {
		ret = -EINVAL;            /* never issued: waiting would hang forever */
		goto out;
	}
	if (vaccel_fence_signaled(ch, req.fence)) {
		ret = 0;
		goto out;
	}
	if (req.timeout_ns == 0) {
		ret = -ETIME;
		goto out;
	}

	if (req.timeout_ns < 0) {
		ret = wait_event_interruptible(ch->fence_wq,
					       vaccel_fence_signaled(ch, req.fence) ||
					       READ_ONCE(ch->dead));
		if (ret)
			goto out;         /* -ERESTARTSYS: safe to restart, no deadline */
	} else {
		unsigned long timeout = nsecs_to_jiffies((u64)req.timeout_ns);

		timeout = clamp_t(unsigned long, timeout, 1, MAX_SCHEDULE_TIMEOUT - 1);
		ret = wait_event_interruptible_timeout(ch->fence_wq,
						       vaccel_fence_signaled(ch, req.fence) ||
						       READ_ONCE(ch->dead),
						       timeout);
		if (ret == 0) {
			ret = -ETIME;
			goto out;
		}
		if (ret < 0) {
			ret = -EINTR;     /* a restart would extend the timeout */
			goto out;
		}
	}
	ret = vaccel_fence_signaled(ch, req.fence) ? 0 : -ECANCELED;
out:
	vaccel_chan_put(ch);
	return ret;
}

/* ---- engine -------------------------------------------------------------- */

/* Round-robin pick. Returns a job with a channel reference held, or NULL. */
static struct vaccel_job *vaccel_pick_job(struct vaccel_dev *vdev)
{
	struct vaccel_chan *ch;
	struct vaccel_job *job;

	spin_lock(&vdev->sched_lock);
	ch = list_first_entry_or_null(&vdev->runlist, struct vaccel_chan, run_node);
	if (!ch) {
		spin_unlock(&vdev->sched_lock);
		return NULL;
	}
	job = list_first_entry(&ch->pending, struct vaccel_job, node);
	list_del_init(&job->node);
	ch->nr_pending--;
	kref_get(&ch->ref);

	if (ch->nr_pending == 0) {
		list_del_init(&ch->run_node);
		ch->on_runlist = false;
		ch->quantum_used = 0;
	} else if (++ch->quantum_used >= max(1u, READ_ONCE(vaccel_sched_quantum))) {
		list_move_tail(&ch->run_node, &vdev->runlist);   /* time slice used up */
		ch->quantum_used = 0;
	}
	spin_unlock(&vdev->sched_lock);
	return job;
}

static void vaccel_exec_cmd(struct vaccel_dev *vdev, const struct vaccel_kcmd *c)
{
	u64 i, n;

	switch (c->op) {
	case VACCEL_OP_FILL32: {
		u32 *d = (u32 *)((u8 *)vaccel_buf_vaddr(c->dst) + c->dst_off);

		n = c->len / 4;
		for (i = 0; i < n; i++) {
			d[i] = c->value;
			if ((i & 0x3fff) == 0x3fff)
				cond_resched();
		}
		break;
	}
	case VACCEL_OP_COPY: {
		u8 *d = (u8 *)vaccel_buf_vaddr(c->dst) + c->dst_off;
		u8 *s = (u8 *)vaccel_buf_vaddr(c->src0) + c->src0_off;
		u64 done = 0, chunk;

		/*
		 * Copy in chunks so a large copy cannot hog the CPU. If the
		 * destination overlaps the source from above, go backwards,
		 * otherwise an early chunk would overwrite source bytes that a
		 * later chunk still has to read.
		 */
		if (d > s && d < s + c->len) {
			while (done < c->len) {
				chunk = min_t(u64, c->len - done, VACCEL_CHUNK);
				done += chunk;
				memmove(d + c->len - done, s + c->len - done, chunk);
				cond_resched();
			}
		} else {
			while (done < c->len) {
				chunk = min_t(u64, c->len - done, VACCEL_CHUNK);
				memmove(d + done, s + done, chunk);
				done += chunk;
				cond_resched();
			}
		}
		break;
	}
	case VACCEL_OP_ADD32: {
		u32 *d = (u32 *)((u8 *)vaccel_buf_vaddr(c->dst) + c->dst_off);
		const u32 *a = (const u32 *)((u8 *)vaccel_buf_vaddr(c->src0) + c->src0_off);
		const u32 *b = (const u32 *)((u8 *)vaccel_buf_vaddr(c->src1) + c->src1_off);

		n = c->len / 4;
		for (i = 0; i < n; i++) {
			d[i] = a[i] + b[i];
			if ((i & 0x3fff) == 0x3fff)
				cond_resched();
		}
		break;
	}
	case VACCEL_OP_DELAY:
		fsleep(c->value);
		return;
	default:
		return;
	}
	atomic64_add(c->len, &vdev->bytes_processed);
}

static void vaccel_run_job(struct vaccel_dev *vdev, struct vaccel_job *job)
{
	struct vaccel_chan *ch = job->chan;
	u64 start, end;
	u32 i;

	start = ktime_get_ns();
	for (i = 0; i < job->ncmds; i++)
		vaccel_exec_cmd(vdev, &job->cmds[i]);
	end = ktime_get_ns();

	atomic64_add(end - start, &vdev->busy_ns);
	atomic64_inc(&vdev->jobs_completed);

	/* Release: every write above is visible to whoever observes the fence. */
	atomic64_set_release(&ch->completed_seq, job->seq);
	wake_up_all(&ch->fence_wq);

	if (job->flags & VACCEL_SUBMIT_NOTIFY)
		vaccel_job_event(job, VACCEL_STATUS_OK, start, end);

	vaccel_job_free(job);
	vaccel_chan_put(ch);
}

static int vaccel_engine_fn(void *data)
{
	struct vaccel_dev *vdev = data;
	struct vaccel_job *job;

	while (!kthread_should_stop()) {
		job = NULL;
		/*
		 * Interruptible so an idle engine does not show up as a hung
		 * D-state task. kthreads receive no signals, so it only wakes
		 * for work or kthread_stop().
		 */
		wait_event_interruptible(vdev->engine_wq,
					 (job = vaccel_pick_job(vdev)) != NULL ||
					 kthread_should_stop());
		if (job)
			vaccel_run_job(vdev, job);
	}
	return 0;
}

int vaccel_sched_init(struct vaccel_dev *vdev)
{
	spin_lock_init(&vdev->sched_lock);
	INIT_LIST_HEAD(&vdev->runlist);
	init_waitqueue_head(&vdev->engine_wq);
	ida_init(&vdev->chan_ida);
	atomic_set(&vdev->channels_used, 0);

	vdev->engine = kthread_run(vaccel_engine_fn, vdev, "vaccel-engine");
	if (IS_ERR(vdev->engine)) {
		int ret = PTR_ERR(vdev->engine);

		vdev->engine = NULL;
		ida_destroy(&vdev->chan_ida);
		return ret;
	}
	return 0;
}

void vaccel_sched_fini(struct vaccel_dev *vdev)
{
	/* Module unload is only possible with no open files, so no work is left. */
	kthread_stop(vdev->engine);
	WARN_ON(!list_empty(&vdev->runlist));
	WARN_ON(atomic_read(&vdev->channels_used) != 0);
	ida_destroy(&vdev->chan_ida);
}
