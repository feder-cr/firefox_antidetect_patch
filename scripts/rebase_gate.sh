#!/usr/bin/env bash
# Rebase gate — run after any rebase onto a new Firefox base.
# Builds the binaries and runs the 4 smoke tests sequentially.
# Exit 0 iff all smoke tests pass.
#
# Mirrors what Brave's CI does on each Chromium upgrade and what Tor Browser's
# ESR-rebase issues track as the "must-pass" checklist.
#
# Usage:
#   ./scripts/rebase_gate.sh
#
# Optional env:
#   STEALTHFOX_PYTHONPATH (default: c:/src/firefox-stealth/release/stealthfox/src)
#   STEALTHFOX_BINARY (default: obj-x86_64-pc-windows-msvc/dist/bin/firefox.exe)
#   SKIP_BUILD=1 (skip the mach build step, useful for re-running tests)

set -e
cd "$(dirname "$0")/.."

if [[ -z "$STEALTHFOX_BINARY" ]]; then
    export STEALTHFOX_BINARY="$(pwd)/obj-x86_64-pc-windows-msvc/dist/bin/firefox.exe"
fi
if [[ -z "$STEALTHFOX_PYTHONPATH" ]]; then
    export STEALTHFOX_PYTHONPATH="c:/src/firefox-stealth/release/stealthfox/src"
fi

echo "=== rebase_gate.sh ==="
echo "STEALTHFOX_BINARY=$STEALTHFOX_BINARY"
echo "STEALTHFOX_PYTHONPATH=$STEALTHFOX_PYTHONPATH"
echo ""

# Build
if [[ "$SKIP_BUILD" != "1" ]]; then
    echo "--- mach build binaries ---"
    if ! ./mach build binaries 2>&1 | tail -5; then
        echo "[GATE FAIL] mach build returned non-zero"
        exit 1
    fi
    if [[ ! -x "$STEALTHFOX_BINARY" ]]; then
        echo "[GATE FAIL] firefox.exe missing after build"
        exit 1
    fi
fi

# Smoke tests
TESTS=(
    "scripts/tests/test_launch.py"
    "scripts/tests/test_new_page.py"
    "scripts/tests/test_mouse.py"
    "scripts/tests/test_stealth.py"
)

FAILED=0
for t in "${TESTS[@]}"; do
    echo ""
    echo "--- $t ---"
    if python "$t"; then
        :
    else
        FAILED=$((FAILED+1))
    fi
done

echo ""
echo "=== rebase_gate.sh summary ==="
if [[ $FAILED -eq 0 ]]; then
    echo "ALL SMOKE TESTS PASSED — rebase is functionally complete."
    echo ""
    echo "Next: tag stealth-head/v<upstream-version> at HEAD and push."
    exit 0
else
    echo "$FAILED smoke test(s) FAILED — do NOT tag/release."
    exit 1
fi
