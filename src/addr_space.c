/*
 * addr_space.c - Address-space container operations: init, range search,
 * canonicalization (merge pass), free-region placement, and the runtime
 * well-formedness oracle.
 */
#include "mm_internal.h"
#include "mm_acsl.h"

/*@
  requires \valid(as);
  assigns *as;
  ensures as->count == 0;
  ensures as->as_min == AS_MIN && as->as_max == AS_MAX;
*/
void as_init(struct addr_space *as)
{
    as->count = 0;
    as->as_min = AS_MIN;
    as->as_max = AS_MAX;
    as->next_map_id = 1u;
}

/*@
  requires \valid(as) && \valid(lo) && \valid(hi);
  requires start < end;
  assigns *lo, *hi;
  ensures *lo <= *hi <= as->count;
*/
void as_find_range(const struct addr_space *as, uint64_t start, uint64_t end,
                   size_t *lo, size_t *hi)
{
    size_t l = 0;
    /* first VMA whose end is strictly past `start` (i.e. could overlap) */
    /*@
      loop invariant 0 <= l <= as->count;
      loop assigns l;
      loop variant as->count - l;
    */
    while (l < as->count && as->vmas[l].end <= start)
        l++;

    size_t h = l;
    /* extend while VMAs start before `end` (still overlapping the range) */
    /*@
      loop invariant l <= h <= as->count;
      loop assigns h;
      loop variant as->count - h;
    */
    while (h < as->count && as->vmas[h].start < end)
        h++;

    *lo = l;
    *hi = h;
}

/*
 * Restore canonical form by merging adjacent mergeable VMAs in a single
 * left-to-right compaction pass. Because the input is already sorted and
 * disjoint, one pass is sufficient: merging is transitive along the chain.
 */
/*@
  requires \valid(as);
  assigns as->vmas[0..VMA_CAP-1], as->count;
  ensures as->count <= \old(as->count);
*/
void as_canonicalize(struct addr_space *as)
{
    if (as->count <= 1)
        return;

    size_t w = 0; /* write cursor: vmas[0..w] is the compacted prefix */

    /*@
      loop invariant 1 <= i <= as->count;
      loop invariant 0 <= w < i;
      loop assigns i, w, as->vmas[0 .. as->count - 1];
      loop variant as->count - i;
    */
    for (size_t i = 1; i < as->count; i++) {
        if (vma_mergeable(&as->vmas[w], &as->vmas[i])) {
            as->vmas[w].end = as->vmas[i].end; /* absorb right into prefix */
        } else {
            w++;
            as->vmas[w] = as->vmas[i];
        }
    }

    as->count = w + 1;
}

/*
 * Top-down first-fit placement: find the highest page-aligned hole of size
 * `length` within (as_min, as_max). Returns MM_OK and *out on success.
 */
/*@
  requires \valid(as) && \valid(out);
  requires acsl_aligned(length) && length > 0;
  assigns *out;
*/
mm_status as_find_free(const struct addr_space *as, uint64_t length,
                       uint64_t *out)
{
    /* Scan gaps from the top of the address space downward. The candidate
     * upper bound starts at as_max and walks down past each VMA. */
    uint64_t hi = as->as_max;

    /*@
      loop invariant 0 <= k <= as->count;
      loop assigns hi, *out, k;
      loop variant k;
    */
    for (size_t k = as->count; ; k--) {
        uint64_t lo = (k == 0) ? as->as_min : as->vmas[k - 1].end;
        if (hi >= length && hi - length >= lo) {
            *out = hi - length; /* place as high as possible in this gap */
            return MM_OK;
        }
        if (k == 0)
            break;
        hi = as->vmas[k - 1].start;
    }
    return MM_ENOMEM;
}

/* ---- runtime well-formedness oracle (mirror of ACSL as_wf) ---- */

bool as_check_wf(const struct addr_space *as)
{
    if (as->count > VMA_CAP)
        return false;
    if (as->as_min > as->as_max)
        return false;

    for (size_t k = 0; k < as->count; k++) {
        const struct vma *v = &as->vmas[k];
        if (!(as->as_min <= v->start))
            return false;
        if (!(v->start < v->end))
            return false;
        if (!(v->end <= as->as_max))
            return false;
        if (!is_page_aligned(v->start) || !is_page_aligned(v->end))
            return false;
        if ((v->prot & ~PROT_ALL) != 0)
            return false;
        if (v->backing == VMA_FILE && !is_page_aligned(v->file_offset))
            return false;
    }

    for (size_t k = 0; k + 1 < as->count; k++) {
        /* sorted and disjoint */
        if (!(as->vmas[k].end <= as->vmas[k + 1].start))
            return false;
        /* canonical: no adjacent mergeable pair left unmerged */
        if (vma_mergeable(&as->vmas[k], &as->vmas[k + 1]))
            return false;
    }

    return true;
}
