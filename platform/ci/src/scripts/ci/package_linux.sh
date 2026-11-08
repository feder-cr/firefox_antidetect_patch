#!/usr/bin/env bash
# Package a Linux leg: the packaged tree, symlinks dereferenced (issue #14),
# gates, strip, tar at the root, then validate_release.py as a hard gate.
#   ASSET=<archive name> scripts/ci/package_linux.sh
set -euo pipefail
: "${ASSET:?ASSET is the archive name (python3 scripts/ci asset --leg ...)}"
# The PACKAGED tree, as the Windows leg ships it (B310). This leg used to tar
# obj-rel/dist/bin, the development tree: the test binaries and TLS test
# servers went out with it, and the 44 locale .properties a page can read
# kept the comments the packager strips, so they differed from the retail's.
# dist/firefox only: the archives `mach package` makes after it are not shipped.
bash scripts/ci/stage_package.sh
DIST=obj-rel/dist/firefox
[ -x "$DIST/firefox" ] || { echo "ERROR: mach package did not produce $DIST/firefox"; exit 1; }
STAGING=staging
rm -rf "$STAGING"; mkdir -p "$STAGING" out
cp -aL "$DIST/." "$STAGING/"                      # -L: dereference ALL symlinks into real files
N=$(find "$STAGING" -type l | wc -l)
[ "$N" -eq 0 ] || { echo "ERROR: $N symlinks remain after cp -aL"; exit 1; }
# WebGPU's Vulkan driver (scripts/ci/lavapipe_build.py), next to libxul where
# the engine looks for it. Required: without it WebGPU answers null, never with a
# host driver.
if [ "${CI_LEG:-}" = linux-x86_64 ]; then
  [ -f obj-rel/dist/bin/lavapipe/libvulkan_lvp.so ] || { echo "ERROR: no lavapipe in obj-rel/dist/bin"; exit 1; }
  mkdir -p "$STAGING/lavapipe"
  cp -a obj-rel/dist/bin/lavapipe/libvulkan_lvp.so obj-rel/dist/bin/lavapipe/VERSION.json "$STAGING/lavapipe/"
  # WebGL's GL (llvmpipe behind EGL, from the same build), where the engine
  # looks for it. Required: without it WebGL has no context, never the host's GL.
  for f in libEGL.so.1 libGLESv2.so.2; do
    [ -f "obj-rel/dist/bin/llvmpipe/$f" ] || { echo "ERROR: no llvmpipe/$f in obj-rel/dist/bin"; exit 1; }
  done
  ls obj-rel/dist/bin/llvmpipe/libgallium-*.so >/dev/null || { echo "ERROR: no llvmpipe libgallium in obj-rel/dist/bin"; exit 1; }
  mkdir -p "$STAGING/llvmpipe"
  cp -a obj-rel/dist/bin/llvmpipe/. "$STAGING/llvmpipe/"
fi
# JUGGLER GATE: present (Playwright cannot drive the binary without it).
# PAGE-SURFACE GATE: what a page can read of the build is the retail's of the
# same version, judged against Mozilla's complete MAR (stock_parity.py).
# Every gate runs and reports before the verdict (scripts/release_gates.py).
python3 scripts/release_gates.py "$STAGING"
find "$STAGING" -type f \
  \( -name '*.so' -o -name firefox -o -name firefox-bin -o -name plugin-container \
     -o -name pingsender -o -name glxtest -o -name vaapitest -o -name updater \) \
  -exec strip --strip-debug {} + 2>/dev/null || true
STAGING="$STAGING" python3 scripts/linux_sanitize.py || true   # no-op in CI (no /home/feder), defensive
tar --owner=0 --group=0 --numeric-owner --mtime="2026-01-01 00:00:00 UTC" \
    -czf "out/$ASSET" -C "$STAGING" .              # firefox at ROOT
echo "=== HARD GATE: scripts/validate_release.py (the issue-#14 protector) ==="
# --no-site-patterns: the site-name token list is deliberately not in this
# repository and cannot be on a CI runner, so this leg checks structure and
# build-machine paths only. The full scan is the operator's pre-publish run.
python3 scripts/validate_release.py --linux "out/$ASSET" --linux-only --no-site-patterns
ls -la out/
