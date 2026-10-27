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
#   PREV_BASE / PREV_BRANCH — the base tag and stealth branch we rebased FROM.
#     When both are set, the patch-survival check runs first. Set them: a
#     rebase that builds and smoke-tests fine can still have silently dropped a
#     patch (the FF150->151 rebase lost the whole TTC font fix that way, and
#     this gate did not notice because it only builds and drives the browser).

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

# Patch survival — cheap, and catches what a build+smoke run cannot see:
# a patch that was dropped by the rebase while everything still compiles.
if [[ -n "${PREV_BASE:-}" && -n "${PREV_BRANCH:-}" ]]; then
    echo "--- patch survival ($PREV_BASE -> $PREV_BRANCH) ---"
    if ! ./scripts/patch_survival_check.sh "$PREV_BASE" "$PREV_BRANCH"; then
        echo "[GATE FAIL] patches were lost in the rebase (see above)"
        exit 1
    fi
    echo ""
else
    echo "[warn] PREV_BASE/PREV_BRANCH not set - skipping the patch-survival"
    echo "       check. Set them after a rebase; build+smoke alone cannot tell"
    echo "       you whether a patch was silently dropped."
    echo ""
fi

# Protocol drift — the other failure this gate could not see. checkScheme rejects
# any field the client sends that we do not declare, and only at runtime: the
# browser still builds, launches and loads pages, so build+smoke stay green while
# the client is broken. Playwright 1.61 added isMobile and screenSize on two
# viewport commands and took out 97 of 133 e2e tests with everything looking fine.
echo "--- protocol drift (installed Playwright vs juggler scheme) ---"
if ! python scripts/protocol_drift_check.py; then
    echo "[GATE FAIL] the installed Playwright sends fields Protocol.js does not declare"
    exit 1
fi
echo ""

# Release naming — the version bump touches ~40 hand-written asset names across the
# workflows plus the client constant. If they drift, CI publishes one name and the
# wrapper fetches another: every download 404s while the build, the archives and the
# release page all look correct (issue #14, 265 users).
echo "--- release asset naming vs the version the tree builds ---"
NAMING_ARGS=()
if [[ -d "${INVISIBLE_CORE_SRC:-}" ]]; then
    NAMING_ARGS=(--core "$INVISIBLE_CORE_SRC")
fi
if ! python scripts/check_release_naming.py "${NAMING_ARGS[@]}"; then
    echo "[GATE FAIL] asset names disagree with browser/config/version_display.txt"
    exit 1
fi
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
