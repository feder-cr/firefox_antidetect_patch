#!/bin/bash
# Build the Linux release tarball from a patched Firefox build.
#
# Run this from inside WSL / Linux. It produces a tarball with NO
# symlinks (all dereferenced into real files) and NO `/home/feder/`
# personal-path leaks. Output is suitable for direct upload to a GitHub
# release.
#
# Usage:
#   scripts/linux_release.sh <tag>
# Example:
#   scripts/linux_release.sh firefox-4
#
# Optional env vars:
#   SOURCE_DIR  source firefox tree (default: ~/ff-build/firefox-150)
#   STAGING     scratch dir for dereferenced copy (default: ~/ff-build/stealth-staging-<tag>)
#   OUTPUT_DIR  where the final tarball lands (default: ~/ff-build/release-<tag>)

set -euo pipefail

TAG="${1:?usage: $0 <tag>}"
SOURCE_DIR="${SOURCE_DIR:-$HOME/ff-build/firefox-150}"
STAGING="${STAGING:-$HOME/ff-build/stealth-staging-$TAG}"
OUTPUT_DIR="${OUTPUT_DIR:-$HOME/ff-build/release-$TAG}"
DIST="$SOURCE_DIR/obj-x86_64-pc-linux-gnu/dist/bin"
TARBALL="$OUTPUT_DIR/firefox-151.0-stealth-linux-x86_64.tar.gz"

if [ ! -d "$DIST" ]; then
    echo "ERROR: dist/bin not found at $DIST" >&2
    echo "       (set SOURCE_DIR to the patched-firefox source root)" >&2
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "[linux_release] tag      = $TAG"
echo "[linux_release] source   = $SOURCE_DIR"
echo "[linux_release] staging  = $STAGING"
echo "[linux_release] output   = $OUTPUT_DIR"

echo ""
echo "[1/5] Dereference symlinks (cp -aL)..."
rm -rf "$STAGING"
mkdir -p "$STAGING"
cp -aL "$DIST/." "$STAGING/"
REMAINING=$(find "$STAGING" -type l | wc -l)
if [ "$REMAINING" -ne "0" ]; then
    echo "ERROR: $REMAINING symlinks remain after cp -aL" >&2
    exit 1
fi
echo "  $(find "$STAGING" -type f | wc -l) regular files, no symlinks"

echo ""
echo "[2/5] Remove dev-only binaries (they leak build paths)..."
for tool in xpcshell certutil pk12util rapl; do
    if [ -f "$STAGING/$tool" ]; then
        echo "  rm $tool"
        rm -f "$STAGING/$tool"
    fi
done

echo ""
echo "[3/5] Strip debug info from binaries..."
find "$STAGING" -type f \
    \( -name "*.so" -o -name "firefox" -o -name "firefox-bin" \
       -o -name "plugin-container" -o -name "pingsender" \
       -o -name "glxtest" -o -name "vaapitest" -o -name "updater" \) \
    -exec strip --strip-debug {} + 2>/dev/null || true

echo ""
echo "[4/5] Sanitize embedded build-host paths..."
STAGING="$STAGING" python3 "$REPO_ROOT/scripts/linux_sanitize.py"

# buildconfig.html has free-form paths; sanitize separately (no
# length constraint, plain HTML).
HTML="$STAGING/chrome/toolkit/content/global/buildconfig.html"
if [ -f "$HTML" ]; then
    sed -i \
        -e 's|/home/feder/\.mozbuild|/b/firefox-stealth/mozbuild|g' \
        -e 's|/home/feder/\.rustup|/b/firefox-stealth/rustup|g' \
        -e 's|/home/feder/|/b/firefox-stealth/|g' \
        "$HTML"
fi

# grep -arl exits 1 when there are NO matches. Combined with set -o pipefail
# that propagates as a non-zero pipeline exit, killing the script silently
# (the "good" case looked like an error). `|| true` keeps the leak check
# semantically the same: LEAK_FILES counts matches, 0 = no leaks = success.
LEAK_FILES=$( ( grep -arl '/home/feder' "$STAGING" 2>/dev/null || true ) | wc -l )
if [ "$LEAK_FILES" -ne "0" ]; then
    echo "ERROR: $LEAK_FILES files still contain /home/feder paths:" >&2
    grep -arl '/home/feder' "$STAGING" 2>/dev/null | head -10 >&2
    exit 1
fi
echo "  clean (no /home/feder bytes remaining)"

echo ""
echo "[5/5] Build tarball (anonymized owner, fixed mtime)..."
mkdir -p "$OUTPUT_DIR"
rm -f "$TARBALL"
tar --owner=0 --group=0 --numeric-owner \
    --mtime="2026-01-01 00:00:00 UTC" \
    -czf "$TARBALL" \
    -C "$STAGING" .

echo ""
echo "=== DONE building tarball ==="
ls -lh "$TARBALL"
sha256sum "$TARBALL"
echo ""
echo "Self-check:"
echo "  symlinks in archive: $(tar -tzvf "$TARBALL" | grep -c '^l')"
echo "  regular files:       $(tar -tzvf "$TARBALL" | grep -c '^-')"
echo ""

echo "[6/6] Mandatory smoke test via validate_release.py..."
# One leg, pre-packaging: structure, build-machine paths and the smoke test. The
# site-name scan needs a token list that lives outside this repo, so it is waived
# HERE and required at publish time - make_release.sh calls the same validator
# without this flag, and that call now exits 3 if the list is not set.
python3 "$REPO_ROOT/scripts/validate_release.py" --linux "$TARBALL" --linux-only \
    --no-site-patterns
echo ""
echo "OK — Linux tarball cleared for upload."
echo ""
echo "  Tarball: $TARBALL"
echo ""
echo "Next: pair it with the Windows zip and call validate_release.py with"
echo "both archives before uploading. scripts/make_release.sh wraps this."
