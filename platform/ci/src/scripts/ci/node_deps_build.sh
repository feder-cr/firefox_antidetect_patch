#!/usr/bin/env bash
# Build the node's packages once, so that a node never asks Ubuntu for them:
#
#     docker run --rm -v "$PWD/scripts/ci:/ci:ro" -v "$OUT:/out" ubuntu:24.04 bash /ci/node_deps_build.sh
#
# node_bootstrap.sh installed its apt packages from one dated snapshot of Ubuntu's
# archive at every start. That service was down for most of 2026-11-04 (503 and 502),
# and slow (3-4 s for every file) when up: two candidates died preparing their node,
# a third took 9 minutes instead of 2. So every .deb the list needs goes into one
# archive, with an index apt reads from disk, and glslang already built; the archive
# goes up to GitHub once, and node_bootstrap.sh fetches it by name and sha256.
#
# The archive IS the pin: its sha256 is in node_bootstrap.sh, and its MANIFEST lists
# every .deb by package, version and architecture. Where they come from: Ubuntu's
# archive by default (NODE_DEPS_MIRROR), whose versions move every day while the
# archive built from it does not.
#
# The list and glslang's version are read from node_bootstrap.sh: one place for every
# version. apt resolves the list as if nothing were installed, so the archive holds
# every package the list needs whatever the node's image already has.
#
# Out: /out/node-deps-<day>-<hash>.tar and its .sha256. A plain tar: the .debs are
# compressed already, and a fresh node unpacks it with nothing installed first.
set -euo pipefail

CI="${NODE_DEPS_CI:-/ci}"
OUT="${NODE_DEPS_OUT:-/out}"
BOOT="$CI/node_bootstrap.sh"

die() { echo "node_deps_build: ERROR: $*" >&2; exit 1; }
[ -f "$BOOT" ] || die "no $BOOT"
. /etc/os-release
[ "${VERSION_CODENAME:-}" = noble ] || die "build it on Ubuntu 24.04 (noble), the node's, this is ${PRETTY_NAME:-unknown}"

value() { sed -n "s/^$1=//p" "$BOOT" | head -1; }
GLSLANG_VERSION="$(value GLSLANG_VERSION)"
GLSLANG_SHA256="$(value GLSLANG_SHA256)"
# The PACKAGES=( ... ) array, comments dropped.
mapfile -t PACKAGES < <(sed -n '/^PACKAGES=(/,/^)/p' "$BOOT" | sed '1d;$d' | sed 's/#.*//' | tr -s ' \t' '\n' | sed '/^$/d')
[ -n "$GLSLANG_VERSION" ] && [ -n "$GLSLANG_SHA256" ] && [ "${#PACKAGES[@]}" -gt 10 ] \
  || die "could not read glslang's version or the package list from $BOOT"
MIRROR="${NODE_DEPS_MIRROR:-http://archive.ubuntu.com/ubuntu}"
echo "== ${#PACKAGES[@]} packages from $MIRROR, glslang $GLSLANG_VERSION"

export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
printf '%s\n' 'Acquire::http::Timeout "60";' 'Acquire::https::Timeout "60";' 'Acquire::Retries "10";' \
  > /etc/apt/apt.conf.d/80-ci-network
# For a mirror that redirects to HTTPS: this image has no CA certificates.
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates >/dev/null
for pocket in noble noble-updates noble-security; do
  echo "deb $MIRROR $pocket main restricted universe multiverse"
done > /etc/apt/sources.list
rm -f /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources
# Built once, by hand: it can wait out an outage.
for try in $(seq "${NODE_DEPS_TRIES:-60}"); do
  apt-get update -qq -o APT::Update::Error-Mode=any && break
  [ "$try" != "${NODE_DEPS_TRIES:-60}" ] || die "the indexes of $MIRROR did not download"
  echo "   the indexes of $MIRROR failed (try $try), again in 60 s"; sleep 60
done

STAGE="$(mktemp -d)"
DEBS="$STAGE/debs"
mkdir -p "$DEBS/partial" "$STAGE/empty"
: > "$STAGE/empty/status"
echo "== every .deb the list needs, as on a machine with nothing installed"
apt-get -qq -o Dir::State::status="$STAGE/empty/status" -o Dir::Cache::archives="$DEBS" \
  install -y --download-only --no-install-recommends "${PACKAGES[@]}" >/dev/null
rm -rf "$DEBS/partial" "$DEBS/lock"
echo "   $(ls "$DEBS"/*.deb | wc -l) files, $(du -sh "$DEBS" | cut -f1)"

echo "== the index apt reads from disk"
apt-get install -y -qq --no-install-recommends dpkg-dev >/dev/null
(cd "$DEBS" && dpkg-scanpackages --multiversion . /dev/null > Packages 2>/dev/null)
grep -c '^Package:' "$DEBS/Packages" | sed 's/^/   entries: /'

echo "== glslang $GLSLANG_VERSION, built here as node_bootstrap.sh would"
apt-get install -y -qq --no-install-recommends curl cmake ninja-build g++ python3 >/dev/null
curl -fsSL --retry 5 -o "$STAGE/glslang.tar.gz" "https://github.com/KhronosGroup/glslang/archive/refs/tags/$GLSLANG_VERSION.tar.gz"
echo "$GLSLANG_SHA256  $STAGE/glslang.tar.gz" | sha256sum -c --quiet - || die "glslang does not match its sha256"
mkdir -p "$STAGE/glslang-src"
tar -xzf "$STAGE/glslang.tar.gz" --strip-components=1 -C "$STAGE/glslang-src"
cmake -S "$STAGE/glslang-src" -B "$STAGE/glslang-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF \
  -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=/usr/local >/dev/null
DESTDIR="$STAGE/glslang-root" ninja -C "$STAGE/glslang-build" install >/dev/null
"$STAGE/glslang-root/usr/local/bin/glslangValidator" --version | grep -q ":$GLSLANG_VERSION\$" || die "the glslang built is not $GLSLANG_VERSION"
rm -rf "$STAGE/glslang-src" "$STAGE/glslang-build" "$STAGE/glslang.tar.gz" "$STAGE/empty"

# The name says what is in it: the day it was taken, and a hash of every .deb by name
# (package, version, architecture) and of glslang.
HASH="$( (cd "$DEBS" && ls -- *.deb; echo "glslang $GLSLANG_VERSION $GLSLANG_SHA256") | sha256sum | cut -c1-12)"
NAME="node-deps-$(date -u +%Y%m%d)-$HASH"
{ echo "name $NAME"; echo "mirror $MIRROR"; echo "taken $(date -u +%FT%TZ)"; echo "glslang $GLSLANG_VERSION"
  echo "packages ${PACKAGES[*]}"; echo "debs:"; (cd "$DEBS" && ls -- *.deb); } > "$STAGE/MANIFEST"
echo "== the archive"
mkdir -p "$OUT"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner -C "$STAGE" -cf "$OUT/$NAME.tar" MANIFEST debs glslang-root
(cd "$OUT" && sha256sum "$NAME.tar" > "$NAME.tar.sha256")
echo "node_deps_build: $OUT/$NAME.tar $(du -h "$OUT/$NAME.tar" | cut -f1) sha256 $(cut -d' ' -f1 "$OUT/$NAME.tar.sha256")"
