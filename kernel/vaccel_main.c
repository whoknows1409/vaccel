// SPDX-License-Identifier: GPL-2.0
/*
 * vaccel: a virtual accelerator driver with a hardware resource manager.
 *
 * There is no real hardware: "device memory" is a vmalloc area and the
 * "engine" is a kernel thread. Everything around them is what a real
 * accelerator driver has to get right: a character device with an ioctl
 * uAPI, per-process resource isolation, a device-memory allocator, a pool of
 * hardware channels, a job scheduler, fences, mmap of device memory,
 * completion events through read()/poll(), cleanup when a process dies, and
 * sysfs/debugfs observability.
 *
 * This file: module parameters, the character device, file operations,
 * clients, and the sysfs/debugfs interfaces.
 */
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "vaccel_internal.h"

/* ---- module parameters --------------------------------------------------- */

static unsigned int vram_mb = 64;
module_param(vram_mb, uint, 0444);
MODULE_PARM_DESC(vram_mb, "Size of simulated device memory in MiB (1-1024, default 64)");

static unsigned int max_channels = 32;
module_param(max_channels, uint, 0444);
MODULE_PARM_DESC(max_channels, "Number of hardware channel slots (1-4096, default 32)");

unsigned int vaccel_max_queue_depth = 256;
module_param_named(max_queue_depth, vaccel_max_queue_depth, uint, 0644);
MODULE_PARM_DESC(max_queue_depth, "Max queued jobs per channel before submit returns EAGAIN");

unsigned int vaccel_sched_quantum = 4;
module_param_named(sched_quantum, vaccel_sched_quantum, uint, 0644);
MODULE_PARM_DESC(sched_quantum, "Jobs a channel may run before the engine rotates to the next");

unsigned int vaccel_client_quota_mb;
module_param_named(client_quota_mb, vaccel_client_quota_mb, uint, 0644);
MODULE_PARM_DESC(client_quota_mb, "Per-client device memory limit in MiB (0 = unlimited)");

static struct class *vaccel_class;
static struct vaccel_dev *vaccel_device;

/* ---- clients ------------------------------------------------------------- */

static void vaccel_client_release(struct kref *ref)
{
	struct vaccel_client *client = container_of(ref, struct vaccel_client, ref);

	mutex_destroy(&client->lock);
	mutex_destroy(&client->read_lock);
	kfree(client);
}

void vaccel_client_put(struct vaccel_client *client)
{
	kref_put(&client->ref, vaccel_client_release);
}

static bool vaccel_events_empty(struct vaccel_client *client)
{
	bool empty;

	spin_lock(&client->ev_lock);
	empty = kfifo_is_empty(&client->events);
	spin_unlock(&client->ev_lock);
	return empty;
}

/* Called by the engine (and channel teardown) to queue a completion record. */
void vaccel_client_push_event(struct vaccel_client *client,
			      const struct vaccel_event *ev)
{
	spin_lock(&client->ev_lock);
	if (!client->closed && !kfifo_put(&client->events, *ev))
		client->events_dropped++;   /* reader too slow: count, don't block the engine */
	spin_unlock(&client->ev_lock);
	wake_up_interruptible(&client->ev_wq);
}

static int vaccel_open(struct inode *inode, struct file *file)
{
	struct vaccel_dev *vdev = container_of(inode->i_cdev, struct vaccel_dev, cdev);
	struct vaccel_client *client;

	client = kzalloc(sizeof(*client), GFP_KERNEL);
	if (!client)
		return -ENOMEM;

	kref_init(&client->ref);
	client->vdev = vdev;
	mutex_init(&client->lock);
	mutex_init(&client->read_lock);
	xa_init_flags(&client->bufs, XA_FLAGS_ALLOC1);
	xa_init_flags(&client->chans, XA_FLAGS_ALLOC1);
	spin_lock_init(&client->ev_lock);
	INIT_KFIFO(client->events);
	init_waitqueue_head(&client->ev_wq);
	client->pid = task_tgid_nr(current);
	get_task_comm(client->comm, current);

	mutex_lock(&vdev->clients_lock);
	list_add_tail(&client->node, &vdev->clients);
	mutex_unlock(&vdev->clients_lock);

	file->private_data = client;
	return nonseekable_open(inode, file);
}

/*
 * Last close of the fd, including the implicit one when a process crashes.
 * Runs only once every mmap() of this fd is gone, because each VMA holds a
 * file reference. Everything the client owned is reclaimed here.
 */
static int vaccel_release(struct inode *inode, struct file *file)
{
	struct vaccel_client *client = file->private_data;
	struct vaccel_dev *vdev = client->vdev;

	spin_lock(&client->ev_lock);
	client->closed = true;
	spin_unlock(&client->ev_lock);

	mutex_lock(&vdev->clients_lock);
	list_del(&client->node);
	mutex_unlock(&vdev->clients_lock);

	vaccel_sched_release_client(client);   /* cancels queued jobs first */
	vaccel_mem_release_client(client);     /* then drops buffer handles */
	vaccel_client_put(client);             /* running jobs may still hold refs */
	return 0;
}

static int vaccel_ioctl_info(struct vaccel_client *client, void __user *uarg)
{
	struct vaccel_dev *vdev = client->vdev;
	struct vaccel_info info;

	memset(&info, 0, sizeof(info));        /* no kernel stack bytes leak out */
	info.api_version = VACCEL_API_VERSION;
	info.page_size = PAGE_SIZE;
	info.vram_size = vdev->vram_size;

	mutex_lock(&vdev->mem_lock);
	info.vram_free = vdev->vram_size - vdev->vram_used;
	mutex_unlock(&vdev->mem_lock);
	info.largest_free_block = vaccel_mem_largest_free(vdev);

	info.max_channels = vdev->max_channels;
	info.free_channels = vdev->max_channels - atomic_read(&vdev->channels_used);
	info.jobs_completed = atomic64_read(&vdev->jobs_completed);
	info.client_quota = (u64)READ_ONCE(vaccel_client_quota_mb) << 20;

	mutex_lock(&client->lock);
	info.client_used = client->vram_used;
	mutex_unlock(&client->lock);

	return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
}

static long vaccel_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vaccel_client *client = file->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
	case VACCEL_IOCTL_INFO:
		return vaccel_ioctl_info(client, uarg);
	case VACCEL_IOCTL_ALLOC:
		return vaccel_ioctl_alloc(client, uarg);
	case VACCEL_IOCTL_FREE:
		return vaccel_ioctl_free(client, uarg);
	case VACCEL_IOCTL_CHAN_CREATE:
		return vaccel_ioctl_chan_create(client, uarg);
	case VACCEL_IOCTL_CHAN_DESTROY:
		return vaccel_ioctl_chan_destroy(client, uarg);
	case VACCEL_IOCTL_SUBMIT:
		return vaccel_ioctl_submit(client, uarg);
	case VACCEL_IOCTL_WAIT:
		return vaccel_ioctl_wait(client, uarg);
	default:
		return -ENOTTY;
	}
}

/*
 * read() returns whole struct vaccel_event records. Each record is peeked,
 * copied, and only then removed, so a faulting user buffer loses nothing.
 */
static ssize_t vaccel_read(struct file *file, char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	struct vaccel_client *client = file->private_data;
	struct vaccel_event ev;
	size_t done = 0;
	ssize_t ret;

	if (count < sizeof(ev))
		return -EINVAL;
	if (mutex_lock_interruptible(&client->read_lock))
		return -ERESTARTSYS;

	while (done + sizeof(ev) <= count) {
		unsigned int have;

		spin_lock(&client->ev_lock);
		have = kfifo_peek(&client->events, &ev);
		spin_unlock(&client->ev_lock);

		if (!have) {
			if (done)
				break;
			if (file->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				goto out;
			}
			ret = wait_event_interruptible(client->ev_wq,
						       !vaccel_events_empty(client));
			if (ret)
				goto out;
			continue;
		}

		if (copy_to_user(ubuf + done, &ev, sizeof(ev))) {
			ret = done ? (ssize_t)done : -EFAULT;
			goto out;
		}
		spin_lock(&client->ev_lock);
		kfifo_skip(&client->events);
		spin_unlock(&client->ev_lock);
		done += sizeof(ev);
	}
	ret = done;
out:
	mutex_unlock(&client->read_lock);
	return ret;
}

static __poll_t vaccel_poll(struct file *file, poll_table *wait)
{
	struct vaccel_client *client = file->private_data;

	poll_wait(file, &client->ev_wq, wait);
	return vaccel_events_empty(client) ? 0 : EPOLLIN | EPOLLRDNORM;
}

static const struct file_operations vaccel_fops = {
	.owner          = THIS_MODULE,
	.open           = vaccel_open,
	.release        = vaccel_release,
	.unlocked_ioctl = vaccel_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,   /* uAPI has no pointers-as-longs */
	.mmap           = vaccel_mmap,
	.read           = vaccel_read,
	.poll           = vaccel_poll,
};

/* ---- sysfs: /sys/class/vaccel/vaccel0/ ----------------------------------- */

#define VACCEL_SHOW(name, fmt, expr)						\
static ssize_t name##_show(struct device *dev,					\
			   struct device_attribute *attr, char *buf)		\
{										\
	struct vaccel_dev *vdev = dev_get_drvdata(dev);				\
	return sysfs_emit(buf, fmt "\n", expr);					\
}										\
static DEVICE_ATTR_RO(name)

VACCEL_SHOW(vram_total_kb, "%llu", vdev->vram_size >> 10);
VACCEL_SHOW(vram_used_kb, "%llu", READ_ONCE(vdev->vram_used) >> 10);
VACCEL_SHOW(vram_largest_free_kb, "%llu", vaccel_mem_largest_free(vdev) >> 10);
VACCEL_SHOW(channels_max, "%u", vdev->max_channels);
VACCEL_SHOW(channels_used, "%d", atomic_read(&vdev->channels_used));
VACCEL_SHOW(jobs_submitted, "%lld", atomic64_read(&vdev->jobs_submitted));
VACCEL_SHOW(jobs_completed, "%lld", atomic64_read(&vdev->jobs_completed));
VACCEL_SHOW(jobs_cancelled, "%lld", atomic64_read(&vdev->jobs_cancelled));
VACCEL_SHOW(bytes_processed, "%lld", atomic64_read(&vdev->bytes_processed));
VACCEL_SHOW(engine_busy_ms, "%lld", atomic64_read(&vdev->busy_ns) / 1000000);

static struct attribute *vaccel_attrs[] = {
	&dev_attr_vram_total_kb.attr,
	&dev_attr_vram_used_kb.attr,
	&dev_attr_vram_largest_free_kb.attr,
	&dev_attr_channels_max.attr,
	&dev_attr_channels_used.attr,
	&dev_attr_jobs_submitted.attr,
	&dev_attr_jobs_completed.attr,
	&dev_attr_jobs_cancelled.attr,
	&dev_attr_bytes_processed.attr,
	&dev_attr_engine_busy_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vaccel);

/* ---- debugfs: /sys/kernel/debug/vaccel/ ---------------------------------- */

static int vaccel_clients_show(struct seq_file *m, void *unused)
{
	struct vaccel_dev *vdev = m->private;
	struct vaccel_client *c;

	seq_printf(m, "%-8s %-16s %10s %8s %8s\n",
		   "PID", "COMM", "VRAM_KB", "CHANNELS", "DROPPED");
	mutex_lock(&vdev->clients_lock);
	list_for_each_entry(c, &vdev->clients, node) {
		mutex_lock(&c->lock);
		seq_printf(m, "%-8d %-16s %10llu %8u %8llu\n", c->pid, c->comm,
			   c->vram_used >> 10, c->nr_channels,
			   READ_ONCE(c->events_dropped));
		mutex_unlock(&c->lock);
	}
	mutex_unlock(&vdev->clients_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vaccel_clients);

static int vaccel_buddyinfo_show(struct seq_file *m, void *unused)
{
	struct vaccel_dev *vdev = m->private;
	struct vbuddy *b = &vdev->buddy;
	u64 free, largest;
	s32 top;
	u32 o;

	mutex_lock(&vdev->mem_lock);
	seq_puts(m, "block_kb");
	for (o = 0; o <= b->max_order; o++)
		seq_printf(m, " %7lu", (PAGE_SIZE >> 10) << o);
	seq_puts(m, "\nfree    ");
	for (o = 0; o <= b->max_order; o++)
		seq_printf(m, " %7u", b->nr_free[o]);
	free = (u64)b->free_pages << PAGE_SHIFT;
	top = vbuddy_largest_free_order(b);
	largest = top < 0 ? 0 : (u64)PAGE_SIZE << top;
	mutex_unlock(&vdev->mem_lock);

	/* External fragmentation: share of free memory unusable for the largest request. */
	seq_printf(m, "\nfree_kb %llu largest_kb %llu fragmentation %llu%%\n",
		   free >> 10, largest >> 10,
		   free ? 100 - div64_u64(largest * 100, free) : 0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(vaccel_buddyinfo);

/* ---- module init/exit ---------------------------------------------------- */

static int __init vaccel_init(void)
{
	struct vaccel_dev *vdev;
	int ret;

	if (vram_mb < 1 || vram_mb > 1024 || max_channels < 1 || max_channels > 4096) {
		pr_err("vaccel: invalid vram_mb=%u or max_channels=%u\n", vram_mb, max_channels);
		return -EINVAL;
	}

	vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
	if (!vdev)
		return -ENOMEM;
	mutex_init(&vdev->clients_lock);
	INIT_LIST_HEAD(&vdev->clients);
	vdev->max_channels = max_channels;

	/* Bring up the internals before the device node becomes visible. */
	ret = vaccel_mem_init(vdev, (u64)vram_mb << 20);
	if (ret)
		goto err_free;
	ret = vaccel_sched_init(vdev);
	if (ret)
		goto err_mem;

	ret = alloc_chrdev_region(&vdev->devt, 0, 1, VACCEL_NAME);
	if (ret)
		goto err_sched;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	vaccel_class = class_create(VACCEL_NAME);
#else
	vaccel_class = class_create(THIS_MODULE, VACCEL_NAME);
#endif
	if (IS_ERR(vaccel_class)) {
		ret = PTR_ERR(vaccel_class);
		goto err_region;
	}

	cdev_init(&vdev->cdev, &vaccel_fops);
	vdev->cdev.owner = THIS_MODULE;
	ret = cdev_add(&vdev->cdev, vdev->devt, 1);
	if (ret)
		goto err_class;

	vdev->device = device_create_with_groups(vaccel_class, NULL, vdev->devt, vdev,
						 vaccel_groups, VACCEL_NAME "%d", 0);
	if (IS_ERR(vdev->device)) {
		ret = PTR_ERR(vdev->device);
		goto err_cdev;
	}

	/* debugfs is best effort: failures are not fatal by design. */
	vdev->debugfs = debugfs_create_dir(VACCEL_NAME, NULL);
	debugfs_create_file("clients", 0444, vdev->debugfs, vdev, &vaccel_clients_fops);
	debugfs_create_file("buddyinfo", 0444, vdev->debugfs, vdev, &vaccel_buddyinfo_fops);

	vaccel_device = vdev;
	pr_info("vaccel: /dev/%s0 ready: %u MiB device memory, %u channels\n",
		VACCEL_NAME, vram_mb, max_channels);
	return 0;

err_cdev:
	cdev_del(&vdev->cdev);
err_class:
	class_destroy(vaccel_class);
err_region:
	unregister_chrdev_region(vdev->devt, 1);
err_sched:
	vaccel_sched_fini(vdev);
err_mem:
	vaccel_mem_fini(vdev);
err_free:
	kfree(vdev);
	return ret;
}

static void __exit vaccel_exit(void)
{
	struct vaccel_dev *vdev = vaccel_device;

	debugfs_remove_recursive(vdev->debugfs);
	device_destroy(vaccel_class, vdev->devt);
	cdev_del(&vdev->cdev);
	class_destroy(vaccel_class);
	unregister_chrdev_region(vdev->devt, 1);
	vaccel_sched_fini(vdev);
	vaccel_mem_fini(vdev);
	kfree(vdev);
	pr_info("vaccel: unloaded\n");
}

module_init(vaccel_init);
module_exit(vaccel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Omkar Bhoir");
MODULE_DESCRIPTION("Virtual accelerator driver with a hardware resource manager");
MODULE_VERSION("1.0");
