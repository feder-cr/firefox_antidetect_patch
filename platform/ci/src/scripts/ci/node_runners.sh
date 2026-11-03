#!/usr/bin/env bash
# Run the build jobs of one release.yml run on the Theta node, one after the other:
#
#     bash node_runners.sh    # on the node, as root: JIT configs on stdin, one per line
#
# One machine, two x86_64 legs: 62 GB cannot hold two full-LTO links, so each leg
# has a runner of its own, and the next starts only when the last has exited. A
# JIT runner takes one job and exits; between two of them nothing of the first
# job is left, neither its processes nor its tree nor its downloads (~/pgo holds
# the PGO profile of ONE leg, under the same name on both).
#
# The configurations come on stdin so that no command line shows them. It returns
# as soon as it has read them; the loop goes on detached, with its log on the node.
set -u

HOME_DIR="${NODE_RUNNER_HOME:-/home/runner}"
LOG="${NODE_RUNNER_LOG:-/var/log/node_runners.log}"
TMP_DIR="${NODE_RUNNER_TMP:-/tmp}"

configs=()
while IFS= read -r line; do
  [ -n "$line" ] && configs+=("$line")
done
[ "${#configs[@]}" -gt 0 ] || { echo "node_runners: ERROR: no runner configuration on stdin" >&2; exit 1; }

clean() {
  pkill -KILL -u runner 2>/dev/null
  rm -rf "$HOME_DIR/work" "$HOME_DIR/pgo" "$HOME_DIR/.mozbuild" "$HOME_DIR/.cache" "$TMP_DIR"/* 2>/dev/null
  install -d -o runner -g runner "$HOME_DIR/work"
}

loop() {
  trap '' HUP   # the SSH session that started it ends at once
  n=0
  for config in "${configs[@]}"; do
    n=$((n + 1))
    clean
    echo "$(date -u +%FT%TZ) runner $n of ${#configs[@]} starts"
    ( cd "$HOME_DIR/actions-runner" &&
      ACTIONS_RUNNER_INPUT_JITCONFIG="$config" runuser -u runner --preserve-environment -- env HOME="$HOME_DIR" ./run.sh )
    code=$?
    echo "$(date -u +%FT%TZ) runner $n exited with $code"
  done
  clean
  echo "$(date -u +%FT%TZ) all runners done"
}

loop >> "$LOG" 2>&1 < /dev/null &
disown
echo "node_runners: ${#configs[@]} runner(s) started, log $LOG"
