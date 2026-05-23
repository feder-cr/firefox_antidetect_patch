#!/usr/bin/env bash
# Package Win + Linux Firefox binaries into archives, validate them, and
# create a GitHub release on feder-cr/invisible_playwright.
#
# Usage:
#   ./scripts/make_release.sh <tag> "<release-name>" "<release-body>"
#
# Example:
#   ./scripts/make_release.sh firefox-5 "invisible_firefox (150.0.1) rev 5" \
#       "Bug fix release. Closes issue #14."
#
# Expects:
#   - Windows build at: c:/ff/source/obj-x86_64-pc-windows-msvc/dist/bin/firefox/
#   - Linux build (WSL): ~/ff-build/firefox-150/obj-x86_64-pc-linux-gnu/dist/bin/
#   - GITHUB_TOKEN env var with repo:write on feder-cr/invisible_playwright
#
# This script REFUSES to upload if scripts/validate_release.py fails on
# either archive. That's the gate that was missing when firefox-3 and
# firefox-4 shipped broken Linux tarballs (issue #14 — 265 affected
# downloads). See scripts/validate_release.py for the checks.

set -euo pipefail

TAG="${1:?Usage: $0 <tag> <release-name> <release-body>}"
NAME="${2:?missing release name}"
BODY="${3:?missing release body}"
TOKEN="${GITHUB_TOKEN:?GITHUB_TOKEN env var required}"
REPO="feder-cr/invisible_playwright"

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$REPO_ROOT/release-out/$TAG"
mkdir -p "$OUT"
rm -f "$OUT"/firefox-*.{zip,tar.gz} "$OUT/checksums.txt"

WIN_ZIP="firefox-150.0.1-stealth-win-x86_64.zip"
LIN_TGZ="firefox-150.0.1-stealth-linux-x86_64.tar.gz"

WIN_DIST="$REPO_ROOT/obj-x86_64-pc-windows-msvc/dist/bin/firefox"

# ---------- Windows zip ----------
echo "=== [1/5] Packaging Windows ==="
if [[ ! -d "$WIN_DIST" ]]; then
    echo "ERROR: Windows dist not found at $WIN_DIST" >&2
    exit 1
fi
( cd "$(dirname "$WIN_DIST")" && \
  powershell.exe -NoProfile -Command \
    "Compress-Archive -Path firefox -DestinationPath '$OUT\\$WIN_ZIP' -Force" )
ls -lh "$OUT/$WIN_ZIP"

# ---------- Linux tar.gz via the CORRECT pipeline ----------
# Runs cp -aL + strip + sanitize before tar. The previous incarnation of
# this script ran `tar -czf firefox` directly against dist/bin/firefox,
# which preserved symlinks pointing at the source tree -> all dangling on
# user machines -> issue #14. We now go through linux_release.sh which
# enforces the correct pipeline and runs validate_release.py as its final
# step.
echo ""
echo "=== [2/5] Packaging Linux (cp -aL + strip + sanitize + tar) ==="
# Drive the WSL pipeline; its output ends up in ~/ff-build/release-<tag>/<LIN_TGZ>
# inside WSL. We then pull it back to Windows via /mnt/c.
wsl.exe -d Ubuntu -- bash "$(wslpath "$REPO_ROOT")/scripts/linux_release.sh" "$TAG"
# Bring the tarball over from WSL to our $OUT
wsl.exe -d Ubuntu -- bash -c "cp \"\$HOME/ff-build/release-$TAG/$LIN_TGZ\" \"$(wslpath "$OUT")/$LIN_TGZ\""
if [[ ! -f "$OUT/$LIN_TGZ" ]]; then
    echo "ERROR: Linux tarball not produced at $OUT/$LIN_TGZ" >&2
    exit 1
fi
ls -lh "$OUT/$LIN_TGZ"

# ---------- Mandatory validator (rejects broken archives) ----------
echo ""
echo "=== [3/5] Mandatory validate_release.py (extract + smoke-test BOTH) ==="
python3 "$REPO_ROOT/scripts/validate_release.py" \
    --linux "$OUT/$LIN_TGZ" \
    --win   "$OUT/$WIN_ZIP"
# If validate_release.py exits non-zero, `set -e` aborts before checksums.

# ---------- Checksums ----------
echo ""
echo "=== [4/5] Checksums ==="
( cd "$OUT" && sha256sum "$WIN_ZIP" "$LIN_TGZ" > checksums.txt )
cat "$OUT/checksums.txt"

# ---------- Create release + upload ----------
echo ""
echo "=== [5/5] Creating GitHub release $TAG on $REPO ==="
RESP=$(curl -sS -X POST \
  -H "Authorization: token $TOKEN" \
  -H "Accept: application/vnd.github+json" \
  "https://api.github.com/repos/$REPO/releases" \
  -d "$(python3 -c "
import json, sys
print(json.dumps({
    'tag_name': sys.argv[1],
    'name': sys.argv[2],
    'body': sys.argv[3],
    'draft': False,
    'prerelease': False,
}))" "$TAG" "$NAME" "$BODY")")
RELEASE_ID=$(echo "$RESP" | python3 -c "import sys,json; print(json.load(sys.stdin)['id'])")
UPLOAD_URL="https://uploads.github.com/repos/$REPO/releases/$RELEASE_ID/assets"
echo "Release ID: $RELEASE_ID"

for f in "$WIN_ZIP" "$LIN_TGZ" checksums.txt; do
    echo "  uploading $f..."
    MIME="application/octet-stream"
    [[ "$f" == *.txt ]] && MIME="text/plain"
    [[ "$f" == *.zip ]] && MIME="application/zip"
    [[ "$f" == *.tar.gz ]] && MIME="application/gzip"
    curl -sS -X POST \
      -H "Authorization: token $TOKEN" \
      -H "Content-Type: $MIME" \
      --data-binary "@$OUT/$f" \
      "$UPLOAD_URL?name=$f" \
      | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'  ok: {d.get(\"name\")} ({d.get(\"size\")} bytes)')"
done

echo ""
echo "=== Release created ==="
echo "https://github.com/$REPO/releases/tag/$TAG"
