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

Functional byte-level outcomes and the ld.so replay scenario are covered by the
runtime test suite, not by WP (see `docs/design.md` §B.2).

## Running

```sh
make proof
```

`make proof` SKIPs cleanly (exit 0, prints `[SKIP]`) when `frama-c` is not on
PATH, so the build+test loop is never blocked by a missing prover. When the
prover is present it fails on any unproven goal.

Direct invocation:

```sh
frama-c -machdep gcc_x86_64 -cpp-extra-args="-Iinclude -DFRAMA_C" \
        -rte -wp -wp-rte -wp-prover alt-ergo,z3 -wp-timeout 20 \
        src/vma.c src/addr_space.c src/mmap_ops.c proofs/wp_entry.c
```

## Reading goal reports

WP prints a per-function goal summary. Any line reporting `Unknown` or
`Timeout` is an undischarged goal: either strengthen the contract / loop
invariant, raise `-wp-timeout`, or document the goal as test-covered in
`docs/design.md`.

## Toolchain

Requires `frama-c` plus a prover backend (`alt-ergo` and/or `z3`). Neither is
installed in the default web environment; `scripts/bootstrap.sh` attempts a
best-effort install but does not fail the session if the network is restricted.
