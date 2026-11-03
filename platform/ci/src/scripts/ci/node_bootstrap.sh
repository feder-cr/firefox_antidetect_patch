#!/usr/bin/env bash
# Prepare a Theta node to run release.yml's build job, as root, on the image's
# Ubuntu 22.04:
#
#     ssh root@node 'bash -s' < scripts/ci/node_bootstrap.sh
#
# What a job on ubuntu-24.04 finds already installed, this installs, at fixed
# versions: the apt packages from one dated snapshot (apt checks them against
# the archive's signature), the rest by version and sha256. A user `runner` with
# home /home/runner, as on GitHub's runners, so the job's paths (HOME, ~/pgo,
# ~/.mozbuild) are the same; the runner itself in ~/actions-runner, jobs in ~/work,
# Python's in /opt/hostedtoolcache. Unlike GitHub's, this user has no sudo: what a job
# would install with it is installed here, and a job has no way to root.
#
# It registers nothing: scripts/ci/node_runner.sh starts the runner, with
# its configuration on stdin. It can run again on a prepared node.
#
# Everything it needs is checked at the end, as that user: a missing tool stops
# here, by name, not an hour into a build.
set -euo pipefail

# Every version in one place.
APT_SNAPSHOT=20261103T000000Z
RUNNER_VERSION=2.338.2
RUNNER_SHA256=baebf204307bac6193480852732f0aba2ae97b6c5597c244f4d5558390147c40
NODE_VERSION=22.23.4
NODE_SHA256=1805bcd35e6815559bd2eb97db438d3fdb0eb3a5e6a977b7c1badca76337e9ce
RUSTUP_VERSION=1.29.3
RUSTUP_SHA256=b1c8cb14e30889e7e2999e6b69e62d9e9b9ffc0f4df8180564d306d9680e6c55
# Ubuntu 24.04's. 22.04's is 11.8, and lavapipe's shaders need 12.2.
GLSLANG_VERSION=14.0.0
GLSLANG_SHA256=80bbb916a23e94ea9cbfb1acb5d1a44a7e0c9613bcf5b5947c03f2273bdc92b0

# The image's sources.list carries no security pocket and follows the archive.
PACKAGES=(
  # the job's steps; sudo is for this script's as_runner
  sudo git curl ca-certificates python3 python3-venv
  build-essential binutils util-linux make m4 pkg-config patch file
  tar gzip bzip2 xz-utils zstd zip unzip rsync procps psmisc
  # the win-x86_64 leg (extra_pkgs in scripts/ci/legs.py)
  # (libc6-i386: Mozilla's wine, which runs fxc.exe, opens /lib/ld-linux.so.2; the hosted image has it)
  msitools p7zip-full libc6-i386 lib32gcc-s1 lib32stdc++6
  # lavapipe (scripts/ci/lavapipe_build.py)
  cmake ninja-build bison flex patchelf
  # what the runner links against
  libicu70 libkrb5-3 liblttng-ust1 libssl3 zlib1g
  # what `mach package` starts: xpcshell loads a libxul linked against these
  libgtk-3-0 libasound2 libdbus-glib-1-2 libx11-xcb1 libxt6 libxtst6 libpci3
)

die() { echo "node_bootstrap: ERROR: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run it as root"
. /etc/os-release
[ "${VERSION_CODENAME:-}" = jammy ] || die "written for Ubuntu 22.04 (jammy), this is ${PRETTY_NAME:-unknown}"

export DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8

fetch() { # url sha256 file
  [ -f "$3" ] && echo "$2  $3" | sha256sum -c --quiet - 2>/dev/null && return 0
  curl -fsSL --retry 5 --retry-delay 5 --connect-timeout 30 --max-time 900 -o "$3" "$1"
  echo "$2  $3" | sha256sum -c --quiet - || die "$1 does not match its sha256"
}

echo "== apt, snapshot $APT_SNAPSHOT"
# apt has no network timeout of its own (run 41248997155 waited 5h40 on a mirror).
printf '%s\n' 'Acquire::http::Timeout "60";' 'Acquire::https::Timeout "60";' 'Acquire::Retries "5";' \
  > /etc/apt/apt.conf.d/80-ci-network
# The snapshot service answers plain HTTP with a redirect to HTTPS, and the image has
# no CA certificates: on a fresh node every index failed and only "Unable to locate
# package" was left to read. So they come first, from the image's own sources.
apt-get update -qq -o APT::Update::Error-Mode=any
apt-get install -y -qq --no-install-recommends ca-certificates >/dev/null
{
  for pocket in jammy jammy-updates jammy-security; do
    echo "deb http://snapshot.ubuntu.com/ubuntu/$APT_SNAPSHOT $pocket main restricted universe multiverse"
  done
} > /etc/apt/sources.list
rm -f /etc/apt/sources.list.d/*.list
# A failed index is an error here, not a warning and a package missing later.
apt-get update -qq -o APT::Update::Error-Mode=any
apt-get install -y -qq --no-install-recommends "${PACKAGES[@]}" >/dev/null

echo "== the user"
id runner >/dev/null 2>&1 || useradd --create-home --home-dir /home/runner --uid 1001 --shell /bin/bash runner
rm -f /etc/sudoers.d/runner
# sudo looks the machine's name up on every call, and warns when it cannot.
grep -q "[[:space:]]$(hostname)\$" /etc/hosts || echo "127.0.0.1 $(hostname)" >> /etc/hosts
install -d -o runner -g runner /home/runner/work /opt/hostedtoolcache

as_runner() { sudo -n -u runner -H env -i HOME=/home/runner PATH=/home/runner/.cargo/bin:/opt/node/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin LANG=C.UTF-8 "$@"; }

echo "== Node.js $NODE_VERSION"
if [ "$(/opt/node/bin/node --version 2>/dev/null)" != "v$NODE_VERSION" ]; then
  fetch "https://nodejs.org/dist/v$NODE_VERSION/node-v$NODE_VERSION-linux-x64.tar.xz" "$NODE_SHA256" /tmp/node.tar.xz
  rm -rf /opt/node && mkdir -p /opt/node
  tar -xJf /tmp/node.tar.xz --strip-components=1 -C /opt/node
  rm -f /tmp/node.tar.xz
fi

echo "== rustup $RUSTUP_VERSION (the job installs the retail's Rust with it)"
if [ "$(as_runner rustup --version 2>/dev/null | awk 'NR==1{print $2}')" != "$RUSTUP_VERSION" ]; then
  fetch "https://static.rust-lang.org/rustup/archive/$RUSTUP_VERSION/x86_64-unknown-linux-gnu/rustup-init" \
    "$RUSTUP_SHA256" /tmp/rustup-init
  chmod 755 /tmp/rustup-init
  as_runner /tmp/rustup-init -y --no-modify-path --default-toolchain none --profile minimal >/dev/null
  rm -f /tmp/rustup-init
fi

echo "== glslang $GLSLANG_VERSION, in /usr/local/bin"
if ! /usr/local/bin/glslangValidator --version 2>/dev/null | grep -q ":$GLSLANG_VERSION\$"; then
  fetch "https://github.com/KhronosGroup/glslang/archive/refs/tags/$GLSLANG_VERSION.tar.gz" "$GLSLANG_SHA256" /tmp/glslang.tar.gz
  rm -rf /tmp/glslang && mkdir -p /tmp/glslang
  tar -xzf /tmp/glslang.tar.gz --strip-components=1 -C /tmp/glslang
  cmake -S /tmp/glslang -B /tmp/glslang/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_OPT=OFF \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DCMAKE_INSTALL_PREFIX=/usr/local >/dev/null
  ninja -C /tmp/glslang/build install >/dev/null
  rm -rf /tmp/glslang /tmp/glslang.tar.gz
fi

echo "== actions-runner $RUNNER_VERSION"
R=/home/runner/actions-runner
if [ "$(as_runner "$R/bin/Runner.Listener" --version 2>/dev/null)" != "$RUNNER_VERSION" ]; then
  fetch "https://github.com/actions/runner/releases/download/v$RUNNER_VERSION/actions-runner-linux-x64-$RUNNER_VERSION.tar.gz" \
    "$RUNNER_SHA256" /tmp/actions-runner.tar.gz
  rm -rf "$R" && install -d -o runner -g runner "$R"
  as_runner tar -xzf /tmp/actions-runner.tar.gz -C "$R"
  rm -f /tmp/actions-runner.tar.gz
fi
# What config.sh would write, and a JIT runner never runs config.sh: the
# environment every job starts from, and where setup-python keeps its Python.
as_runner sh -c "cat > $R/.env" <<'EOF'
LANG=C.UTF-8
AGENT_TOOLSDIRECTORY=/opt/hostedtoolcache
RUNNER_TOOL_CACHE=/opt/hostedtoolcache
EOF
as_runner sh -c "echo /home/runner/.cargo/bin:/opt/node/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin > $R/.path"

echo "== everything the job counts on, as the user that runs it"
missing=()
for tool in git curl python3 gcc g++ make strip tar xz zstd zip unzip 7z msiextract cmake ninja \
            glslangValidator bison flex patchelf node rustup; do
  as_runner sh -c "command -v $tool" >/dev/null || missing+=("$tool")
done
! as_runner sudo -n true 2>/dev/null || missing+=("a user without sudo")
[ "$(as_runner sh -c 'echo $HOME')" = /home/runner ] || missing+=("HOME=/home/runner")
[ -w /home/runner/work ] && as_runner test -w /home/runner/work || missing+=("a writable /home/runner/work")
as_runner test -w /opt/hostedtoolcache || missing+=("a writable tool cache")
[ "$(as_runner "$R/bin/Runner.Listener" --version 2>/dev/null)" = "$RUNNER_VERSION" ] \
  || missing+=("a runner that starts ($(as_runner "$R/bin/Runner.Listener" --version 2>&1 | head -1))")
[ "$(as_runner node --version)" = "v$NODE_VERSION" ] || missing+=("Node.js $NODE_VERSION")
/usr/local/bin/glslangValidator --version | grep -q ":$GLSLANG_VERSION\$" || missing+=("glslang $GLSLANG_VERSION")
[ "${#missing[@]}" = 0 ] || die "missing: ${missing[*]}"

echo "== a host key of this node's own"
# The image's is the same on every node (measured). theta_node.py pins this one, read
# from this session, for every later connection.
rm -f /etc/ssh/ssh_host_*
ssh-keygen -A >/dev/null
kill -HUP "$(pgrep -o -x sshd)"
host_key="$(cut -d' ' -f1,2 /etc/ssh/ssh_host_ed25519_key.pub)"
served=""
for _ in $(seq 30); do
  # awk, not cut: ssh-keyscan's "# host banner" line goes to stdout in newer OpenSSH.
  served="$(ssh-keyscan -T 2 -t ed25519 127.0.0.1 2>/dev/null | awk '$1 !~ /^#/ {print $2, $3}' || true)"
  [ "$served" != "$host_key" ] || break
  sleep 1
done
[ "$served" = "$host_key" ] || die "sshd does not serve the host key just made"
echo "node_bootstrap: host key: $host_key"
echo "node_bootstrap: ready (runner $RUNNER_VERSION, node $NODE_VERSION, rustup $RUSTUP_VERSION, glslang $GLSLANG_VERSION, apt snapshot $APT_SNAPSHOT)"
