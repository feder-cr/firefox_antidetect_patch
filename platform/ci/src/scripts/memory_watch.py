"""Watch a CI leg's memory, name what fills it, and keep the runner alive.

    python3 -u scripts/memory_watch.py --log memwatch.log &

Why: the PGO + full LTO Windows legs of runs 40446702889 and 40466778026
stopped logging mid-build with memory at 3 of 16 GB in the last two-minute
sample, and the runners never answered again (a cancel took 47 minutes): a
burst between two samples pushed the machine into swap and froze it. Before
the 16 GB swap the same burst killed the runner outright (run 40441908109).
Neither way left a line saying which process it was.

So every 5 seconds:
  - a "[mem]" line every minute: memory, swap, free disk;
  - below LOW_MB available, the largest processes by resident memory, at most
    every 15 seconds, so the burst has a name;
  - below KILL_MB of memory AND swap left for two samples in a row, the
    largest process that is not the runner itself is killed. The build then
    fails with a named culprit and its logs upload, instead of a frozen
    machine. Swap counts: the first version judged memory alone and killed
    ld.lld linking libxul.so with full LTO at 14 GB (run 40500378511), a link
    that finishes in swap (run 40466778026).
Every line also goes to --log, which the workflow uploads with build.log.
"""
from __future__ import annotations

import argparse
import os
import shutil
import signal
import sys
import time
from pathlib import Path

LOW_MB = 3000
KILL_MB = 600
# The runner's own processes: killing them is what freezes or ends the job.
SPARED = ("Runner.Worker", "Runner.Listener", "memory_watch.py")


def meminfo(proc: Path = Path("/proc")) -> dict:
    out = {}
    for line in (proc / "meminfo").read_text().splitlines():
        key, value = line.split(":", 1)
        out[key] = int(value.split()[0]) // 1024  # kB -> MB
    return out


def processes(proc: Path = Path("/proc")) -> list:
    """(rss MB, pid, command line) of every process, largest first."""
    found = []
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            rss = 0
            for line in (entry / "status").read_text().splitlines():
                if line.startswith("VmRSS:"):
                    rss = int(line.split()[1]) // 1024
            cmd = (entry / "cmdline").read_bytes().replace(b"\0", b" ").decode("utf-8", "replace").strip()
        except (OSError, ValueError):
            continue  # gone meanwhile, or a kernel thread
        if cmd:
            found.append((rss, int(entry.name), cmd))
    return sorted(found, reverse=True)


def left_mb(m: dict) -> int:
    """What a process can still get before the kernel has nothing: memory, then swap."""
    return m["MemAvailable"] + m["SwapFree"]


def victim(procs: list, me: int):
    for rss, pid, cmd in procs:
        if pid != me and not any(s in cmd for s in SPARED):
            return rss, pid, cmd
    return None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--every", type=float, default=5.0)
    args = ap.parse_args(argv)
    log = open(args.log, "a", encoding="utf-8")

    def say(text):
        line = "%s %s" % (time.strftime("%H:%M:%S"), text)
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    last_status = last_top = 0.0
    starving = 0
    while True:
        m = meminfo()
        avail = m["MemAvailable"]
        swap_used = m["SwapTotal"] - m["SwapFree"]
        now = time.monotonic()
        if now - last_status >= 60:
            disk = shutil.disk_usage("/").free // 2**30
            say("[mem] available %d/%d MB, swap used %d/%d MB, disk free %d GB"
                % (avail, m["MemTotal"], swap_used, m["SwapTotal"], disk))
            last_status = now
        if avail < LOW_MB and now - last_top >= 15:
            top = processes()[:6]
            say("[mem] available %d MB, largest: %s" % (
                avail, "; ".join("%d MB pid %d %s" % (r, p, c[:160]) for r, p, c in top)))
            last_top = now
        starving = starving + 1 if left_mb(m) < KILL_MB else 0
        if starving >= 2:
            hit = victim(processes(), os.getpid())
            if hit:
                rss, pid, cmd = hit
                say("[mem] KILLED pid %d (%d MB) to keep the runner alive: %s" % (pid, rss, cmd[:400]))
                try:
                    os.kill(pid, signal.SIGKILL)
                except OSError:
                    pass
            starving = 0
        time.sleep(args.every)


if __name__ == "__main__":
    sys.exit(main())
