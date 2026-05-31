# mmap — a verified logical simulator of Linux `mmap`

A C-language **logical simulator of Linux `mmap` semantics**, faithful enough to
model how a dynamic loader (`ld-linux` / `ld.so`) maps a shared object:
`PROT_NONE` reservation, multiple `MAP_FIXED` overlays into that reservation, and
`mprotect`-based protection switching.

It is **pure bookkeeping — no real memory is allocated.** The virtual address
space is a fixed-capacity, sorted, non-overlapping, maximally-merged (canonical)
array of VMAs. Correctness is established two ways:

- a **runtime unit-test suite** (zero-dependency harness, `ASSERT_WF` after every
  operation), and
- **Frama-C/WP deductive proofs** of the same well-formedness invariant (`as_wf`),
  authored as ACSL contracts co-located with the code.

## Quick start

```sh
make all          # build the core object files
make test         # build + run all unit tests
make test-asan    # run the tests under ASan/UBSan
make proof        # Frama-C/WP proofs; SKIPs cleanly if frama-c is absent
make verify       # scripts/verify.sh: two-tier (tests [+ proofs]) pass/fail

# run a single suite (also: test-vma / test-ldso-replay):
make test-mmap-ops
```

`gcc`/`make` are always available, so **build + tests always work**. The proof
tier needs `frama-c` with the WP plugin plus a prover (`z3`/`alt-ergo`); when
they're absent, `make proof` and the harness SKIP gracefully — this is expected,
not a failure. ACSL annotations are authored regardless, so the core stays
"proof-ready".

## Architecture

- **Provable core** (`src/`, fed to Frama-C): `vma.c` (primitives + arithmetic
  guards), `addr_space.c` (container ops + the `as_check_wf` runtime oracle),
  `mmap_ops.c` (`mm_mmap` / `mm_mprotect` / `mm_munmap`). The three operations
  all reduce to the primitives `as_split_at` / `as_insert_at` / `as_remove_range`
  plus an `as_canonicalize` merge pass.
- **Headers** (`include/`): `mm_types.h`, `mm_api.h`, `mm_internal.h`, and
  `mm_acsl.h` (ACSL predicates, incl. the central `as_wf` invariant — invisible
  to the C compiler, read only by Frama-C).
- **Tests/tools** (never fed to Frama-C): the `tests/` harness and suites
  including the `tests/test_ldso_replay.c` loader-sequence scenario.

The `as_wf` invariant is the linchpin: defined once as an ACSL predicate
(`mm_acsl.h`), mirrored at runtime (`as_check_wf`), and asserted after every
operation in tests via `ASSERT_WF`. The three are kept in lockstep.

Full design: [`docs/design.md`](docs/design.md) · model semantics:
[`docs/mmap_model_spec.md`](docs/mmap_model_spec.md) · loader sequence:
[`docs/ldso_sequence.md`](docs/ldso_sequence.md).

## Dashboard (GitHub Pages)

CI publishes a static site with five pages: an **Overview** (pass/fail badge,
visual requirement-coverage bar, requirement→test traceability matrix, CI
job-dependency graph), **Tests** (per-suite bars + per-test results with a
failing-only filter), an interactive **Visualize** tab (steps through how the
operations reshape the VMA list), **Docs** (the design specs with an auto
table-of-contents), and **Team** (the AI agent-team design). Build it locally:

```sh
python3 tools/gen_dashboard.py --reports reports --requirements docs/requirements.json --docs docs --out site
python3 -m http.server -d site   # serve over HTTP so Docs/Team can fetch docs/*.md
```

## AI agent team

This repository is developed by a team of Claude Code agents driving an
autonomous loop — `dev-lead` plans and delegates to five specialists
(`simulator-implementer`, `proof-engineer`, `test-engineer`,
`ldso-spec-researcher`, `design-visualizer`), and the `dev-loop` / `pr-triage`
skills carry a change from branch through verification, PR, CI/review handling,
and merge. See [`docs/agent_team.md`](docs/agent_team.md) (rendered on the Pages
**Team** tab) and [`CLAUDE.md`](CLAUDE.md).
