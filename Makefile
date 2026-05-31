# Makefile for the mmap/ld.so logical simulator.
#
# Targets:
#   make all        build the core object files
#   make test       build and run all unit tests (zero-dependency harness)
#   make test-asan  run unit tests under ASan/UBSan
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

.PHONY: all clean test test-asan proof verify

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

test-asan: CFLAGS += $(ASAN)
test-asan: clean test

proof:
	@if command -v frama-c >/dev/null 2>&1; then \
	    echo "== running Frama-C/WP proofs =="; \
	    frama-c -machdep gcc_x86_64 \
	        -cpp-extra-args="-Iinclude -DFRAMA_C" \
	        -rte -wp -wp-rte \
	        -wp-prover alt-ergo,z3 -wp-timeout 20 \
	        $(CORE_SRC) proofs/wp_entry.c; \
	else \
	    echo "[SKIP] frama-c not on PATH - proof tier skipped"; \
	fi

verify:
	@./scripts/verify.sh

clean:
	@rm -f $(CORE_OBJ)
	@rm -rf build
