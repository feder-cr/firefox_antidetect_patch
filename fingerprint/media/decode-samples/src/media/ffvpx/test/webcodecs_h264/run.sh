#!/bin/sh
# usage: run.sh <dist/bin of a Linux x86_64 build> <webcodecs wpt dir with h264.annexb>
# Builds the debian:12 image of the Dockerfile next to this script, runs the
# build's firefox in it under Xvfb with test.html, and exits with the result.
# WORK: a directory on the docker host to stage the page in (default .work).
# The container is removed on exit (--rm) and killed after 150 s.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=$1
wpt=$2
work=${WORK:-$here/.work}; rm -rf "$work"; mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
cp "$here/test.html" "$here/server.py" "$wpt/h264.annexb" "$work/"
docker build -q -t ffvpx-webcodecs-test "$here" >/dev/null
exec timeout 150 docker run --rm --init \
  -v "$bin:/ff:ro" -v "$work:/t:ro" ffvpx-webcodecs-test sh -c '
    set -e
    ldconfig -p | grep -c "libavcodec" && { echo "system libavcodec present"; exit 2; } || true
    mkdir -p /tmp/profile
    cat > /tmp/profile/user.js <<EOF
user_pref("dom.media.webcodecs.enabled", true);
user_pref("media.ffvpx.enabled", true);
user_pref("media.ffmpeg.enabled", false);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
EOF
    cd /t
    python3 server.py 100 &
    srv=$!
    xvfb-run -a /ff/firefox -no-remote -profile /tmp/profile http://127.0.0.1:8123/test.html &
    wait $srv
  '
