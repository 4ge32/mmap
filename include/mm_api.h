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

/* Change protection on [addr, addr+length). The whole range must be mapped
 * (a gap yields MM_ENOMEM). Splits and re-merges as needed. */
mm_status mm_mprotect(struct addr_space *as,
                      uint64_t addr, uint64_t length, int prot);

/* Remove any mappings overlapping [addr, addr+length). Unmapped sub-ranges
 * are tolerated (like the kernel). Splits partial VMAs at the boundaries. */
mm_status mm_munmap(struct addr_space *as,
                    uint64_t addr, uint64_t length);

#endif /* MM_API_H */
