#!/bin/sh
# verify.sh - Two-tier verification harness for the mmap/ld.so simulator.
#
# Tier 1 (mandatory): build + unit tests. Always runs; gates everything.
# Tier 2 (conditional): Frama-C/WP proofs. Runs only when frama-c is on PATH;
#         otherwise SKIPs without failing.
#
# Exit 0 iff Tier 1 passed AND (Tier 2 passed OR was skipped).
# Set VERIFY_REQUIRE_PROOF=1 to turn a proof SKIP into a hard failure (for CI
# gates that mandate proofs).
set -eu

cd "$(dirname "$0")/.."

echo "=============================================="
echo " Tier 1: build + unit tests (mandatory)"
echo "=============================================="
make clean >/dev/null 2>&1 || true
if ! make test; then
    echo "VERIFY: FAIL (tests)"
    exit 1
fi

echo
echo "=============================================="
echo " Tier 2: Frama-C/WP proofs (conditional)"
echo "=============================================="
PROOF_STATUS="SKIPPED"
if command -v frama-c >/dev/null 2>&1; then
    if make proof; then
        PROOF_STATUS="PASS"
    else
        PROOF_STATUS="FAIL"
    fi
else
    echo "[SKIP] frama-c not on PATH - proof tier skipped"
    if [ "${VERIFY_REQUIRE_PROOF:-0}" = "1" ]; then
        echo "VERIFY: FAIL (VERIFY_REQUIRE_PROOF=1 but frama-c absent)"
        exit 1
    fi
fi

echo
echo "----------------------------------------------"
echo "VERIFY: PASS (tests) / PROOF: ${PROOF_STATUS}"
echo "----------------------------------------------"

if [ "$PROOF_STATUS" = "FAIL" ]; then
    exit 1
fi
exit 0
