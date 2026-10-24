// SPDX-License-Identifier: GPL-2.0
/*
 * User-space fuzz test for the kernel's buddy allocator (kernel/vaccel_buddy.c
 * is compiled unchanged into this program). Build with -fsanitize=address,undefined.
 *
 * Random alloc/free sequences are checked against a shadow map that records
 * which allocation owns every page, so any overlap, lost page, or double
 * hand-out is caught immediately; vbuddy_check() verifies the allocator's
 * internal invariants (alignment, tiling, free lists, complete coalescing).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/vaccel_buddy.h"

static unsigned long long rng_state = 0x9E3779B97F4A7C15ull;
static unsigned rnd(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (unsigned)(rng_state >> 11);
}

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)

static void check(struct vbuddy *b, const char *ctx)
{
	const char *why = NULL;

	if (vbuddy_check(b, &why))
		DIE("invariant broken after %s: %s", ctx, why);
}

static void run(u32 npages, long ops)
{
	struct vbuddy b;
	struct vbuddy_page *pages = calloc(npages, sizeof(*pages));
	int *owner = malloc(npages * sizeof(int));     /* page -> allocation id, -1 free */
	s32 *live = malloc(npages * sizeof(s32));      /* first pages of live blocks */
	u32 nlive = 0, i;
	long op, allocs = 0, fails = 0;

	for (i = 0; i < npages; i++)
		owner[i] = -1;
	vbuddy_init(&b, pages, npages);
	check(&b, "init");
	if (b.free_pages != npages)
		DIE("init lost pages");

	for (op = 0; op < ops; op++) {
		int do_alloc = nlive == 0 || (rnd() % 100) < 55;

		if (do_alloc) {
			u32 order = rnd() % (b.max_order + 1);
			s32 p;

			if (rnd() % 4)
				order = rnd() % 4;   /* mostly small blocks */
			p = vbuddy_alloc(&b, order);
			if (p == VBUDDY_NONE) {
				/* Must only fail when no free block of that order exists. */
				s32 top = vbuddy_largest_free_order(&b);

				if (top >= (s32)order)
					DIE("alloc(order %u) failed with a free order-%d block", order, top);
				fails++;
			} else {
				if (p & ((1u << order) - 1))
					DIE("misaligned block");
				for (i = 0; i < (1u << order); i++) {
					if (owner[p + i] != -1)
						DIE("page %u handed out twice", p + i);
					owner[p + i] = p;
				}
				live[nlive++] = p;
				allocs++;
			}
		} else {
			u32 k = rnd() % nlive;
			s32 p = live[k];
			u32 order = b.pages[p].order;

			if (vbuddy_free(&b, (u32)p))
				DIE("free of live block %d rejected", p);
			if (vbuddy_free(&b, (u32)p) == 0)
				DIE("double free not detected");
			for (i = 0; i < (1u << order); i++)
				owner[p + i] = -1;
			live[k] = live[--nlive];
		}
		if ((op & 63) == 0)
			check(&b, do_alloc ? "alloc" : "free");
	}

	while (nlive)
		if (vbuddy_free(&b, (u32)live[--nlive]))
			DIE("final free failed");
	check(&b, "final free");
	if (b.free_pages != npages)
		DIE("pages leaked: %u of %u free", b.free_pages, npages);
	if (vbuddy_free(&b, npages + 5) == 0 || vbuddy_free(&b, 1) == 0)
		DIE("bogus free accepted");

	printf("  npages=%-6u ops=%ld allocs=%ld failed=%ld max_order=%u  OK\n",
	       npages, ops, allocs, fails, b.max_order);
	free(pages);
	free(owner);
	free(live);
}

int main(int argc, char **argv)
{
	long ops = argc > 1 ? atol(argv[1]) : 1000000;
	/* Powers of two and awkward sizes that need several initial blocks. */
	u32 sizes[] = { 1, 2, 3, 64, 1000, 4096, 4097, 16384 };
	unsigned k;

	printf("buddy allocator fuzz test\n");
	for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
		run(sizes[k], sizes[k] < 64 ? ops / 10 : ops);
	printf("all buddy fuzz runs passed\n");
	return 0;
}
