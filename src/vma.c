/*
 * vma.c - VMA primitives and arithmetic guards.
 *
 * These are the unit-of-proof functions. No allocation, no recursion; the
 * array shifts are bounded by VMA_CAP. ACSL contracts are co-located above
 * each function so they stay in sync with the implementation.
 */
#include "mm_internal.h"
#include "mm_acsl.h"

#include <stdint.h>

/* ---- arithmetic guards ---- */

/*@
  assigns \nothing;
  ensures \result <==> acsl_add_overflows(a, b);
*/
bool add_overflows(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b;
}

/*@
  assigns \nothing;
  ensures \result <==> acsl_round_up_overflows(len);
*/
bool round_up_overflows(uint64_t len)
{
    return len > UINT64_MAX - (PAGE_SIZE - 1u);
}

/*@
  requires !acsl_round_up_overflows(len);
  assigns \nothing;
  ensures acsl_aligned(\result);
  ensures \result >= len;
  ensures \result < len + PAGE_SIZE;
*/
uint64_t round_up_page(uint64_t len)
{
    return (len + PAGE_MASK) & ~PAGE_MASK;
}

/*@
  assigns \nothing;
  ensures \result <==> acsl_aligned(x);
*/
bool is_page_aligned(uint64_t x)
{
    return (x & PAGE_MASK) == 0u;
}

/* ---- VMA helpers ---- */

/*@
  requires \valid_read(a) && \valid_read(b);
  assigns \nothing;
*/
bool vma_mergeable(const struct vma *a, const struct vma *b)
{
    if (a->end != b->start)
        return false;
    if (a->prot != b->prot || a->flags != b->flags || a->backing != b->backing)
        return false;
    /* Distinct logical mappings (e.g. different ld.so objects) never coalesce,
     * even when otherwise compatible, so a multi-object layout stays faithful
     * to its per-object boundaries. A split preserves map_id, so the two halves
     * of one mapping still re-merge once an mprotect makes their prot equal. */
    if (a->map_id != b->map_id)
        return false;
    if (a->backing == VMA_FILE) {
        if (a->fd != b->fd)
            return false;
        /* contiguous file region: offsets must line up with the gap */
        if (a->file_offset + (a->end - a->start) != b->file_offset)
            return false;
    }
    return true;
}

/* ---- array primitives ---- */

/*@
  requires \valid(as);
  requires 0 <= as->count <= VMA_CAP;
  requires 0 <= idx < as->count;
  requires as->vmas[idx].start < split_addr < as->vmas[idx].end;
  requires acsl_aligned(split_addr);
  assigns as->vmas[0 .. VMA_CAP - 1], as->count;
  ensures 0 <= as->count <= VMA_CAP;
  ensures \result == MM_OK ==> as->count == \old(as->count) + 1;
  // the left half keeps its start, ends at the split point
  ensures \result == MM_OK ==> as->vmas[idx].start == \old(as->vmas[idx].start);
  ensures \result == MM_OK ==> as->vmas[idx].end == split_addr;
  // the right half starts at the split point, keeps the old end
  ensures \result == MM_OK ==> as->vmas[idx + 1].start == split_addr;
  ensures \result == MM_OK ==> as->vmas[idx + 1].end == \old(as->vmas[idx].end);
  // everything before idx is untouched; everything after idx shifts up by one
  ensures \result == MM_OK ==>
    (\forall integer k; 0 <= k < idx ==> as->vmas[k] == \old(as->vmas[k]));
  ensures \result == MM_OK ==>
    (\forall integer k; idx + 1 < k <= \old(as->count) ==>
        as->vmas[k] == \old(as->vmas[k - 1]));
*/
mm_status as_split_at(struct addr_space *as, size_t idx, uint64_t split_addr)
{
    if (as->count >= VMA_CAP)
        return MM_ENOMEM;
    if (idx >= as->count)
        return MM_EINVAL;

    struct vma *v = &as->vmas[idx];
    if (!(v->start < split_addr && split_addr < v->end))
        return MM_EINVAL;
    if (!is_page_aligned(split_addr))
        return MM_EINVAL;

    /* The right half inherits everything; its file_offset advances by the
     * size of the left half. */
    struct vma right = *v;
    right.start = split_addr;
    if (v->backing == VMA_FILE)
        right.file_offset = v->file_offset + (split_addr - v->start);

    v->end = split_addr;

    /* shift tail up by one to open a slot at idx+1. The only mutation before
     * this point is `v->end = split_addr` at index idx, so for every source
     * index k-1 > idx the loop copies a value still equal to its Pre value;
     * the upper invariant is therefore phrased against Pre, which lets the
     * function-level tail-shift postcondition (ensures k>idx+1) close directly. */
    /*@
      loop invariant idx + 1 <= i <= as->count;
      loop invariant as->count < VMA_CAP;
      // slots above i hold the original (function-entry) element one lower
      loop invariant \forall integer k; i < k <= as->count ==>
          as->vmas[k] == \at(as->vmas[k - 1], Pre);
      // slots strictly below i are still untouched (post-edit) values
      loop invariant \forall integer k; 0 <= k < i ==>
          as->vmas[k] == \at(as->vmas[k], LoopEntry);
      loop assigns i, as->vmas[idx + 2 .. VMA_CAP - 1];
      loop variant i - (idx + 1);
    */
    for (size_t i = as->count; i > idx + 1; i--)
        as->vmas[i] = as->vmas[i - 1];

    as->vmas[idx + 1] = right;
    as->count++;
    return MM_OK;
}

/*@
  requires \valid(as);
  requires 0 <= as->count <= VMA_CAP;
  requires 0 <= idx <= as->count;
  assigns as->vmas[idx .. VMA_CAP - 1], as->count;
  ensures 0 <= as->count <= VMA_CAP;
  ensures \result == MM_OK ==> as->count == \old(as->count) + 1;
  ensures \result == MM_OK ==> as->vmas[idx] == v;
  ensures \result == MM_OK ==>
    (\forall integer k; 0 <= k < idx ==>
        as->vmas[k] == \old(as->vmas[k]));
  ensures \result == MM_OK ==>
    (\forall integer k; idx < k <= \old(as->count) ==>
        as->vmas[k] == \old(as->vmas[k - 1]));
*/
mm_status as_insert_at(struct addr_space *as, size_t idx, struct vma v)
{
    if (as->count >= VMA_CAP)
        return MM_ENOMEM;
    if (idx > as->count)
        return MM_EINVAL;

    /*@
      loop invariant idx <= i <= as->count;
      loop invariant as->count < VMA_CAP;
      // slots above i hold the original element one position lower
      loop invariant \forall integer k; i < k <= as->count ==>
          as->vmas[k] == \at(as->vmas[k - 1], LoopEntry);
      // slots strictly below i are still untouched originals
      loop invariant \forall integer k; 0 <= k < i ==>
          as->vmas[k] == \at(as->vmas[k], LoopEntry);
      loop assigns i, as->vmas[idx + 1 .. VMA_CAP - 1];
      loop variant i - idx;
    */
    for (size_t i = as->count; i > idx; i--)
        as->vmas[i] = as->vmas[i - 1];

    as->vmas[idx] = v;
    as->count++;
    return MM_OK;
}

/*@
  requires \valid(as);
  requires 0 <= as->count <= VMA_CAP;
  requires 0 <= lo <= hi <= as->count;
  assigns as->vmas[lo .. VMA_CAP - 1], as->count;
  ensures 0 <= as->count <= VMA_CAP;
  ensures as->count == \old(as->count) - (hi - lo);
  // elements before lo are untouched
  ensures \forall integer k; 0 <= k < lo ==>
      as->vmas[k] == \old(as->vmas[k]);
*/
void as_remove_range(struct addr_space *as, size_t lo, size_t hi)
{
    if (lo >= hi || hi > as->count)
        return;

    size_t gap = hi - lo;

    /*@
      loop invariant hi <= i <= as->count;
      loop invariant as->count <= VMA_CAP;
      loop invariant lo < hi <= as->count;
      loop invariant gap == hi - lo;
      loop invariant 1 <= gap <= lo + gap <= hi;
      // slots below lo never change
      loop invariant \forall integer k; 0 <= k < lo ==>
          as->vmas[k] == \at(as->vmas[k], LoopEntry);
      loop assigns i, as->vmas[lo .. VMA_CAP - 1];
      loop variant as->count - i;
    */
    for (size_t i = hi; i < as->count; i++)
        as->vmas[i - gap] = as->vmas[i];

    as->count -= gap;
}
