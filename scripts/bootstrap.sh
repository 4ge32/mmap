#!/bin/sh
# bootstrap.sh - SessionStart hook: ensure the dev toolchain is present and
# report which verification tiers are available this session.
#
# MUST NOT block or fail the session: the proof toolchain (Frama-C + provers)
# may be impossible to install when the network is restricted. In that case we
# print a banner and continue; build + unit tests still work.
#
# Idempotent: a marker file avoids re-attempting installs every session.
set -u

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MARKER="${REPO_ROOT}/.bootstrap-done"

have() { command -v "$1" >/dev/null 2>&1; }

echo "=== mmap simulator: environment check ==="

# Tier 1 toolchain (expected present).
for tool in gcc make; do
    if have "$tool"; then
        echo "  [ok]   $tool"
    else
        echo "  [MISS] $tool  (Tier 1 build will not work)"
    fi
done

# Tier 2 toolchain (often absent; best-effort install once).
if have frama-c; then
    echo "  [ok]   frama-c  -> proof tier AVAILABLE"
else
    echo "  [miss] frama-c  -> proof tier will SKIP"
    if [ ! -f "$MARKER" ]; then
        echo "  ... attempting best-effort install (non-fatal) ..."
        if have apt-get; then
            apt-get install -y frama-c >/dev/null 2>&1 \
                && echo "  [ok]   installed frama-c via apt-get" \
                || echo "  [info] apt-get install failed (likely no network); skipping"
        elif have opam; then
            opam install -y frama-c >/dev/null 2>&1 \
                && echo "  [ok]   installed frama-c via opam" \
                || echo "  [info] opam install failed; skipping"
        else
            echo "  [info] no apt-get/opam available; cannot install frama-c"
        fi
        : > "$MARKER" 2>/dev/null || true
    fi
fi

echo "=== verification tiers ==="
echo "  Tier 1 (build + tests): use 'make test' or './scripts/verify.sh'"
if have frama-c; then
    echo "  Tier 2 (proofs):        use 'make proof' (frama-c present)"
else
    echo "  Tier 2 (proofs):        SKIPPED this session (frama-c absent)"
fi

# Never fail the session.
exit 0
