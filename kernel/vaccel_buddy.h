/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Binary buddy allocator for vaccel device memory.
 *
 * Deliberately free of kernel dependencies (no list_head, no locking, no
 * allocation) so the exact same file is compiled into the module and into a
 * user-space fuzz test that runs under AddressSanitizer/UBSan.
 *
 * Memory is managed in pages. A block of order k is 2^k pages and starts at
 * a page index that is a multiple of 2^k. Per-page metadata lives in an
 * array supplied by the caller; only the first page of a block ("head") is
 * meaningful. Free lists are intrusive doubly linked lists of page indices.
 *
 * The caller provides the locking (the driver uses vdev->mem_lock).
 */
#ifndef VACCEL_BUDDY_H
#define VACCEL_BUDDY_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint8_t  u8;
typedef uint32_t u32;
typedef int32_t  s32;
typedef uint64_t u64;
#endif

#define VBUDDY_MAX_ORDER  20   /* 2^20 pages = 4 GiB with 4 KiB pages */
#define VBUDDY_NONE       (-1)

#define VBUDDY_FREE_HEAD  0x1  /* first page of a free block */
#define VBUDDY_ALLOC_HEAD 0x2  /* first page of an allocated block */

struct vbuddy_page {
	s32 next;   /* free-list links (page indices), valid for free heads */
	s32 prev;
	u8 order;   /* order of the block this page heads */
	u8 flags;   /* VBUDDY_*_HEAD, 0 for pages inside a block */
	u8 pad[2];
};

struct vbuddy {
	u32 npages;
	u32 max_order;
	u32 free_pages;
	s32 free_head[VBUDDY_MAX_ORDER + 1];
	u32 nr_free[VBUDDY_MAX_ORDER + 1];   /* free blocks per order */
	struct vbuddy_page *pages;           /* npages entries, caller-owned */
};

/* Smallest order whose block holds at least `npages` pages. */
static inline u32 vbuddy_order_for(u64 npages)
{
	u32 order = 0;

	while (order < 63 && ((u64)1 << order) < npages)
		order++;
	return order;
}

void vbuddy_init(struct vbuddy *b, struct vbuddy_page *pages, u32 npages);
s32  vbuddy_alloc(struct vbuddy *b, u32 order);   /* first page index, or VBUDDY_NONE */
int  vbuddy_free(struct vbuddy *b, u32 page);     /* 0, or -1 if not an allocated head */
s32  vbuddy_largest_free_order(const struct vbuddy *b); /* VBUDDY_NONE if full */

#ifndef __KERNEL__
/* Full structural consistency check, used by the user-space fuzz test. */
int vbuddy_check(const struct vbuddy *b, const char **why);
#endif

#endif /* VACCEL_BUDDY_H */
