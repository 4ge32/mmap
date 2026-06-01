# Proofs (Frama-C/WP + ACSL)

This directory drives deductive verification of the simulator core.

## What is proved

The headline property is **invariant preservation**: each public operation
satisfies `requires as_wf(as)` ⇒ `ensures as_wf(as)`, where `as_wf` (defined
in [`include/mm_acsl.h`](../include/mm_acsl.h)) captures canonical form —
sorted, pairwise-disjoint, page-aligned, prot-mask-valid, in-bounds VMAs with
no adjacent mergeable pair left unmerged.

WP, with the RTE plugin (`-rte -wp-rte`), additionally discharges runtime-error
obligations: no out-of-bounds array access, no signed/unsigned overflow in the
page arithmetic, no division by zero.

Current status: `make proof PROOF_REQUIRE_WP=1` discharges **658 / 658** goals
(zero Timeout/Unknown/Failed). All three public ops carry `requires as_wf(as)`
and prove `ensures 0 <= as->count <= VMA_CAP` end-to-end, plus the strengthened
primitive postconditions and all RTE goals. The *geometric* half of
`ensures as_wf` (sorted/disjoint/canonical) on the public ops is the remaining
open M2 headline, documented in `docs/design.md` §B.2; functional byte-level
outcomes and the ld.so replay scenario are likewise covered by the runtime test
suite, not by WP.

### Page-mask foundation and the script session

`proofs/wp_session/` holds one committed WP proof script,
`script/lemma_low_is_mod.json`, which discharges the lemma
`low_is_mod : x & PAGE_MASK == x % PAGE_SIZE` via the `Wp.modmask` tactic (WP's
SMT back-ends do not prove it directly). The Makefile `proof` target therefore
runs with `-wp-session proofs/wp_session -wp-prover script,z3,alt-ergo` so the
script is replayed. The companion fact `high_split`
(`x & ~PAGE_MASK == x - x%PAGE_SIZE`) is an `axiom` in `proofs/wp_entry.c` — a
true bit-vector identity outside the reach of Alt-Ergo/Z3 in WP's integer
model; see that file's header for the rationale.

`-no-warn-unaligned-pointer` (in `FRAMAC_FLAGS`) suppresses RTE `\aligned(...)`
pointer-alignment alarms, which WP cannot model ("\aligned not yet implemented")
and which carry no real content for in-array struct-element addresses.

## Running

```sh
make proof          # full WP proofs if WP is present; else parse+RTE fallback
make proof-parse    # ACSL parse + RTE generation only (needs just frama-c-base)
make proof PROOF_REQUIRE_WP=1   # hard-fail unless the WP plugin actually ran
```

`make proof` has three tiers, chosen automatically:

1. **`frama-c` absent** → `[SKIP]` (exit 0). The build+test loop is never
   blocked by a missing prover.
2. **`frama-c` present but WP plugin absent** → runs `make proof-parse`, which
   parses and type-checks every ACSL contract and generates the RTE
   obligations. This catches malformed annotations even without a prover, and
   fails on any annotation error. (The Ubuntu apt `frama-c` package ships the
   kernel + RTE but **not** WP; WP comes via opam.)
3. **WP plugin present** → full deductive proof; fails on any unproven goal.

Set **`PROOF_REQUIRE_WP=1`** to turn tiers 1 and 2 into a hard failure: the
command then errors unless the real WP plugin is present and runs. CI's
`proofs` job uses this so a green check genuinely means WP ran — a broken
prover install can no longer hide behind a passing parse-fallback. The job
stays `continue-on-error` (informational) while M2 goal discharge is in
progress.

Direct invocation of the full proof:

```sh
frama-c -machdep gcc_x86_64 -cpp-extra-args="-Iinclude -DFRAMA_C" \
        -rte -wp -wp-rte -wp-prover z3,alt-ergo -wp-timeout 20 \
        src/vma.c src/addr_space.c src/mmap_ops.c proofs/wp_entry.c
```

## Reading goal reports

WP prints a per-function goal summary. Any line reporting `Unknown` or
`Timeout` is an undischarged goal: either strengthen the contract / loop
invariant, raise `-wp-timeout`, or document the goal as test-covered in
`docs/design.md`.

## Toolchain

Requires `frama-c` **with the WP plugin** plus a prover backend (`z3` and/or
`alt-ergo`). On Ubuntu, `apt` provides only `frama-c-base` (kernel + RTE, no
WP); the WP plugin and Alt-Ergo come via **opam** (`opam install frama-c
alt-ergo`). The CI `proofs` job installs them this way; locally, `make proof`
falls back to the parse+RTE check when only `frama-c-base` is available.

## ACSL authoring note

In `assigns`/range clauses, keep spaces around `..` range bounds and operators
that involve macros, e.g. `assigns as->vmas[0 .. VMA_CAP - 1];`. Writing
`VMA_CAP-1` (no spaces) can defeat macro expansion in Frama-C's separate
annotation preprocessor and yields a spurious "unbound logic variable" error.
