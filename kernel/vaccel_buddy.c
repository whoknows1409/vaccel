// SPDX-License-Identifier: GPL-2.0
/*
 * Binary buddy allocator. See vaccel_buddy.h for the design notes.
 *
 * Complexity: alloc and free are O(max_order) — at most one split or merge
 * per order — independent of how much memory is in use.
 */
#include "vaccel_buddy.h"

static void list_push(struct vbuddy *b, u32 page, u32 order)
{
	struct vbuddy_page *p = &b->pages[page];
	s32 head = b->free_head[order];

	p->order = (u8)order;
	p->flags = VBUDDY_FREE_HEAD;
	p->prev = VBUDDY_NONE;
	p->next = head;
	if (head != VBUDDY_NONE)
		b->pages[head].prev = (s32)page;
	b->free_head[order] = (s32)page;
	b->nr_free[order]++;
}

static void list_remove(struct vbuddy *b, u32 page)
{
	struct vbuddy_page *p = &b->pages[page];
	u32 order = p->order;

	if (p->prev != VBUDDY_NONE)
		b->pages[p->prev].next = p->next;
	else
		b->free_head[order] = p->next;
	if (p->next != VBUDDY_NONE)
		b->pages[p->next].prev = p->prev;
	p->next = p->prev = VBUDDY_NONE;
	p->flags = 0;
	b->nr_free[order]--;
}

void vbuddy_init(struct vbuddy *b, struct vbuddy_page *pages, u32 npages)
{
	u32 i, idx;

	b->pages = pages;
	b->npages = npages;
	b->free_pages = 0;
	b->max_order = 0;
	while (b->max_order < VBUDDY_MAX_ORDER &&
	       ((u64)1 << (b->max_order + 1)) <= npages)
		b->max_order++;

	for (i = 0; i <= VBUDDY_MAX_ORDER; i++) {
		b->free_head[i] = VBUDDY_NONE;
		b->nr_free[i] = 0;
	}
	for (i = 0; i < npages; i++) {
		pages[i].next = pages[i].prev = VBUDDY_NONE;
		pages[i].order = 0;
		pages[i].flags = 0;
	}

	/*
	 * Carve [0, npages) into the largest naturally aligned blocks. For a
	 * power-of-two size this is a single block of max_order.
	 */
	idx = 0;
	while (idx < npages) {
		u32 order = b->max_order;

		while (order > 0 &&
		       ((idx & ((1u << order) - 1)) != 0 ||
			(u64)idx + (1u << order) > npages))
			order--;
		list_push(b, idx, order);
		b->free_pages += 1u << order;
		idx += 1u << order;
	}
}

s32 vbuddy_alloc(struct vbuddy *b, u32 order)
{
	u32 o = order;
	s32 page;

	if (order > b->max_order)
		return VBUDDY_NONE;

	/* Find the smallest free block that is big enough. */
	while (o <= b->max_order && b->free_head[o] == VBUDDY_NONE)
		o++;
	if (o > b->max_order)
		return VBUDDY_NONE;

	page = b->free_head[o];
	list_remove(b, (u32)page);

	/* Split it down, returning the upper halves to the free lists. */
	while (o > order) {
		o--;
		list_push(b, (u32)page + (1u << o), o);
	}

	b->pages[page].order = (u8)order;
	b->pages[page].flags = VBUDDY_ALLOC_HEAD;
	b->free_pages -= 1u << order;
	return page;
}

int vbuddy_free(struct vbuddy *b, u32 page)
{
	u32 order;

	if (page >= b->npages || b->pages[page].flags != VBUDDY_ALLOC_HEAD)
		return -1;   /* double free or bogus pointer */

	order = b->pages[page].order;
	b->pages[page].flags = 0;
	b->free_pages += 1u << order;

	/* Merge with the buddy while it is a free block of the same order. */
	while (order < b->max_order) {
		u32 buddy = page ^ (1u << order);

		if ((u64)buddy + (1u << order) > b->npages)
			break;
		if (b->pages[buddy].flags != VBUDDY_FREE_HEAD ||
		    b->pages[buddy].order != order)
			break;
		list_remove(b, buddy);
		if (buddy < page)
			page = buddy;
		order++;
	}
	list_push(b, page, order);
	return 0;
}

s32 vbuddy_largest_free_order(const struct vbuddy *b)
{
	s32 o;

	for (o = (s32)b->max_order; o >= 0; o--)
		if (b->free_head[o] != VBUDDY_NONE)
			return o;
	return VBUDDY_NONE;
}

#ifndef __KERNEL__
int vbuddy_check(const struct vbuddy *b, const char **why)
{
	u32 idx = 0, free_pages = 0, o;
	u32 counted[VBUDDY_MAX_ORDER + 1] = { 0 };

#define FAIL(msg) do { *why = (msg); return -1; } while (0)
	/* 1. The heads tile [0, npages) exactly, aligned, with clean interiors. */
	while (idx < b->npages) {
		const struct vbuddy_page *p = &b->pages[idx];
		u32 size, j;

		if (p->flags != VBUDDY_FREE_HEAD && p->flags != VBUDDY_ALLOC_HEAD)
			FAIL("page that should be a block head is not");
		if (p->order > b->max_order)
			FAIL("order above max_order");
		size = 1u << p->order;
		if (idx & (size - 1))
			FAIL("block not naturally aligned");
		if ((u64)idx + size > b->npages)
			FAIL("block runs past the end");
		for (j = 1; j < size; j++)
			if (b->pages[idx + j].flags)
				FAIL("interior page marked as a head");
		if (p->flags == VBUDDY_FREE_HEAD) {
			u32 buddy = idx ^ size;

			free_pages += size;
			counted[p->order]++;
			/* 2. Coalescing is complete: no free buddy pairs remain. */
			if (p->order < b->max_order &&
			    (u64)buddy + size <= b->npages &&
			    b->pages[buddy].flags == VBUDDY_FREE_HEAD &&
			    b->pages[buddy].order == p->order)
				FAIL("two free buddies were not merged");
		}
		idx += size;
	}
	if (free_pages != b->free_pages)
		FAIL("free page counter mismatch");

	/* 3. Free lists are well-formed and agree with the tiling. */
	for (o = 0; o <= b->max_order; o++) {
		s32 cur = b->free_head[o], prev = VBUDDY_NONE;
		u32 n = 0;

		while (cur != VBUDDY_NONE) {
			const struct vbuddy_page *p = &b->pages[cur];

			if (p->flags != VBUDDY_FREE_HEAD || p->order != o)
				FAIL("free list entry has wrong flags/order");
			if (p->prev != prev)
				FAIL("broken prev link");
			if (++n > b->npages)
				FAIL("cycle in free list");
			prev = cur;
			cur = p->next;
		}
		if (n != counted[o] || n != b->nr_free[o])
			FAIL("free list length mismatch");
	}
#undef FAIL
	return 0;
}
#endif
