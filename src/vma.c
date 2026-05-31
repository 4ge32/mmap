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
  requires 0 <= idx < as->count;
  requires as->count < VMA_CAP;
  requires as->vmas[idx].start < split_addr < as->vmas[idx].end;
  requires acsl_aligned(split_addr);
  assigns as->vmas[0 .. VMA_CAP - 1], as->count;
  ensures \result == MM_OK ==> as->count == \old(as->count) + 1;
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

    /* shift tail up by one to open a slot at idx+1 */
    /*@
      loop invariant idx + 1 <= i <= as->count;
      loop assigns i, as->vmas[idx+1 .. as->count];
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
  requires 0 <= idx <= as->count;
  requires as->count < VMA_CAP;
  assigns as->vmas[0 .. VMA_CAP - 1], as->count;
  ensures \result == MM_OK ==> as->count == \old(as->count) + 1;
*/
mm_status as_insert_at(struct addr_space *as, size_t idx, struct vma v)
{
    if (as->count >= VMA_CAP)
        return MM_ENOMEM;
    if (idx > as->count)
        return MM_EINVAL;

    /*@
      loop invariant idx <= i <= as->count;
      loop assigns i, as->vmas[idx .. as->count];
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
  requires 0 <= lo <= hi <= as->count;
  assigns as->vmas[0 .. VMA_CAP - 1], as->count;
  ensures as->count == \old(as->count) - (hi - lo);
*/
void as_remove_range(struct addr_space *as, size_t lo, size_t hi)
{
    if (lo >= hi || hi > as->count)
        return;

    size_t gap = hi - lo;

    /*@
      loop invariant hi <= i <= as->count;
      loop assigns i, as->vmas[lo .. as->count - gap - 1];
      loop variant as->count - i;
    */
    for (size_t i = hi; i < as->count; i++)
        as->vmas[i - gap] = as->vmas[i];

    as->count -= gap;
}
