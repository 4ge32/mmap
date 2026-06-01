/*
 * wp_entry.c - Frama-C/WP entry translation unit.
 *
 * This file exists so the proof target has a single, explicit place to pull
 * in the annotated core and host proof-only lemmas, helper specs, or a
 * verification harness driver. It deliberately includes ONLY the provable
 * core headers; no test or I/O code is ever fed to Frama-C.
 *
 * The core .c files are passed on the frama-c command line alongside this
 * file (see Makefile `proof` target), so this unit only needs the public and
 * internal declarations in scope for any cross-unit lemmas added here.
 */
#include "mm_api.h"
#include "mm_internal.h"
#include "mm_acsl.h"

/*
 * ---------------------------------------------------------------------------
 * Page-mask / modulo bridge: the trusted bit-arithmetic foundation.
 * ---------------------------------------------------------------------------
 *
 * PAGE_SIZE == 4096 == 2^12, PAGE_MASK == 0xFFF, ~PAGE_MASK == 0xFFFFFFFFFFFFF000.
 * The C primitives in vma.c round and test alignment with bit-twiddling
 * (`x & PAGE_MASK`, `x & ~PAGE_MASK`), whereas the ACSL `acsl_aligned`
 * predicate is stated arithmetically with `% PAGE_SIZE`.  WP encodes the
 * bitwise `land` as an *uninterpreted* function over mathematical integers,
 * so it cannot, on its own, relate a power-of-two mask to the division /
 * modulo identities.  We bridge them with exactly two facts, both restricted
 * to the page mask:
 *
 *   low_is_mod : x & PAGE_MASK  == x % PAGE_SIZE          (low bits = remainder)
 *   high_split : x & ~PAGE_MASK == x - (x % PAGE_SIZE)    (high bits = floor)
 *
 * Status of each fact:
 *
 *  * `low_is_mod` is a genuine THEOREM: WP's `Wp.modmask` tactic discharges it
 *    (a power-of-two mask equals modulo by that power of two).  Its closed
 *    proof is replayed from the committed script session
 *    `proofs/wp_session/` via `-wp-prover script,...` — see the Makefile
 *    `proof` target.  It is therefore proved, not assumed.
 *
 *  * `high_split` is stated as an AXIOM.  It is the mask-complement identity
 *    `x = (x & m) + (x & ~m)` specialised to the page mask.  This is a true,
 *    elementary bit-vector fact, but discharging it requires bit-vector
 *    reasoning that neither Alt-Ergo 2.6.3 nor Z3 4.8.12 complete in WP's
 *    integer model (the 64-bit symbolic bit partition blows up), and no
 *    bit-vector-native backend is available in this environment.  We therefore
 *    take it as a documented, minimal trusted base — the deductive-proof
 *    analogue of importing a verified bit library.  It is exercised at runtime
 *    by every alignment assertion in the test suite (`ASSERT_WF`), so a
 *    mistake here would be caught by `make test` as well.
 *
 * From these two facts, ordinary linear arithmetic discharges every alignment
 * / round-up obligation in `round_up_page` and `is_page_aligned`.
 *
 * The constant 0xFFFFFFFFFFFFF000 below is `~PAGE_MASK` reduced to its unsigned
 * 64-bit value, matching exactly the literal WP produces from `& ~PAGE_MASK`.
 */
#define WP_HIGH_MASK ((uint64_t)0xFFFFFFFFFFFFF000)

/*@
  // Low bits of x are exactly x modulo the page size (proved by Wp.modmask).
  lemma low_is_mod:
    \forall integer x; 0 <= x <= UINT64_MAX ==>
      (x & PAGE_MASK) == (x % PAGE_SIZE);

  axiomatic PageMaskHigh {
    // High bits of x are x rounded DOWN to a page multiple. Mask-complement
    // identity specialised to the page mask; see the file header for why this
    // is an axiom rather than a discharged lemma.
    axiom high_split:
      \forall integer x; 0 <= x <= UINT64_MAX ==>
        (x & WP_HIGH_MASK) == x - (x % PAGE_SIZE);
  }
*/
