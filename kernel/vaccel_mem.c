// SPDX-License-Identifier: GPL-2.0
/*
 * vaccel: device memory management.
 *
 * Device memory is a single vmalloc_user() area. The buddy allocator hands
 * out naturally aligned power-of-two page blocks from it. Each allocation is
 * a reference-counted struct vaccel_buf owned by a client handle, and user
 * space maps it with mmap(fd, offset = handle << PAGE_SHIFT).
 *
 * Locking rule that matters here: the mmap path runs with mmap_lock held and
 * the VMA close path takes vdev->mem_lock, so no code may fault on user
 * memory (copy_{to,from}_user) while holding client->lock or mem_lock.
 * Handle lookups use the xarray's own lock instead of client->lock.
 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/version.h>

#include "vaccel_internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
#define vaccel_vm_flags_clear(vma, f) vm_flags_clear(vma, f)
#else
#define vaccel_vm_flags_clear(vma, f) ((vma)->vm_flags &= ~(f))
#endif

int vaccel_mem_init(struct vaccel_dev *vdev, u64 size)
{
	u32 npages = size >> PAGE_SHIFT;

	mutex_init(&vdev->mem_lock);
	vdev->vram_size = (u64)npages << PAGE_SHIFT;

	/* vmalloc_user(): zeroed and flagged VM_USERMAP so it may be mmap()ed. */
	vdev->vram = vmalloc_user(vdev->vram_size);
	if (!vdev->vram)
		return -ENOMEM;

	vdev->buddy_pages = kvcalloc(npages, sizeof(*vdev->buddy_pages), GFP_KERNEL);
	if (!vdev->buddy_pages) {
		vfree(vdev->vram);
		return -ENOMEM;
	}
	vbuddy_init(&vdev->buddy, vdev->buddy_pages, npages);
	return 0;
}

void vaccel_mem_fini(struct vaccel_dev *vdev)
{
	WARN_ON(vdev->vram_used != 0);
	kvfree(vdev->buddy_pages);
	vfree(vdev->vram);
}

u64 vaccel_mem_largest_free(struct vaccel_dev *vdev)
{
	s32 order;

	mutex_lock(&vdev->mem_lock);
	order = vbuddy_largest_free_order(&vdev->buddy);
	mutex_unlock(&vdev->mem_lock);
	return order < 0 ? 0 : (u64)PAGE_SIZE << order;
}

static void vaccel_buf_release(struct kref *ref)
{
	struct vaccel_buf *buf = container_of(ref, struct vaccel_buf, ref);
	struct vaccel_dev *vdev = buf->vdev;

	mutex_lock(&vdev->mem_lock);
	WARN_ON(vbuddy_free(&vdev->buddy, buf->first_page));
	vdev->vram_used -= buf->size;
	mutex_unlock(&vdev->mem_lock);
	kfree(buf);
}

/* May sleep: never call with a spinlock held. */
void vaccel_buf_put(struct vaccel_buf *buf)
{
	if (buf)
		kref_put(&buf->ref, vaccel_buf_release);
}

/*
 * Returns the buffer with an extra reference, or NULL. Taking the reference
 * under xa_lock is safe because removal (xa_erase) also takes xa_lock, so the
 * handle table's own reference is still held while we look.
 */
struct vaccel_buf *vaccel_buf_lookup(struct vaccel_client *client, u32 handle)
{
	struct vaccel_buf *buf;

	xa_lock(&client->bufs);
	buf = xa_load(&client->bufs, handle);
	if (buf)
		kref_get(&buf->ref);
	xa_unlock(&client->bufs);
	return buf;
}

int vaccel_ioctl_alloc(struct vaccel_client *client, void __user *uarg)
{
	struct vaccel_dev *vdev = client->vdev;
	struct vaccel_alloc req;
	struct vaccel_buf *buf;
	u64 quota;
	u32 handle;
	s32 page;
	int ret;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;
	if (req.flags || req.size == 0)
		return -EINVAL;
	if (req.size > vdev->vram_size)
		return -ENOMEM;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	kref_init(&buf->ref);
	buf->vdev = vdev;
	buf->order = vbuddy_order_for(DIV_ROUND_UP_ULL(req.size, PAGE_SIZE));
	buf->size = (u64)PAGE_SIZE << buf->order;
	atomic_set(&buf->map_count, 0);

	/* client->lock makes the quota check and the reservation atomic. */
	mutex_lock(&client->lock);
	quota = (u64)READ_ONCE(vaccel_client_quota_mb) << 20;
	if (quota && client->vram_used + buf->size > quota) {
		ret = -EDQUOT;
		goto err_unlock;
	}

	mutex_lock(&vdev->mem_lock);
	page = vbuddy_alloc(&vdev->buddy, buf->order);
	if (page != VBUDDY_NONE)
		vdev->vram_used += buf->size;
	mutex_unlock(&vdev->mem_lock);
	if (page == VBUDDY_NONE) {
		ret = -ENOMEM;
		goto err_unlock;
	}
	buf->first_page = page;

	/* Never hand a client memory that still holds another client's data. */
	memset(vaccel_buf_vaddr(buf), 0, buf->size);

	/* Cyclic IDs make a stale handle unlikely to name a new object. */
	ret = xa_alloc_cyclic(&client->bufs, &handle, buf,
			      XA_LIMIT(1, VACCEL_MAX_HANDLES - 1),
			      &client->next_buf_handle, GFP_KERNEL);
	if (ret < 0) {
		mutex_unlock(&client->lock);
		vaccel_buf_put(buf);          /* returns the pages */
		return ret == -EBUSY ? -ENOSPC : ret;
	}
	client->vram_used += buf->size;
	mutex_unlock(&client->lock);

	req.handle = handle;
	req.alloc_size = buf->size;
	req.mmap_offset = (u64)handle << PAGE_SHIFT;
	req.dev_addr = (u64)buf->first_page << PAGE_SHIFT;
	if (copy_to_user(uarg, &req, sizeof(req))) {
		/* User space never learnt the handle: undo, unless already freed. */
		mutex_lock(&client->lock);
		if (xa_cmpxchg(&client->bufs, handle, buf, NULL, 0) == buf) {
			client->vram_used -= buf->size;
			mutex_unlock(&client->lock);
			vaccel_buf_put(buf);
		} else {
			mutex_unlock(&client->lock);
		}
		return -EFAULT;
	}
	return 0;

err_unlock:
	mutex_unlock(&client->lock);
	kfree(buf);
	return ret;
}

int vaccel_ioctl_free(struct vaccel_client *client, void __user *uarg)
{
	struct vaccel_handle_arg req;
	struct vaccel_buf *buf;

	if (copy_from_user(&req, uarg, sizeof(req)))
		return -EFAULT;
	if (req.pad)
		return -EINVAL;

	mutex_lock(&client->lock);
	buf = xa_erase(&client->bufs, req.handle);
	if (buf)
		client->vram_used -= buf->size;
	mutex_unlock(&client->lock);
	if (!buf)
		return -ENOENT;

	/*
	 * Drops only the handle's reference. Queued jobs and live mappings
	 * keep the memory reserved until they are done with it.
	 */
	vaccel_buf_put(buf);
	return 0;
}

void vaccel_mem_release_client(struct vaccel_client *client)
{
	struct vaccel_buf *buf;
	unsigned long idx;

	mutex_lock(&client->lock);
	xa_for_each(&client->bufs, idx, buf) {
		xa_erase(&client->bufs, idx);
		vaccel_buf_put(buf);
	}
	client->vram_used = 0;
	mutex_unlock(&client->lock);
	xa_destroy(&client->bufs);
}

/* ---- mmap ---------------------------------------------------------------- */

/* Called when a VMA is duplicated (fork) or split (partial munmap/mprotect). */
static void vaccel_vm_open(struct vm_area_struct *vma)
{
	struct vaccel_buf *buf = vma->vm_private_data;

	kref_get(&buf->ref);
	atomic_inc(&buf->map_count);
}

static void vaccel_vm_close(struct vm_area_struct *vma)
{
	struct vaccel_buf *buf = vma->vm_private_data;

	atomic_dec(&buf->map_count);
	vaccel_buf_put(buf);
}

static const struct vm_operations_struct vaccel_vm_ops = {
	.open  = vaccel_vm_open,
	.close = vaccel_vm_close,
};

int vaccel_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vaccel_client *client = file->private_data;
	unsigned long size = vma->vm_end - vma->vm_start;
	struct vaccel_buf *buf;
	int ret;

	/* Private (copy-on-write) mappings would silently detach from the device. */
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;
	/* Device memory is data, never code. */
	if (vma->vm_flags & VM_EXEC)
		return -EPERM;
	if (vma->vm_pgoff > U32_MAX)
		return -EINVAL;

	buf = vaccel_buf_lookup(client, (u32)vma->vm_pgoff);
	if (!buf)
		return -EINVAL;
	if (size > buf->size) {
		ret = -EINVAL;
		goto err_put;
	}

	vaccel_vm_flags_clear(vma, VM_MAYEXEC);   /* forbid a later mprotect(PROT_EXEC) */

	/* Inserts every page now, so no fault handler is needed. */
	ret = remap_vmalloc_range(vma, buf->vdev->vram, buf->first_page);
	if (ret)
		goto err_put;

	vma->vm_ops = &vaccel_vm_ops;
	vma->vm_private_data = buf;   /* the lookup reference now belongs to the VMA */
	atomic_inc(&buf->map_count);
	return 0;

err_put:
	vaccel_buf_put(buf);
	return ret;
}
