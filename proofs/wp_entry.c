/*
 * wp_entry.c - Frama-C/WP entry translation unit.
 *
 * This file exists so the proof target has a single, explicit place to pull
 * in the annotated core and (later) host proof-only lemmas, helper specs, or
 * a verification harness driver. It deliberately includes ONLY the provable
 * core headers; no test or I/O code is ever fed to Frama-C.
 *
 * The core .c files are passed on the frama-c command line alongside this
 * file (see Makefile `proof` target), so this unit only needs the public and
 * internal declarations in scope for any cross-unit lemmas added here.
 */
#include "mm_api.h"
#include "mm_internal.h"
#include "mm_acsl.h"

/* No proof lemmas yet. Contracts live with their implementations in the core
 * source files. Add ACSL `lemma` declarations or ghost drivers here as the
 * proof effort (milestone M2) requires. */
