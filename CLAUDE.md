# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

A C-language **logical simulator of Linux `mmap` semantics**, faithful enough to
model how a dynamic loader (`ld-linux` / `ld.so`) maps a shared object:
`PROT_NONE` reservation, multiple `MAP_FIXED` overlays into that reservation,
and `mprotect`-based protection switching. It is **pure bookkeeping — no real
memory is allocated**; the virtual address space is a fixed-capacity, sorted,
non-overlapping, maximally-merged (canonical) array of VMAs.

Correctness is established two ways: a runtime unit-test suite and Frama-C/WP
deductive proofs of the same well-formedness invariant.

## Commands

```sh
make all          # build the core object files
make test         # build + run all unit tests (zero-dependency harness)
make test-asan    # run tests under ASan/UBSan
make proof        # Frama-C/WP proofs; SKIPs cleanly if frama-c is absent
make verify       # scripts/verify.sh: two-tier (tests [+ proofs]) pass/fail
./scripts/verify.sh   # same as `make verify`

# run a single test binary directly:
make build/test_mmap_ops && ./build/test_mmap_ops
```

`VERIFY_REQUIRE_PROOF=1 ./scripts/verify.sh` turns a proof SKIP into a hard
failure (for CI gates that mandate proofs).

## Toolchain reality

`gcc`/`make` are present, so **build + tests always work**. The proof tier
needs `frama-c` plus a prover (`alt-ergo`/`z3`), which are **often absent** in
the web environment and may be impossible to install when the network is
restricted. `make proof` and the harness SKIP gracefully in that case — this
is expected, not a failure. ACSL annotations are authored regardless so the
core stays "proof-ready".

## Architecture

- **Provable core** (`src/`, fed to Frama-C): `vma.c` (primitives +
  arithmetic guards), `addr_space.c` (container ops + the `as_check_wf`
  runtime oracle), `mmap_ops.c` (`mm_mmap`/`mm_mprotect`/`mm_munmap`). The
  three operations all reduce to the primitives `as_split_at` / `as_insert_at`
  / `as_remove_range` plus an `as_canonicalize` merge pass.
- **Headers** (`include/`): `mm_types.h` (types/constants), `mm_api.h` (public
  API), `mm_internal.h` (primitives + oracle), `mm_acsl.h` (ACSL predicates,
  including the central `as_wf` well-formedness invariant — invisible to the C
  compiler, read only by Frama-C).
- **Tests/tools** (never fed to Frama-C): `tests/` zero-dependency harness and
  suites incl. the `tests/test_ldso_replay.c` loader-sequence scenario;
  `tools/vma_dump.c` debug printer.
- Specs: `docs/design.md`, `docs/mmap_model_spec.md`, `docs/ldso_sequence.md`.

The `as_wf` invariant is the linchpin: it is defined once as an ACSL predicate
(`mm_acsl.h`), mirrored at runtime (`as_check_wf`), and asserted after every
operation in tests via `ASSERT_WF`. Keep all three in lockstep.

## Rules for agents (enforced by convention)

- **No dynamic allocation and no recursion in `src/`.** The `struct
  addr_space` array IS the arena. This is what keeps the WP proof tractable.
- Addresses/sizes are `uint64_t`; guard every `addr + length` and page
  round-up against overflow (`add_overflows`, `round_up_overflows`).
- Keep VMAs in canonical form after every public operation.
- Keep ACSL contracts co-located with and in sync with the code.
- **Editing boundaries** (see `.claude/agents/`): `simulator-implementer` owns
  `src/`+`include/` logic; `proof-engineer` owns ACSL bodies + `proofs/`;
  `test-engineer` owns `tests/`; `ldso-spec-researcher` owns
  `docs/ldso_sequence.md`.
