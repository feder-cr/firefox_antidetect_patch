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
# The PACKAGED tree (`./mach package` first), as release.yml ships it (B310):
# dist/bin is the development tree, with the test binaries and the comments the
# packager strips from the .properties a page can read.
DIST="$SOURCE_DIR/obj-x86_64-pc-linux-gnu/dist/firefox"
TARBALL="$OUTPUT_DIR/firefox-151.0-stealth-linux-x86_64.tar.gz"

if [ ! -d "$DIST" ]; then
    echo "ERROR: dist/firefox not found at $DIST: run ./mach package first" >&2
    echo "       (and set SOURCE_DIR to the patched-firefox source root)" >&2
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
echo "[2/5] Strip debug info from binaries..."
find "$STAGING" -type f \
    \( -name "*.so" -o -name "firefox" -o -name "firefox-bin" \
       -o -name "plugin-container" -o -name "pingsender" \
       -o -name "glxtest" -o -name "vaapitest" -o -name "updater" \) \
    -exec strip --strip-debug {} + 2>/dev/null || true

echo ""
echo "[3/5] Sanitize embedded build-host paths..."
STAGING="$STAGING" python3 "$REPO_ROOT/scripts/linux_sanitize.py"

# No leak check here: validate_release.py below reads every file AND every
# member of omni.ja, where the packaged buildconfig.html now lives and where the
# sed and the grep that used to be here could not reach. A home directory left
# in a local build is refused there instead of shipped.

echo ""
echo "[4/5] Build tarball (anonymized owner, fixed mtime)..."
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

echo "[5/5] Mandatory smoke test via validate_release.py..."
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
