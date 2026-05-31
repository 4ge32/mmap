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

  // Two adjacent VMAs k, k+1 are mergeable (must NOT remain so in canonical form).
  predicate acsl_mergeable(struct addr_space *as, integer k) =
      as->vmas[k].end == as->vmas[k+1].start &&
      as->vmas[k].prot == as->vmas[k+1].prot &&
      as->vmas[k].flags == as->vmas[k+1].flags &&
      as->vmas[k].backing == as->vmas[k+1].backing &&
      (as->vmas[k].backing == VMA_FILE ==>
          (as->vmas[k].fd == as->vmas[k+1].fd &&
           as->vmas[k].file_offset + (as->vmas[k].end - as->vmas[k].start)
               == as->vmas[k+1].file_offset));

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
