# Design: Verified mmap/ld.so Logical Simulator

## Context

Goal: a C-language simulator of Linux `mmap` semantics, faithful enough to
model how a dynamic loader (`ld-linux` / `ld.so`) maps a shared object —
specifically `PROT_NONE` reservation with multiple overlaid mappings and
`mprotect`-based protection switching. The implementation is verified two
ways: a runtime unit-test suite and Frama-C/WP deductive proofs.

## A. Architecture (the C model)

Pure logical model: **no real memory is allocated.** The virtual address space
is a `struct addr_space` holding a fixed-capacity array `vmas[VMA_CAP]` of VMAs
kept in canonical form (sorted, disjoint, page-aligned, maximally merged).
See `docs/mmap_model_spec.md` for exact semantics.

**Array, not linked list** — deliberate, for WP tractability: `assigns` clauses
reference `as->vmas[..]` and `as->count` with no heap/separation reasoning, and
loop invariants map directly to array indices. The cost (O(count) shifts,
fixed capacity → `MM_ENOMEM` when full) is irrelevant at ld.so scale (count
≪ 20).

Files: `include/mm_types.h` (types/constants), `include/mm_api.h` (public API),
`include/mm_internal.h` (primitives + oracle), `src/vma.c` (primitives +
arithmetic guards), `src/addr_space.c` (container ops + `as_check_wf`),
`src/mmap_ops.c` (the three operations).

## B. Verification strategy (Frama-C/WP + ACSL)

### B.1 In scope for WP
- Memory safety of the core (no OOB array access, no overflow, no div-by-zero)
  via the RTE plugin (`-rte -wp-rte`).
- **Invariant preservation**: each public op, `requires as_wf ⇒ ensures as_wf`.
- Local functional correctness of the primitives (split union, insert/remove
  shifts, merge count reduction) and bounded-capacity safety.

### B.2 Deferred to the runtime test suite (by decision)
- End-to-end byte-level functional outcomes of the operations.
- The ld.so replay scenario (`tests/test_ldso_replay.c`).
- Placement-policy correctness (`as_find_free` top-down first-fit).

These are checked by `make test` with `ASSERT_WF` after every operation, giving
defense in depth even when the prover is unavailable.

### B.3 Mechanics
ACSL predicates (`as_wf`, `acsl_mergeable`, `acsl_aligned`, `acsl_prot_ok`)
live in `include/mm_acsl.h` inside annotation comments — invisible to gcc/clang,
read only by Frama-C. Function contracts are co-located above each function.
The static `struct addr_space` array is the only storage; **no malloc in the
core** is the single most important rule for keeping WP happy.

## C. Repository layout

```
include/   mm_types.h, mm_api.h, mm_internal.h, mm_acsl.h
src/       vma.c, addr_space.c, mmap_ops.c        (provable core)
tests/     test_harness.h, test_vma.c, test_mmap_ops.c, test_ldso_replay.c
proofs/    wp_entry.c, README.md
tools/     vma_dump.c                              (debug only, not proven)
docs/      design.md, mmap_model_spec.md, ldso_sequence.md
scripts/   verify.sh, bootstrap.sh
.claude/   agents/*.md, settings.json
```

## D. Agents (Claude Code subagents)

Strict editing boundaries keep reviews tractable:
- **simulator-implementer** — `src/`, `include/` (logic).
- **proof-engineer** — ACSL bodies + `proofs/` (specs only).
- **test-engineer** — `tests/`.
- **ldso-spec-researcher** — `docs/ldso_sequence.md`.

## E. Harness

`scripts/verify.sh` is two-tier and returns a single pass/fail: Tier 1 (build +
unit tests) is mandatory and always runs; Tier 2 (Frama-C/WP) runs only when
`frama-c` is on PATH and otherwise SKIPs cleanly. `VERIFY_REQUIRE_PROOF=1`
promotes a SKIP to a hard failure for CI gates that mandate proofs. The
`SessionStart` hook (`scripts/bootstrap.sh`) reports available tiers and makes
a non-fatal, idempotent attempt to install Frama-C.

CI (`.github/workflows/ci.yml`) is structured so each test surfaces in the PR
status checks (which are per-job):

- **`unit-tests`** — a matrix with **one job per suite**, so each appears as its
  own self-explanatory status check (`CI / VMA primitives…`, `CI / mmap…`,
  `CI / ld.so mapping-sequence replay…`). Each runs the suite under gcc (with
  `JUNIT_XML` set so the harness emits a JUnit report) and under ASan/UBSan via
  the per-suite `asan-test-*` targets, and uploads the report as an artifact.
- **`clang-portability`** — builds and runs the full suite with clang.
- **`test-report`** — consumes the JUnit artifacts and publishes a per-test
  breakdown (each test function as a row) in the Checks UI via
  `EnricoMi/publish-unit-test-result-action`.
- **`proofs`** — *informational* (`continue-on-error`): installs Frama-C +
  provers and runs `make proof`. The M2 goals are not all discharged yet and
  prover availability varies; promote to a required check once they are.

The test harness (`tests/test_harness.h`) backs this: `RUN_TEST(fn, "desc")`
records each test with a human-readable description, prints a `[PASS]/[FAIL]`
line (plus GitHub Actions `::group::`/`::error::` annotations under CI), and
writes JUnit XML when `JUNIT_XML` is set. `make test` / `scripts/verify.sh`
remain the local two-tier entry points.

## F. Conventions

C11; strict flags (`-Wall -Wextra -Werror -Wconversion -Wsign-conversion
-Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes -fno-common`); a
WP-friendly safety subset (no dynamic allocation or recursion in the core,
bounded loops with ACSL variants, explicit `uint64_t` for addresses/sizes,
overflow guards before every page computation). Naming: `snake_case` with
`mm_`/`vma_`/`as_` prefixes; ACSL contracts co-located with implementations.

## G. Risks

1. **WP proof of merge/split under the array model** — the canonical-form
   postcondition after `as_canonicalize` and the index-shift loops are the
   hardest goals. Mitigation: prove primitives in isolation with tight
   `assigns`; keep merge a single bounded pass; ghost state only if needed.
2. **Page-alignment / address overflow** — guarded by `add_overflows` /
   `round_up_overflows` plus RTE; rejected before use.
3. **Toolchain absence / restricted network** — the two-tier harness keeps
   build+test always gating; proofs are additive and gracefully skipped; ACSL
   is authored regardless.
4. **malloc creeping into the core** — forbidden by convention and enforced by
   the simulator-implementer agent's constraints.

## H. Milestones

- **M0** foundation & harness (scaffolding, Makefile, verify/bootstrap, agents,
  docs). ✅
- **M1** core VMA model + invariants + unit tests, `ASSERT_WF` everywhere. ✅
- **M2** ACSL contracts + WP proofs (runs when frama-c available; contracts
  authored now). 🟡 contracts in place; proof discharge pending a prover.
- **M3** ld.so replay + conformance. ✅
- **M4** polish/docs, CI-ready `verify.sh`. ✅
