/*
 * mm_internal.h - Internal primitives shared by the core and the test/oracle
 * layer. These are the unit-of-proof functions (split/insert/remove/merge)
 * plus arithmetic guards and the well-formedness oracle.
 *
 * Nothing here allocates or recurses.
 */
#ifndef MM_INTERNAL_H
#define MM_INTERNAL_H

#include <stdbool.h>
#include "mm_types.h"

/* ---- arithmetic guards (page math near UINT64_MAX / AS_MAX) ---- */

/* True iff a + b would overflow uint64_t. */
bool add_overflows(uint64_t a, uint64_t b);

/* True iff rounding len up to the next page multiple would overflow. */
bool round_up_overflows(uint64_t len);

/* Round len up to a page multiple. Caller must ensure !round_up_overflows. */
uint64_t round_up_page(uint64_t len);

/* True iff x is page-aligned. */
bool is_page_aligned(uint64_t x);

/* ---- VMA helpers ---- */

/* True iff a and b are adjacent (a.end == b.start) and compatible to merge. */
bool vma_mergeable(const struct vma *a, const struct vma *b);

/* ---- address-space primitives ---- */

/*
 * Find the half-open index range [*lo, *hi) of VMAs that overlap
 * [start, end). On return *lo == *hi means no overlap; *lo is then the
 * insertion point preserving sort order.
 */
void as_find_range(const struct addr_space *as, uint64_t start, uint64_t end,
                   size_t *lo, size_t *hi);

/*
 * Split the VMA at index idx at split_addr (start < split_addr < end),
 * producing two adjacent VMAs. Requires count < VMA_CAP. Returns MM_OK or
 * MM_ENOMEM (no capacity) / MM_EINVAL (split_addr not interior/aligned).
 */
mm_status as_split_at(struct addr_space *as, size_t idx, uint64_t split_addr);

/* Insert v at index idx, shifting the tail up. Requires count < VMA_CAP. */
mm_status as_insert_at(struct addr_space *as, size_t idx, struct vma v);

/* Remove VMAs in index range [lo, hi), shifting the tail down. */
void as_remove_range(struct addr_space *as, size_t lo, size_t hi);

/* Merge any adjacent mergeable VMAs in [from, to] neighborhood, restoring
 * canonical form. Safe to call with indices near the edited region. */
void as_canonicalize(struct addr_space *as);

/* Top-down first-fit placement of a free hole of `length` bytes (page
 * multiple) within (as_min, as_max). Returns MM_OK and *out, or MM_ENOMEM. */
mm_status as_find_free(const struct addr_space *as, uint64_t length,
                       uint64_t *out);

/* ---- well-formedness oracle (runtime mirror of the ACSL as_wf predicate) ---- */

/* Returns true iff `as` satisfies every well-formedness invariant. Used by
 * tests (ASSERT_WF) for defense-in-depth alongside the WP proof. */
bool as_check_wf(const struct addr_space *as);

#endif /* MM_INTERNAL_H */
