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
static mm_status split_boundaries(struct addr_space *as,
                                   uint64_t start, uint64_t end)
{
    size_t lo, hi;
    as_find_range(as, start, end, &lo, &hi);

    /* split the VMA straddling `start` (it is the first overlapper, if any) */
    if (lo < hi && as->vmas[lo].start < start && start < as->vmas[lo].end) {
        mm_status st = as_split_at(as, lo, start);
        if (st != MM_OK)
            return st;
        /* the range now begins at the new right half */
        lo++;
        hi++;
    }

    /* re-find the right boundary; split the VMA straddling `end` */
    as_find_range(as, start, end, &lo, &hi);
    if (hi > lo) {
        size_t last = hi - 1;
        if (as->vmas[last].start < end && end < as->vmas[last].end) {
            mm_status st = as_split_at(as, last, end);
            if (st != MM_OK)
                return st;
        }
    }
    return MM_OK;
}

/*@
  requires \valid(as);
  assigns *as, *out_addr;
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

    uint64_t base;
    if (flags & MAP_FIXED) {
        if (!is_page_aligned(addr))
            return MM_EINVAL;
        if (add_overflows(addr, len))
            return MM_EINVAL;
        if (addr < as->as_min || addr + len > as->as_max)
            return MM_ENOMEM;
        base = addr;
    } else {
        /* hint honored only if free; otherwise top-down first-fit */
        mm_status st = as_find_free(as, len, &base);
        if (st != MM_OK)
            return st;
    }

    uint64_t end = base + len;

    /* MAP_FIXED overlay: punch a hole over [base, end) first. */
    if (flags & MAP_FIXED) {
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
  assigns *as;
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
    for (size_t k = lo; k < hi; k++)
        as->vmas[k].prot = prot;

    as_canonicalize(as);
    return MM_OK;
}

/*@
  requires \valid(as);
  assigns *as;
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
