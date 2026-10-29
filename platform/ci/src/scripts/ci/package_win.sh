#!/usr/bin/env bash
# Package the Windows (cross) leg: zip the CLEAN dist/firefox tree with
# firefox.exe at the zip root, after the same gates as the Linux legs.
#   ASSET=<archive name> scripts/ci/package_win.sh
set -euo pipefail
: "${ASSET:?ASSET is the archive name (python3 scripts/ci asset --leg ...)}"
# A release MUST come from the clean dist/firefox; dist/bin is the dev tree
# (cruft + loose juggler that masked the firefox-7/8 packaging bugs). A
# failing `mach package` fails the leg, never falls through to a stale tree.
./mach package
WIN_APP=obj-rel/dist/firefox
[ -f "$WIN_APP/firefox.exe" ] || { echo "ERROR: mach package did not produce a clean dist/firefox tree"; exit 1; }
echo "packaging from: $WIN_APP"
# JUGGLER GATE in omni.ja and the PAGE-SURFACE GATE, as on the Linux legs.
[ -f "$WIN_APP/omni.ja" ] || { echo "ERROR: no omni.ja in $WIN_APP"; exit 1; }
python3 scripts/release_gates.py "$WIN_APP"
mkdir -p out
( cd "$WIN_APP" && zip -qr "$GITHUB_WORKSPACE/out/$ASSET" . )   # firefox.exe at zip ROOT
ls -la out/
