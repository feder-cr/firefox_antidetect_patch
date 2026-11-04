#!/usr/bin/env bash
# Start the one runner of a leg of a release.yml run on its node:
#
#     bash node_runner.sh RUN    # on the node, as root: one JIT config on stdin
#
# One node, one runner, one job: a JIT runner takes one job and exits, and the node is
# deleted after it.
#
# The runner starts from an empty environment: every inherited CARGO_* variable and
# the working directory enter sccache's key for Rust, and root's environment
# has no business in it. What a job finds comes from here, ~/actions-runner/.env and
# .path, and the job's own env.
#
# A job of any other run dies at its first step, a script the runner calls, root's in a
# directory of root's: a job can move a file in its own home, even one it cannot write
# (measured). The node is only ever for this run's leg. A failed step alone would
# not do: the steps after it still evaluate their own `if`, and `always()` runs. So the
# script kills every process of the user, the Worker that would run them included. The
# leg stays queued with no runner, and the watch fails the run (STUCK_S in theta_node.py).
#
# The configuration comes on stdin so that no command line shows it. It returns as
# soon as it has read it; the runner goes on detached, with its log on the node.
set -u

HOME_DIR="${NODE_RUNNER_HOME:-/home/runner}"
LOG="${NODE_RUNNER_LOG:-/var/log/node_runner.log}"
RUNNER_PATH=/home/runner/.cargo/bin:/opt/node/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
GUARD="${NODE_RUNNER_GUARD:-/usr/local/sbin/job_guard.sh}"

RUN="${1:-}"
case "$RUN" in
  ''|*[!0-9]*) echo "node_runner: ERROR: the run id is the argument, got '$RUN'" >&2; exit 1 ;;
esac
config=""
IFS= read -r config || true
[ -n "$config" ] || { echo "node_runner: ERROR: no runner configuration on stdin" >&2; exit 1; }

run_one() {
  trap '' HUP   # the SSH session that started it ends at once
  install -d -o runner -g runner "$HOME_DIR/work"
  # A build opens far more files than a login's default of 1024; GitHub's runners allow 65536.
  ulimit -n 65536
  rm -f "$GUARD"
  cat > "$GUARD" <<EOF
#!/bin/bash
[ "\$GITHUB_RUN_ID" = "$RUN" ] || { echo "node: this job is of run \$GITHUB_RUN_ID, and this node serves only run $RUN" >&2; pkill -KILL -u runner; exit 1; }
EOF
  chown root:root "$GUARD"
  chmod 755 "$GUARD"
  echo "$(date -u +%FT%TZ) the runner starts, for run $RUN"
  ( cd "$HOME_DIR/actions-runner" &&
    printf '%s\n' "$config" |
    runuser -u runner -- env -i HOME="$HOME_DIR" PATH="$RUNNER_PATH" LANG=C.UTF-8 \
      ACTIONS_RUNNER_HOOK_JOB_STARTED="$GUARD" \
      bash -c 'IFS= read -r ACTIONS_RUNNER_INPUT_JITCONFIG; export ACTIONS_RUNNER_INPUT_JITCONFIG; exec ./run.sh' )
  code=$?
  echo "$(date -u +%FT%TZ) the runner exited with $code"
}

run_one >> "$LOG" 2>&1 < /dev/null &
disown
echo "node_runner: the runner started, log $LOG"
