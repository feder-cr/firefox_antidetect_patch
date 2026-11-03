#!/usr/bin/env bash
# Run the build job of one leg of a release.yml run on the Theta node:
#
#     bash node_runners.sh RUN    # on the node, as root: one JIT config on stdin
#
# One machine, two x86_64 legs: 62 GB cannot hold two full-LTO links, so each leg
# has a runner of its own, registered when the leg is due, and the next starts only
# when the last has exited. A JIT runner takes one job and exits; between two of
# them nothing of the first job is left, neither its processes nor its tree nor its
# downloads (~/pgo holds the PGO profile of ONE leg, under the same name on both).
#
# The runner starts from an empty environment: every inherited CARGO_* variable and
# the working directory enter sccache's key for Rust, and root's container environment
# has no business in it. What a job finds comes from here, ~/actions-runner/.env and
# .path, and the job's own env.
#
# A job of any other run is refused before it starts, by a script the runner calls and
# the job cannot change: the node is only ever for this run's legs.
#
# The configuration comes on stdin so that no command line shows it. It returns as
# soon as it has read it; the runner goes on detached, with its log on the node.
set -u

HOME_DIR="${NODE_RUNNER_HOME:-/home/runner}"
LOG="${NODE_RUNNER_LOG:-/var/log/node_runners.log}"
TMP_DIR="${NODE_RUNNER_TMP:-/tmp}"
RUNNER_PATH=/home/runner/.cargo/bin:/opt/node/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
GUARD="$HOME_DIR/job_guard.sh"
PIDFILE="$LOG.pid"
# How long the runner before this one has to leave, once its job is over.
PREVIOUS_LIMIT_S="${NODE_RUNNER_PREVIOUS_LIMIT_S:-120}"

RUN="${1:-}"
case "$RUN" in
  ''|*[!0-9]*) echo "node_runners: ERROR: the run id is the argument, got '$RUN'" >&2; exit 1 ;;
esac
config=""
IFS= read -r config || true
[ -n "$config" ] || { echo "node_runners: ERROR: no runner configuration on stdin" >&2; exit 1; }

clean() {
  pkill -KILL -u runner 2>/dev/null
  rm -rf "$HOME_DIR/work" "$HOME_DIR/pgo" "$HOME_DIR/.mozbuild" "$HOME_DIR/.cache" "$TMP_DIR"/* 2>/dev/null
  install -d -o runner -g runner "$HOME_DIR/work"
}

previous="$(cat "$PIDFILE" 2>/dev/null)"

wait_previous() {
  [ -n "$previous" ] || return 0
  waited=0
  while kill -0 "$previous" 2>/dev/null && [ "$waited" -lt "$PREVIOUS_LIMIT_S" ]; do
    sleep 1
    waited=$((waited + 1))
  done
}

run_one() {
  trap '' HUP   # the SSH session that started it ends at once
  wait_previous
  clean
  rm -f "$GUARD"
  cat > "$GUARD" <<EOF
#!/bin/bash
[ "\$GITHUB_RUN_ID" = "$RUN" ] || { echo "node: this job is of run \$GITHUB_RUN_ID, and this node serves only run $RUN" >&2; exit 1; }
EOF
  chmod 755 "$GUARD"
  echo "$(date -u +%FT%TZ) a runner starts, for run $RUN"
  ( cd "$HOME_DIR/actions-runner" &&
    printf '%s\n' "$config" |
    runuser -u runner -- env -i HOME="$HOME_DIR" PATH="$RUNNER_PATH" LANG=C.UTF-8 \
      ACTIONS_RUNNER_HOOK_JOB_STARTED="$GUARD" \
      bash -c 'IFS= read -r ACTIONS_RUNNER_INPUT_JITCONFIG; export ACTIONS_RUNNER_INPUT_JITCONFIG; exec ./run.sh' )
  code=$?
  echo "$(date -u +%FT%TZ) the runner exited with $code"
}

run_one >> "$LOG" 2>&1 < /dev/null &
echo "$!" > "$PIDFILE"
disown
echo "node_runners: a runner started, log $LOG"
