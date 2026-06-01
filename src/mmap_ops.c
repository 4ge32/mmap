/*
 * mmap_ops.c - The three public operations: mm_mmap, mm_mprotect, mm_munmap.
 *
 * Each reduces to the array primitives in vma.c plus a canonicalize pass:
 *  - split any VMA straddling the range endpoints,
 *  - remove fully-covered VMAs (mmap overlay / munmap) or edit prot (mprotect),
 *  - insert the new VMA (mmap),
 *  - re-merge to restore canonical form.
 */
#include "mm_api.h"
#include "mm_internal.h"
#include "mm_acsl.h"

/*
 * Split VMAs so that neither `start` nor `end` falls strictly inside an
 * existing VMA. After this, every VMA is either fully inside or fully
 * outside [start, end). Needs up to two free slots.
 */
/*@
  requires \valid(as);
  requires 0 <= as->count <= VMA_CAP;
  requires start < end;
  requires acsl_aligned(start) && acsl_aligned(end);
  assigns as->vmas[0 .. VMA_CAP - 1], as->count;
  ensures 0 <= as->count <= VMA_CAP;
*/
static mm_status split_boundaries(struct addr_space *as,
                                   uint64_t start, uint64_t end)
{
    size_t lo, hi;
    as_find_range(as, start, end, &lo, &hi);

    /* Preflight: count how many boundary splits are required (0, 1, or 2)
     * from the *original* geometry, and ensure capacity up front. This keeps
     * the operation atomic: we either perform all splits or mutate nothing,
     * so a public op never returns an error with a non-canonical addr_space. */
    size_t need = 0;
    bool split_start = (lo < hi && as->vmas[lo].start < start &&
                        start < as->vmas[lo].end);
    if (split_start)
        need++;
    if (hi > lo) {
        size_t last = hi - 1;
        if (as->vmas[last].start < end && end < as->vmas[last].end)
            need++;
    }
    if (as->count + need > VMA_CAP)
        return MM_ENOMEM; /* not enough room - do not mutate */

    /* split the VMA straddling `start` (it is the first overlapper, if any) */
    if (split_start) {
        /*@ assert need >= 1; */
        /*@ assert as->count < VMA_CAP; */
        /*@ assert lo < as->count; */
        mm_status st = as_split_at(as, lo, start);
        if (st != MM_OK)
            return st; /* unreachable after preflight; defensive */
    }

    /* re-find the right boundary; split the VMA straddling `end` */
    as_find_range(as, start, end, &lo, &hi);
    if (hi > lo) {
        size_t last = hi - 1;
        if (as->vmas[last].start < end && end < as->vmas[last].end) {
            mm_status st = as_split_at(as, last, end);
            if (st != MM_OK)
                return st; /* unreachable after preflight; defensive */
        }
    }
    return MM_OK;
}

/*@
  requires \valid(as);
  requires as_wf(as);
  requires out_addr == \null || \valid(out_addr);
  requires \separated(as, out_addr);
  assigns as->vmas[0 .. VMA_CAP - 1], as->count, as->next_map_id, *out_addr;
  ensures 0 <= as->count <= VMA_CAP;
*/
mm_status mm_mmap(struct addr_space *as,
                  uint64_t addr, uint64_t length,
                  int prot, int flags,
                  enum vma_backing backing, int fd, uint64_t offset,
                  uint64_t *out_addr)
{
    if (length == 0)
        return MM_EINVAL;
    if ((prot & ~PROT_ALL) != 0)
        return MM_EINVAL;
    if ((flags & ~MAP_ALL) != 0)
        return MM_EINVAL;
    if (round_up_overflows(length))
        return MM_EINVAL;

    uint64_t len = round_up_page(length);

    if (backing == VMA_FILE) {
        if (!is_page_aligned(offset))
            return MM_EINVAL;
        if (add_overflows(offset, len))
            return MM_EINVAL;
    }

    /* MAP_FIXED_NOREPLACE places exactly like MAP_FIXED but never overlays an
     * existing mapping; the collision check happens after placement below. */
    bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;

    uint64_t base;
    if (fixed) {
        if (!is_page_aligned(addr))
            return MM_EINVAL;
        if (add_overflows(addr, len))
            return MM_EINVAL;
        if (addr < as->as_min || addr + len > as->as_max)
            return MM_ENOMEM;
        base = addr;
    } else {
        /* Honor a usable hint: page-aligned, in bounds, and the requested
         * range is currently free. Otherwise fall back to top-down first-fit. */
        bool placed = false;
        if (addr != 0 && is_page_aligned(addr) && !add_overflows(addr, len) &&
            addr >= as->as_min && addr + len <= as->as_max) {
            size_t lo, hi;
            as_find_range(as, addr, addr + len, &lo, &hi);
            if (lo == hi) { /* nothing overlaps - the hint is free */
                base = addr;
                placed = true;
            }
        }
        if (!placed) {
            /* Re-expose as_wf's per-VMA invariant (acsl_vma_ok) that
             * as_find_free needs as a precondition. */
            /*@ assert \forall integer j; 0 <= j < as->count ==>
                  acsl_vma_ok(as, j); */
            mm_status st = as_find_free(as, len, &base);
            if (st != MM_OK)
                return st;
        }
    }

    uint64_t end = base + len;
    /*@ assert len >= PAGE_SIZE; */
    /*@ assert end == base + len; */
    /*@ assert base < end; */

    /* MAP_FIXED_NOREPLACE: refuse rather than overlay if anything is already
     * mapped in [base, end). This is a no-mutation early return. */
    if (flags & MAP_FIXED_NOREPLACE) {
        size_t lo, hi;
        as_find_range(as, base, end, &lo, &hi);
        if (lo < hi)
            return MM_EEXIST;
    }

    /* MAP_FIXED overlay: punch a hole over [base, end) first. (For
     * MAP_FIXED_NOREPLACE the range is now known empty, so the hole-punch is a
     * no-op, but running it uniformly keeps the placement path simple.) */
    if (fixed) {
        mm_status st = split_boundaries(as, base, end);
        if (st != MM_OK)
            return st;
        size_t lo, hi;
        as_find_range(as, base, end, &lo, &hi);
        as_remove_range(as, lo, hi);
    }

    /* room check: the insert needs one slot */
    if (as->count >= VMA_CAP)
        return MM_ENOMEM;

    struct vma v;
    v.start = base;
    v.end = end;
    v.prot = prot;
    v.flags = flags & MAP_PERSIST_MASK;
    v.backing = backing;
    v.fd = (backing == VMA_FILE) ? fd : -1;
    v.file_offset = (backing == VMA_FILE) ? offset : 0;
    v.map_id = as->next_map_id++;

    /*@ assert base < end; */
    /*@ assert 0 <= as->count <= VMA_CAP; */
    size_t lo, hi;
    as_find_range(as, base, end, &lo, &hi);
    mm_status st = as_insert_at(as, lo, v);
    if (st != MM_OK)
        return st;

    as_canonicalize(as);

    if (out_addr)
        *out_addr = base;
    return MM_OK;
}

/*@
  requires \valid(as);
  requires as_wf(as);
  assigns as->vmas[0 .. VMA_CAP - 1], as->count;
  ensures 0 <= as->count <= VMA_CAP;
*/
mm_status mm_mprotect(struct addr_space *as,
                      uint64_t addr, uint64_t length, int prot)
{
    if (length == 0)
        return MM_EINVAL;
    if (!is_page_aligned(addr))
        return MM_EINVAL;
    if ((prot & ~PROT_ALL) != 0)
        return MM_EINVAL;
    if (round_up_overflows(length))
        return MM_EINVAL;

    uint64_t len = round_up_page(length);
    if (add_overflows(addr, len))
        return MM_EINVAL;
    uint64_t end = addr + len;

    /* The entire range must be mapped (kernel returns ENOMEM on a gap). */
    {
        uint64_t cursor = addr;
        size_t lo, hi;
        as_find_range(as, addr, end, &lo, &hi);
        /*@
          loop invariant lo <= k <= hi;
          loop invariant hi <= as->count;
          loop assigns k, cursor;
          loop variant hi - k;
        */
        for (size_t k = lo; k < hi; k++) {
            if (as->vmas[k].start > cursor)
                return MM_ENOMEM; /* gap before this VMA */
            if (as->vmas[k].end > cursor)
                cursor = as->vmas[k].end;
        }
        if (cursor < end)
            return MM_ENOMEM; /* gap at the tail */
    }

    mm_status st = split_boundaries(as, addr, end);
    if (st != MM_OK)
        return st;

    size_t lo, hi;
    as_find_range(as, addr, end, &lo, &hi);
    /*@
      loop invariant lo <= k <= hi;
      loop invariant hi <= as->count;
      loop assigns k, as->vmas[lo .. hi - 1];
      loop variant hi - k;
    */
    for (size_t k = lo; k < hi; k++)
        as->vmas[k].prot = prot;

    as_canonicalize(as);
    return MM_OK;
}

/*
 * Insert a fully-specified VMA (including a caller-chosen map_id) into a free
 * range [v.start, v.end), then re-merge. Unlike mm_mmap this does NOT stamp a
 * fresh map_id, so it can place the moved/extended half of an existing mapping
 * while preserving the mapping's identity. The caller guarantees the range is
 * currently free (used only on ranges just verified empty by as_find_range).
 */
/*@
  requires \valid(as);
  requires 0 <= as->count <= VMA_CAP;
  requires v.start < v.end;
  assigns as->vmas[0 .. VMA_CAP - 1], as->count;
  ensures 0 <= as->count <= VMA_CAP;
*/
static mm_status insert_vma_keep_id(struct addr_space *as, struct vma v)
{
    if (as->count >= VMA_CAP)
        return MM_ENOMEM;

    size_t lo, hi;
    as_find_range(as, v.start, v.end, &lo, &hi);
    mm_status st = as_insert_at(as, lo, v);
    if (st != MM_OK)
        return st;

    as_canonicalize(as);
    return MM_OK;
}

/*@
  requires \valid(as);
  requires as_wf(as);
  requires out_addr == \null || \valid(out_addr);
  requires \separated(as, out_addr);
  assigns as->vmas[0 .. VMA_CAP - 1], as->count, *out_addr;
  ensures 0 <= as->count <= VMA_CAP;
*/
mm_status mm_mremap(struct addr_space *as,
                    uint64_t old_addr, uint64_t old_len,
                    uint64_t new_len, int flags,
                    uint64_t *out_addr)
{
    if (old_len == 0 || new_len == 0)
        return MM_EINVAL;
    if (!is_page_aligned(old_addr))
        return MM_EINVAL;
    if ((flags & ~MREMAP_MAYMOVE) != 0)
        return MM_EINVAL;
    if (round_up_overflows(old_len) || round_up_overflows(new_len))
        return MM_EINVAL;

    uint64_t olen = round_up_page(old_len);
    uint64_t nlen = round_up_page(new_len);
    if (add_overflows(old_addr, olen))
        return MM_EINVAL;
    uint64_t old_end = old_addr + olen;

    /* Locate the source: exactly one VMA spanning precisely the old range. */
    size_t lo, hi;
    as_find_range(as, old_addr, old_end, &lo, &hi);
    if (!(hi == lo + 1))
        return MM_EINVAL;
    /*@ assert lo < as->count; */
    if (as->vmas[lo].start != old_addr || as->vmas[lo].end != old_end)
        return MM_EINVAL;

    /* Capture the source mapping's identity/properties. */
    struct vma src = as->vmas[lo];

    /* Case 1: no change in size. */
    if (nlen == olen) {
        if (out_addr)
            *out_addr = old_addr;
        return MM_OK;
    }

    /* Case 2: shrink - unmap the tail, base stays put. */
    if (nlen < olen) {
        /* old_addr + nlen < old_end <= AS_MAX, so no overflow. */
        mm_status st = mm_munmap(as, old_addr + nlen, olen - nlen);
        if (st != MM_OK)
            return st;
        if (out_addr)
            *out_addr = old_addr;
        return MM_OK;
    }

    /* Case 3: grow (nlen > olen). */
    if (add_overflows(old_addr, nlen))
        return MM_ENOMEM;
    uint64_t new_end = old_addr + nlen;

    /* 3a: in-place extension if the gap [old_end, new_end) is free and in
     * bounds. The extension carries the SAME map_id/prot/flags/backing/fd, so
     * canonicalize re-merges it into the source VMA. */
    if (new_end <= as->as_max) {
        size_t glo, ghi;
        as_find_range(as, old_end, new_end, &glo, &ghi);
        if (glo == ghi) {
            struct vma ext = src;
            ext.start = old_end;
            ext.end = new_end;
            if (src.backing == VMA_FILE)
                ext.file_offset = src.file_offset + (old_end - src.start);
            else
                ext.file_offset = 0;
            /*@ assert ext.start < ext.end; */
            mm_status st = insert_vma_keep_id(as, ext);
            if (st != MM_OK)
                return st;
            if (out_addr)
                *out_addr = old_addr;
            return MM_OK;
        }
    }

    /* 3b: relocate if MREMAP_MAYMOVE is set. */
    if (flags & MREMAP_MAYMOVE) {
        /* Re-expose as_wf's per-VMA invariant for as_find_free. */
        /*@ assert \forall integer j; 0 <= j < as->count ==>
              acsl_vma_ok(as, j); */
        uint64_t new_base;
        mm_status st = as_find_free(as, nlen, &new_base);
        if (st != MM_OK)
            return st;
        /*@ assert new_base + nlen <= as->as_max; */

        /* Unmap the old range FIRST, while as_wf still holds (mm_munmap
         * requires it). The moved copy is inserted afterward; doing it in this
         * order keeps every as_wf-requiring call on a well-formed input. */
        st = mm_munmap(as, old_addr, olen);
        if (st != MM_OK)
            return st;

        struct vma moved = src;
        moved.start = new_base;
        moved.end = new_base + nlen;
        moved.file_offset = (src.backing == VMA_FILE) ? src.file_offset : 0;
        /*@ assert moved.start < moved.end; */

        st = insert_vma_keep_id(as, moved);
        if (st != MM_OK)
            return st;

        if (out_addr)
            *out_addr = new_base;
        return MM_OK;
    }

    /* 3c: grow, no room in place, no MREMAP_MAYMOVE. */
    return MM_ENOMEM;
}

/*@
  requires \valid(as);
  requires as_wf(as);
  assigns as->vmas[0 .. VMA_CAP - 1], as->count;
  ensures 0 <= as->count <= VMA_CAP;
*/
mm_status mm_munmap(struct addr_space *as,
                    uint64_t addr, uint64_t length)
{
    if (length == 0)
        return MM_EINVAL;
    if (!is_page_aligned(addr))
        return MM_EINVAL;
    if (round_up_overflows(length))
        return MM_EINVAL;

    uint64_t len = round_up_page(length);
    if (add_overflows(addr, len))
        return MM_EINVAL;
    uint64_t end = addr + len;

    mm_status st = split_boundaries(as, addr, end);
    if (st != MM_OK)
        return st;

    size_t lo, hi;
    as_find_range(as, addr, end, &lo, &hi);
    as_remove_range(as, lo, hi);

    as_canonicalize(as); /* removal cannot create new mergeables, but cheap */
    return MM_OK;
}

/*
 * Unmap an entire shared object: drop every VMA whose map_id matches, in a
 * single left-to-right compaction pass (write-cursor `w` copies kept elements
 * down, exactly like as_canonicalize). Removal only deletes elements and
 * preserves the relative order of the survivors, so the result stays sorted
 * and disjoint. Unlike the other ops we deliberately do NOT run an
 * as_canonicalize pass afterward: dropping VMAs can only open gaps between the
 * survivors, never create a newly-mergeable adjacent pair (any pair that was
 * non-mergeable before is still non-mergeable, and any pair separated by a now
 * removed VMA had distinct map_ids and stays disjoint). Canonicalizing would
 * also re-frame the array under its own (weaker) contract, defeating the
 * no-matching-map_id postcondition that is the point of this operation.
 */
/*@
  requires \valid(as);
  requires as_wf(as);
  assigns as->vmas[0 .. VMA_CAP - 1], as->count;
  ensures 0 <= as->count <= VMA_CAP;
  ensures \forall integer k; 0 <= k < as->count ==> as->vmas[k].map_id != map_id;
*/
mm_status mm_munmap_object(struct addr_space *as, uint32_t map_id)
{
    size_t w = 0; /* write cursor: vmas[0..w) is the compacted survivor prefix */

    /*@
      loop invariant 0 <= w <= i <= as->count;
      loop invariant as->count <= VMA_CAP;
      loop invariant \forall integer j; 0 <= j < w ==>
          as->vmas[j].map_id != map_id;
      loop assigns i, w, as->vmas[0 .. VMA_CAP - 1];
      loop variant as->count - i;
    */
    for (size_t i = 0; i < as->count; i++) {
        if (as->vmas[i].map_id != map_id) {
            as->vmas[w] = as->vmas[i]; /* keep this VMA, copy it down */
            w++;
        }
    }

    as->count = w;
    return MM_OK;
}
