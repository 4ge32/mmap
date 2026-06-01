/*
 * mm_api.h - Public API of the mmap/ld.so logical simulator.
 *
 * Three operations mirror the kernel surface needed by a dynamic loader:
 * mmap (incl. PROT_NONE reservation and MAP_FIXED overlay), mprotect, munmap.
 * All operate purely on the bookkeeping model in `struct addr_space`.
 *
 * ACSL contracts are co-located with the implementations in the src tree and
 * use the predicates declared in mm_acsl.h.
 */
#ifndef MM_API_H
#define MM_API_H

#include "mm_types.h"

/* Initialize an address space to empty with default x86-64 user bounds. */
void as_init(struct addr_space *as);

/*
 * Create a mapping.
 *
 * addr   : desired address. With MAP_FIXED it must be page-aligned and the
 *          range is overlaid (existing mappings in the range are replaced).
 *          Without MAP_FIXED, addr is a hint; if 0 (or unusable) a placement
 *          is chosen deterministically (top-down first-fit).
 * length : >0; rounded up to a page multiple internally.
 * prot   : bitmask within PROT_ALL (PROT_NONE == 0).
 * flags  : subset of MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED.
 * backing/fd/offset: VMA_FILE with page-aligned offset, or VMA_ANON.
 * out_addr: on MM_OK, receives the chosen start address.
 */
mm_status mm_mmap(struct addr_space *as,
                  uint64_t addr, uint64_t length,
                  int prot, int flags,
                  enum vma_backing backing, int fd, uint64_t offset,
                  uint64_t *out_addr);

/*
 * Create a mapping, choosing its object identity (map_id).
 *
 * Identical to mm_mmap in every respect (placement, MAP_FIXED/
 * MAP_FIXED_NOREPLACE overlay, canonicalize) except for how the new VMA's
 * map_id is assigned:
 *
 *   map_id == 0 : allocate a fresh identity (as->next_map_id++), exactly like
 *                 mm_mmap. Use this for the first segment of a new object.
 *   map_id != 0 : stamp the new VMA with exactly this id and do NOT bump
 *                 next_map_id. Use this to JOIN an existing object group so
 *                 that a single mm_munmap_object(map_id) can later unload all
 *                 of an object's segments (text/rodata/data/bss) at once.
 *
 * Contract for a non-zero map_id: callers must pass either 0 (fresh) or an id
 * obtained from a prior fresh mapping in the SAME address space (e.g. read
 * back from as.vmas[k].map_id of the object's reservation). No dedup is
 * performed, so reusing an id minted by next_map_id of a different object
 * would conflate the two objects.
 *
 * mm_mmap(...) is exactly mm_mmap_obj(..., 0u, out_addr).
 */
mm_status mm_mmap_obj(struct addr_space *as,
                      uint64_t addr, uint64_t length,
                      int prot, int flags,
                      enum vma_backing backing, int fd, uint64_t offset,
                      uint32_t map_id,
                      uint64_t *out_addr);

/* Change protection on [addr, addr+length). The whole range must be mapped
 * (a gap yields MM_ENOMEM). Splits and re-merges as needed. */
mm_status mm_mprotect(struct addr_space *as,
                      uint64_t addr, uint64_t length, int prot);

/* Remove any mappings overlapping [addr, addr+length). Unmapped sub-ranges
 * are tolerated (like the kernel). Splits partial VMAs at the boundaries. */
mm_status mm_munmap(struct addr_space *as,
                    uint64_t addr, uint64_t length);

/*
 * Resize the mapping at [old_addr, old_addr+old_len) to new_len.
 *
 * This is a deliberately simplified model of Linux mremap: the source must be
 * EXACTLY ONE existing VMA spanning precisely [old_addr, old_addr+old_len)
 * (mremap operates on a whole mapping; a partial/multi-VMA span is MM_EINVAL).
 *
 * old_addr : page-aligned start of the existing mapping.
 * old_len  : >0; rounded up to a page multiple; must match the source span.
 * new_len  : >0; rounded up to a page multiple; the requested new size.
 * flags    : subset of MREMAP_MAYMOVE (unknown bits => MM_EINVAL). MREMAP_FIXED
 *            is reserved and not implemented.
 * out_addr : on MM_OK, receives the (possibly new) start address.
 *
 * Behavior:
 *  - new_len == old_len: no-op; *out_addr = old_addr.
 *  - shrink: unmaps the tail; base stays old_addr.
 *  - grow in place: if the extension range is free and in bounds, extend; the
 *    new VMA carries the source's prot/flags/backing/fd/map_id and re-merges.
 *  - grow with MREMAP_MAYMOVE: relocate the mapping (same prot/flags/backing/
 *    fd/map_id) to a freshly placed region and unmap the old range.
 *  - grow with no room and no MREMAP_MAYMOVE: MM_ENOMEM, no mutation.
 */
mm_status mm_mremap(struct addr_space *as,
                    uint64_t old_addr, uint64_t old_len,
                    uint64_t new_len, int flags,
                    uint64_t *out_addr);

/*
 * Unmap an entire shared object: remove every VMA whose .map_id equals the
 * given map_id, in one call. This models a dynamic loader's dlclose, which
 * tears down all the segments (text/rodata/data/bss overlays) that were mapped
 * for one shared object under a single logical mapping group.
 *
 * Idempotent: a map_id that matches no VMA is a no-op returning MM_OK. On
 * return no surviving VMA carries the removed map_id.
 */
mm_status mm_munmap_object(struct addr_space *as, uint32_t map_id);

#endif /* MM_API_H */
