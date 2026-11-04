#!/usr/bin/env bash
# Start the runner of one leg of a release.yml run on its node:
#
#     bash node_runner.sh RUN LEG    # on the node, as root: one JIT config on stdin
#
# One node, both x86_64 legs at the same time, one runner each: a JIT runner takes one
# job and exits, and the node is deleted after both. A leg's link spends most of its
# time on one core of 24, so the other leg fills what it leaves (measured 2026-11-04:
# each leg alone used 15-17% of the machine's core-minutes).
#
# The two legs share nothing that is theirs. Each has its own user (so the guard and
# memory_watch.py, which kill by user, can never touch the other leg), its own copy of
# the prepared /home/runner, of /opt/hostedtoolcache and of /tmp, mounted at the SAME
# paths in a mount namespace of its own (the working directory enters sccache's key for
# Rust: a leg must see /home/runner/work as it always has), its own sccache port, and
# its own cgroup, with half the memory as its ceiling: memory_watch.py and the compile
# jobs read the cgroup's limits (memory_watch.py, mozconfig.node_parallel), so neither
# leg can take the memory the other needs to link.
#
# The runner starts from an empty environment: every inherited CARGO_* variable and
# the working directory enter sccache's key for Rust, and root's environment
# has no business in it. What a job finds comes from here, ~/actions-runner/.env and
# .path, and the job's own env.
#
# A job of any other run dies at its first step, a script the runner calls, root's in a
# directory of root's: a job can move a file in its own home, even one it cannot write
# (measured). The node is only ever for this run's legs. A failed step alone would
# not do: the steps after it still evaluate their own `if`, and `always()` runs. So the
# script kills every process of the leg's user, the Worker that would run them included.
# The leg stays queued with no runner, and the watch fails the run (STUCK_S in theta_node.py).
#
# The configuration comes on stdin so that no command line shows it. It returns as
# soon as it has read it; the runner goes on detached, with its log on the node.
set -u

TEMPLATE="${NODE_RUNNER_TEMPLATE:-/home/runner}"
HOME_DIR="${NODE_RUNNER_HOME:-/home/runner}"
TOOLCACHE="${NODE_RUNNER_TOOLCACHE:-/opt/hostedtoolcache}"
TMP_DIR="${NODE_RUNNER_TMP:-/tmp}"
CGROUP_DIR="${NODE_RUNNER_CGROUP:-/sys/fs/cgroup}"
ROOT="${NODE_RUNNER_ROOT:-/srv/legs}"
MEMINFO="${NODE_RUNNER_MEMINFO:-/proc/meminfo}"
RUNNER_PATH=/home/runner/.cargo/bin:/opt/node/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

RUN="${1:-}"
LEG="${2:-}"
case "$RUN" in
  ''|*[!0-9]*) echo "node_runner: ERROR: the run id is the first argument, got '$RUN'" >&2; exit 1 ;;
esac
case "$LEG" in
  [a-z]) ;;
  *) echo "node_runner: ERROR: the leg's letter is the second argument, got '$LEG'" >&2; exit 1 ;;
esac
config=""
IFS= read -r config || true
[ -n "$config" ] || { echo "node_runner: ERROR: no runner configuration on stdin" >&2; exit 1; }

# a=1 ... z=26: the leg's user id, its sccache port and its unit, from its letter.
N=$(( $(printf '%d' "'$LEG") - 96 ))
USER_NAME="runner-$LEG"
LEG_ROOT="$ROOT/$LEG"
GUARD="${NODE_RUNNER_GUARD:-/usr/local/sbin/job_guard}-$LEG.sh"
LOG="${NODE_RUNNER_LOG:-/var/log/node_runner}-$LEG.log"
# sccache's default is 4226; each leg its own, so two servers never meet.
PORT=$(( 4300 + N ))
# Half of what the machine has, less 2 GB for the system: each leg's ceiling.
MEM_KB="$(awk '/^MemTotal:/ {print $2}' "$MEMINFO")"
LEG_MEM_MB=$(( (MEM_KB / 1024 - 4096) / 2 ))

run_one() {
  trap '' HUP   # the SSH session that started it ends at once
  id -u "$USER_NAME" >/dev/null 2>&1 || useradd --no-create-home --home-dir /home/runner --uid $(( 1100 + N )) --shell /bin/bash "$USER_NAME"
  rm -rf "$LEG_ROOT"
  mkdir -p "$LEG_ROOT/tmp" "$LEG_ROOT/toolcache"
  cp -a "$TEMPLATE" "$LEG_ROOT/home"
  cp -a "$TOOLCACHE/." "$LEG_ROOT/toolcache/"
  install -d "$LEG_ROOT/home/work"
  chown -R "$USER_NAME:$USER_NAME" "$LEG_ROOT/home" "$LEG_ROOT/toolcache"
  chmod 1777 "$LEG_ROOT/tmp"
  # A build opens far more files than a login's default of 1024; GitHub's runners allow 65536.
  ulimit -n 65536
  rm -f "$GUARD"
  cat > "$GUARD" <<EOF
#!/bin/bash
[ "\$GITHUB_RUN_ID" = "$RUN" ] || { echo "node: this job is of run \$GITHUB_RUN_ID, and this node serves only run $RUN" >&2; pkill -KILL -u $USER_NAME; exit 1; }
EOF
  chown root:root "$GUARD"
  chmod 755 "$GUARD"
  echo "$(date -u +%FT%TZ) the runner of leg $LEG starts, for run $RUN, ceiling ${LEG_MEM_MB} MB"
  printf '%s\n' "$config" |
    systemd-run --scope --quiet --collect --unit="leg-$LEG-$RUN" -p "MemoryMax=${LEG_MEM_MB}M" -p MemorySwapMax=0 -- \
    unshare --mount --cgroup --propagation private -- \
    bash -c 'set -e
      mount --bind "$1/home" "$2"; mount --bind "$1/toolcache" "$3"; mount --bind "$1/tmp" "$4"
      # The cgroup2 of the host is already mounted there: lifted first, in this namespace only,
      # or the mount fails and the runner never starts (candidate 41810835741).
      umount -l "$5" || true; mount -t cgroup2 cgroup2 "$5"
      shift 5; cd "$HOME_DIR_IN/actions-runner"; exec "$@"' _ "$LEG_ROOT" "$HOME_DIR" "$TOOLCACHE" "$TMP_DIR" "$CGROUP_DIR" \
    runuser -u "$USER_NAME" -- env -i HOME="$HOME_DIR" PATH="$RUNNER_PATH" LANG=C.UTF-8 \
      SCCACHE_SERVER_PORT="$PORT" ACTIONS_RUNNER_HOOK_JOB_STARTED="$GUARD" \
      bash -c 'IFS= read -r ACTIONS_RUNNER_INPUT_JITCONFIG; export ACTIONS_RUNNER_INPUT_JITCONFIG; exec ./run.sh'
  code=$?
  echo "$(date -u +%FT%TZ) the runner of leg $LEG exited with $code"
}

export HOME_DIR_IN="$HOME_DIR"
run_one >> "$LOG" 2>&1 < /dev/null &
disown
echo "node_runner: the runner of leg $LEG started, log $LOG"
