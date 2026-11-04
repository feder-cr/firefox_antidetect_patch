#!/usr/bin/env bash
# The tree a leg ships, and nothing more:
#
#     bash scripts/ci/stage_package.sh      # from the source tree, after the build
#
# Both legs ship their own archive of obj-rel/dist/firefox (package_linux.sh,
# package_win.sh). `mach package` stages that tree first (`stage-package`, Mozilla's
# packager) and then spends time on archives nobody ships: on Windows a zip (32 s) and
# the NSIS installer, its 7z on one core (89 s); on Linux a tar.xz. Measured on
# candidate 41892772401 (04/11): the Windows package step took 2.7 min, of which 2 min
# were those. This stages the tree only.
#
# CI_PACKAGE_PROVE=1 also runs `mach package` after it and fails unless dist/firefox
# is the same, byte for byte, file for file and link for link: the proof that the tree
# shipped does not change, run once on a candidate.
set -euo pipefail

OBJ="${CI_OBJDIR:-obj-rel}"
MAKE_BIN="${CI_MAKE:-$(command -v gmake || command -v make)}"
MACH="${CI_MACH:-./mach}"

manifest() {
  (cd "$OBJ/dist/firefox" &&
    find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum &&
    find . -type l -printf '%p -> %l\n' | LC_ALL=C sort)
}

"$MAKE_BIN" -C "$OBJ" stage-package
[ -d "$OBJ/dist/firefox" ] || { echo "stage_package: ERROR: stage-package made no $OBJ/dist/firefox" >&2; exit 1; }

if [ "${CI_PACKAGE_PROVE:-}" = 1 ]; then
  manifest > "$OBJ/stage-only.manifest"
  "$MACH" package
  manifest > "$OBJ/mach-package.manifest"
  if cmp -s "$OBJ/stage-only.manifest" "$OBJ/mach-package.manifest"; then
    echo "stage_package: proved: dist/firefox is what mach package leaves, byte for byte ($(wc -l < "$OBJ/stage-only.manifest") entries)"
  else
    echo "stage_package: ERROR: dist/firefox differs from what mach package leaves:" >&2
    diff "$OBJ/stage-only.manifest" "$OBJ/mach-package.manifest" | head -20 >&2
    exit 1
  fi
fi
