# Makefile for the mmap/ld.so logical simulator.
#
# Targets:
#   make all        build the core object files
#   make test       build and run all unit tests (zero-dependency harness)
#   make test-vma / test-mmap-ops / test-ldso-replay
#                   build and run one unit-test suite (used by CI for
#                   self-explanatory, per-suite step titles)
#   make test-asan  run all unit tests under ASan/UBSan
#   make asan-test-vma / asan-test-mmap-ops / asan-test-ldso-replay
#                   run one unit-test suite under ASan/UBSan (per-suite CI job)
#   make proof      run Frama-C/WP proofs (SKIPs cleanly if frama-c absent)
#   make verify     run scripts/verify.sh (two-tier: tests [+ proofs])
#   make clean

CC      ?= gcc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Werror -Wconversion -Wsign-conversion -Wshadow \
           -Wpointer-arith -Wcast-qual -Wstrict-prototypes -fno-common
CFLAGS  ?= $(CSTD) $(WARN) -Iinclude -O2 -fno-strict-aliasing
ASAN    := -fsanitize=address,undefined -fno-omit-frame-pointer -g

CORE_SRC := src/vma.c src/addr_space.c src/mmap_ops.c
CORE_OBJ := $(CORE_SRC:.c=.o)

TEST_SRC := tests/test_vma.c tests/test_mmap_ops.c tests/test_ldso_replay.c
TEST_BIN := $(patsubst tests/%.c,build/%,$(TEST_SRC))

.PHONY: all clean test test-vma test-mmap-ops test-ldso-replay test-asan \
        asan-test-vma asan-test-mmap-ops asan-test-ldso-replay \
        proof proof-parse verify

all: $(CORE_OBJ)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

build:
	@mkdir -p build

# Each test links against the core sources directly.
build/%: tests/%.c $(CORE_SRC) | build
	$(CC) $(CFLAGS) -Itests $< $(CORE_SRC) -o $@

test: $(TEST_BIN)
	@echo "== running unit tests =="
	@rc=0; for t in $(TEST_BIN); do echo "-- $$t"; ./$$t || rc=1; done; \
	 if [ $$rc -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "TESTS FAILED"; fi; \
	 exit $$rc

# Per-suite run targets: build (via the pattern rule) and run a single suite.
# CI invokes these so each step has a self-explanatory title.
test-vma: build/test_vma
	@./$<
test-mmap-ops: build/test_mmap_ops
	@./$<
test-ldso-replay: build/test_ldso_replay
	@./$<

test-asan: CFLAGS += $(ASAN)
test-asan: clean test

# Per-suite ASan/UBSan run targets (one self-explanatory CI status check each).
# Same target-specific-variable mechanism as test-asan: CFLAGS+=ASAN propagates
# to the rebuilt binary via the prerequisite chain.
asan-test-vma:          CFLAGS += $(ASAN)
asan-test-vma:          clean test-vma
asan-test-mmap-ops:     CFLAGS += $(ASAN)
asan-test-mmap-ops:     clean test-mmap-ops
asan-test-ldso-replay:  CFLAGS += $(ASAN)
asan-test-ldso-replay:  clean test-ldso-replay

# -no-warn-unaligned-pointer: suppress RTE \aligned(...) pointer-alignment
# alarms. WP does not implement \aligned ("not yet implemented"), so these
# goals can only degenerate; array-element addresses are aligned by
# construction, so the check carries no real safety content here.
FRAMAC_FLAGS := -machdep gcc_x86_64 -cpp-extra-args="-Iinclude -DFRAMA_C" \
                -no-warn-unaligned-pointer
FRAMAC_SRC   := $(CORE_SRC) proofs/wp_entry.c
# Per-goal prover timeout (s) and parallelism. Overridable so a slow/contended
# CI runner without a warm prover cache can be given more headroom than the
# local default (the heaviest goal needs ~2 min of Qed even when cached).
WP_TIMEOUT   ?= 120
WP_PAR       ?= 4

# Full WP proof tier. Runs only when frama-c AND its WP plugin are present
# (the Debian frama-c-base package ships the kernel + RTE but not WP; WP comes
# via opam). Falls back to a parse/typecheck + RTE-generation check that the
# ACSL is well-formed, so the annotations stay verified even without a prover.
#
# Set PROOF_REQUIRE_WP=1 to make a missing frama-c or WP plugin a hard FAILURE
# instead of SKIP/fallback — used in CI so a broken prover install is visible
# (a green "proofs" job no longer hides the fact that WP never ran).
proof:
	@if ! command -v frama-c >/dev/null 2>&1; then \
	    if [ "$${PROOF_REQUIRE_WP:-0}" = "1" ]; then \
	        echo "[FAIL] PROOF_REQUIRE_WP=1 but frama-c is not on PATH"; exit 1; \
	    fi; \
	    echo "[SKIP] frama-c not on PATH - proof tier skipped"; \
	elif frama-c -wp-help >/dev/null 2>&1; then \
	    echo "== running Frama-C/WP proofs =="; \
	    frama-c $(FRAMAC_FLAGS) -rte -wp -wp-rte \
	        -wp-session proofs/wp_session \
	        -wp-prover script,z3,alt-ergo -wp-timeout $(WP_TIMEOUT) -wp-par $(WP_PAR) $(FRAMAC_SRC); \
	elif [ "$${PROOF_REQUIRE_WP:-0}" = "1" ]; then \
	    echo "[FAIL] PROOF_REQUIRE_WP=1 but the WP plugin is not installed"; \
	    echo "       (frama-c is present but 'frama-c -wp-help' failed)"; exit 1; \
	else \
	    echo "[WARN] WP plugin absent - running ACSL parse + RTE check only"; \
	    echo "       (install Frama-C with WP via opam for full proofs)"; \
	    $(MAKE) --no-print-directory proof-parse; \
	fi

# ACSL well-formedness check: parse + typecheck the contracts and generate RTE
# obligations. Works with frama-c-base alone (no prover needed). Fails on any
# annotation error (-kernel-warn-key annot-error=+ is fatal by default).
proof-parse:
	@command -v frama-c >/dev/null 2>&1 || { echo "[SKIP] frama-c absent"; exit 0; }
	@echo "== Frama-C ACSL parse + RTE generation =="
	frama-c $(FRAMAC_FLAGS) -rte $(FRAMAC_SRC)

verify:
	@./scripts/verify.sh

clean:
	@rm -f $(CORE_OBJ)
	@rm -rf build
