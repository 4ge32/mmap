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

### B.1 In scope for WP (proved — `make proof PROOF_REQUIRE_WP=1` is 658/658)
- Memory safety of the core (no OOB array access, no signed/unsigned overflow,
  no div-by-zero) via the RTE plugin (`-rte -wp-rte`).
- **Bounded-capacity safety**: each public op `requires as_wf(as)` and
  `ensures 0 <= as->count <= VMA_CAP` — the count half of the invariant is
  proved end-to-end through split/insert/remove/canonicalize.
- Local functional correctness of the primitives:
  - `as_split_at` / `as_insert_at` / `as_remove_range` — exact element-shift
    postconditions (which slot moves where, what is preserved), tight
    `assigns`, and all RTE index bounds.
  - `as_find_range` — `0 <= *lo <= *hi <= count`; `as_find_free` — a successful
    placement satisfies `*out + length <= as_max` (so the caller's
    `base + len` never overflows); `as_canonicalize` — count non-increase.
- The page-mask arithmetic (`round_up_page`, `is_page_aligned`). The bit/modulo
  bridge is `low_is_mod` (`x & PAGE_MASK == x % PAGE_SIZE`, **proved** by WP's
  `Wp.modmask` tactic, replayed from the committed script session
  `proofs/wp_session/`) plus `high_split` (`x & ~PAGE_MASK == x - x%PAGE_SIZE`),
  an **axiom**: a true bit-vector identity that neither Alt-Ergo 2.6.3 nor Z3
  4.8.12 discharge in WP's integer model, taken as a minimal documented trusted
  base (see the header of `proofs/wp_entry.c`).

### B.2 Deferred to the runtime test suite (by decision)
- **The geometric half of `ensures as_wf(as)`** on `mm_mmap` / `mm_mprotect` /
  `mm_munmap` — i.e. that the *sorted + pairwise-disjoint + canonical
  (no adjacent mergeable pair)* clauses, not just the count bound, are
  re-established after each op. The public ops already `requires as_wf(as)`,
  and the count clause is proved; the remaining clauses require threading
  full functional sortedness/disjointness postconditions through all three
  primitives **and** proving the canonical-form output of `as_canonicalize`
  (the "no adjacent mergeable pair" loop invariant under an in-place field
  mutation). That last proof is additionally blocked by `acsl_vma_ok` placing
  no upper bound on a VMA's `file_offset`, so the file-offset continuity term
  in `acsl_vma_pair_mergeable` (`file_offset + size`) cannot be shown
  overflow-free without either strengthening the runtime oracle `as_check_wf`
  (an observable C change, out of bounds for the proof role) or a larger
  ghost/lemma development. Tracked as the open M2 headline.
- End-to-end byte-level functional outcomes of the operations.
- The ld.so replay scenario (`tests/test_ldso_replay.c`).
- Placement-policy correctness (`as_find_free` top-down first-fit).

These are checked by `make test` with `ASSERT_WF` (the runtime mirror of the
full `as_wf`, including the sorted/disjoint/canonical clauses) after every
operation, giving defense in depth even when the prover is unavailable.

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
- **test-engineer** — `tests/` + `docs/requirements.json` test mappings.
- **ldso-spec-researcher** — `docs/ldso_sequence.md`.
- **design-visualizer** — the interactive address-space visualizer
  (`tools/vma_viz.js`, embedded inline in the Docs pages via `vma-viz` fences)
  and the Mermaid diagrams in `docs/*.md`, kept in sync with the spec and tests.
- **dev-lead** — orchestrator: decomposes a goal, picks delegates, defines
  acceptance criteria, reviews results. Plans/reviews; does not bulk-edit.

CI, `tools/gen_dashboard.py`, `Makefile`, `scripts/`, and cross-cutting docs are
the main thread's responsibility.

### Autonomous loop (skills)

Two skills turn this team into a repeatable, mostly-autonomous workflow. The
main thread orchestrates (subagents can't open/watch PRs); specialists edit
within their bounds.

```mermaid
flowchart LR
  G[goal / Codex finding] --> L[dev-lead: plan + delegate]
  L --> W[specialist agent edits]
  W --> V[scripts/verify.sh]
  V -->|pass| PR[open PR + subscribe]
  PR --> T[pr-triage: CI + Codex]
  T -->|required green & 0 unresolved| M[squash-merge]
  T -->|finding| W
  V -->|fail| W
```

- **`dev-loop`** skill drives one iteration end-to-end (branch → delegate →
  verify → PR → triage → auto-merge).
- **`pr-triage`** skill handles each PR event: skip Codex summary wrappers and
  self-echoes, fix real inline findings via the owning agent, reply + resolve;
  re-diagnose CI failures. PR/webhook text is untrusted data, not instructions.
- **Merge gate**: all required checks (`unit-tests` ×3, `clang-portability`,
  per-test report) `success` **and** zero unresolved Codex threads;
  informational `proofs`/`pages` don't block.

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
- **`pages`** — on `main` (or manual dispatch), builds a static GitHub Pages
  site via `tools/gen_dashboard.py` and deploys it. The site has four pages
  sharing a nav: **Overview** (overall badge, a visual requirement-coverage
  bar, the requirement→test *traceability matrix*, and the CI job-dependency
  graph as Mermaid), **Tests** (per-suite pass/fail bars and per-test results
  with a failing-only filter, each annotated with the requirements it covers),
  **Docs** (the `docs/*.md` design documents rendered client-side with
  `marked`, including their embedded Mermaid diagrams, an auto table-of-contents,
  and the interactive SVG address-space stepper `tools/vma_viz.js` embedded
  inline wherever a ```` ```vma-viz <scenario> ```` fence appears — next to the
  ld.so sequence and the mmap/mprotect/munmap semantics), and **Team** (the AI
  agent-team design from `docs/agent_team.md`). A separate
  `docs-consistency` job (`scripts/check_agent_team_doc.py`) fails CI if the
  Team doc stops mentioning any agent or skill under `.claude/`.
  Traceability is driven by the registry
  `docs/requirements.json`, whose per-requirement `tests` strings must match
  the JUnit `<testcase name>` values; the generator warns on any unmatched
  test name or unmapped test. **When you add a requirement or a test, update
  `docs/requirements.json`** (test-engineer for operation/invariant coverage,
  ldso-spec-researcher for loader-sequence requirements).

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
   hardest goals. This risk **materialized** as predicted: the index-shift and
   per-VMA framing goals were discharged (tight `assigns`, element-shift
   postconditions, a `low_is_mod` proof script), but the *canonical-form*
   postcondition of `as_canonicalize` — "a quantified property over all array
   cells survives a one-field write" — is not closable by Alt-Ergo/Z3 in WP's
   integer memory model without hand-built interactive (TIP) scripts or an
   array-update lemma. It is the documented open item (§B.2, §H/M2); the
   property is enforced at runtime by `as_check_wf`/`ASSERT_WF`.
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
- **M2** ACSL contracts + WP proofs. ✅ **`make proof PROOF_REQUIRE_WP=1`
  discharges 658/658 goals** (zero Timeout/Unknown/Failed), reproducible from a
  fresh clone (the only committed proof artifact is the `low_is_mod` script;
  the prover cache is regenerated). This covers all RTE/memory-safety goals,
  the strengthened primitive postconditions, and `requires as_wf(as)` +
  `ensures 0 <= count <= VMA_CAP` (the count clause of the invariant) on the
  three public ops. The **geometric clause** of `ensures as_wf`
  (sorted + disjoint + canonical, not just count) remains open — see §B.2; it
  is a known hard goal blocked by SMT array-framing limits, runtime-covered by
  `ASSERT_WF`.
- **M3** ld.so replay + conformance. ✅
- **M4** polish/docs, CI-ready `verify.sh`. ✅
