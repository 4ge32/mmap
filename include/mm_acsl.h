/*
 * mm_acsl.h - Shared ACSL logic definitions for Frama-C/WP.
 *
 * Everything here lives inside ACSL annotation comments, so a normal C
 * compiler ignores the entire file body; only Frama-C reads it. Include this
 * header in core translation units (it expands to nothing for gcc/clang).
 *
 * These predicates define the canonical-form well-formedness invariant
 * `as_wf` that every public operation must preserve, plus the helpers it
 * builds on. The runtime mirror lives in as_check_wf() (mm_internal.h).
 */
#ifndef MM_ACSL_H
#define MM_ACSL_H

#include "mm_types.h"

/*@
  // x is page-aligned.
  predicate acsl_aligned(integer x) = (x % PAGE_SIZE) == 0;

  // prot is a subset of the legal protection bits.
  predicate acsl_prot_ok(integer prot) = (prot & ~PROT_ALL) == 0;

  // Arithmetic-guard predicates, mirrored by the C helpers in vma.c. ACSL
  // terms cannot call C functions, so contracts reference these instead.
  // a + b would overflow uint64_t.
  predicate acsl_add_overflows(integer a, integer b) = a + b > UINT64_MAX;

  // Rounding len up to the next page multiple would overflow uint64_t.
  predicate acsl_round_up_overflows(integer len) =
      len + (PAGE_SIZE - 1) > UINT64_MAX;

  // A single VMA at index k of `as` is internally well-formed.
  predicate acsl_vma_ok(struct addr_space *as, integer k) =
      0 <= k < as->count &&
      as->as_min <= as->vmas[k].start &&
      as->vmas[k].start < as->vmas[k].end &&
      as->vmas[k].end <= as->as_max &&
      acsl_aligned(as->vmas[k].start) &&
      acsl_aligned(as->vmas[k].end) &&
      acsl_prot_ok(as->vmas[k].prot);

  // Two individual VMAs a, b are mergeable. Single source of truth for the
  // merge relation: acsl_mergeable(as, k) is just the pair (k, k+1), and this
  // mirrors the C runtime predicate vma_mergeable() in vma.c (incl. the map_id
  // conjunct) - keep the two in lockstep.
  predicate acsl_vma_pair_mergeable(struct vma *a, struct vma *b) =
      a->end == b->start &&
      a->prot == b->prot &&
      a->flags == b->flags &&
      a->backing == b->backing &&
      a->map_id == b->map_id &&
      (a->backing == VMA_FILE ==>
          (a->fd == b->fd &&
           a->file_offset + (a->end - a->start) == b->file_offset));

  // Two adjacent VMAs k, k+1 are mergeable (must NOT remain so in canonical form).
  predicate acsl_mergeable(struct addr_space *as, integer k) =
      acsl_vma_pair_mergeable(&as->vmas[k], &as->vmas[k+1]);

  // The whole address space is well-formed (the headline invariant):
  //  1. count within capacity
  //  2. every VMA internally ok (bounds, order, alignment, prot mask)
  //  3. sorted and pairwise disjoint
  //  4. canonical: no adjacent mergeable pair left unmerged
  predicate as_wf(struct addr_space *as) =
      0 <= as->count <= VMA_CAP &&
      as->as_min <= as->as_max &&
      (\forall integer k; 0 <= k < as->count ==> acsl_vma_ok(as, k)) &&
      (\forall integer k; 0 <= k < as->count - 1 ==>
          as->vmas[k].end <= as->vmas[k+1].start) &&
      (\forall integer k; 0 <= k < as->count - 1 ==> !acsl_mergeable(as, k));
*/

#endif /* MM_ACSL_H */
