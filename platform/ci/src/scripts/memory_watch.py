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
Every line also goes to --log, which the workflow uploads with build.log. The same
loop drives cpu_watch.py: its "[cpu]" lines and the samples behind its summary.

On a machine whose container has less memory than its host (the Latitude node: a
cgroup limit of 62.5 GB, no swap possible, and a /proc/meminfo that describes the
host) the limits are the cgroup's: what /proc/meminfo calls available and free
swap would be memory the kernel never gives this build, and the first sign would
be the cgroup's OOM killer choosing the victim. The thresholds grow with the
limit there; on a 16 GB runner they are what they always were.
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
# The machine those two were measured on. Larger ones keep the same proportion.
BASE_MB = 16000
CGROUP = Path("/sys/fs/cgroup")
# The runner's own processes: killing them is what freezes or ends the job.
SPARED = ("Runner.Worker", "Runner.Listener", "memory_watch.py")


def meminfo(proc: Path = Path("/proc")) -> dict:
    out = {}
    for line in (proc / "meminfo").read_text().splitlines():
        key, value = line.split(":", 1)
        out[key] = int(value.split()[0]) // 1024  # kB -> MB
    return out


def _cgroup_value(path: Path):
    """A cgroup file's number; None when it is absent or says "max"."""
    try:
        return int(path.read_text().split()[0])
    except (OSError, ValueError, IndexError):
        return None


def unreclaimable(cgroup: Path = CGROUP):
    """Bytes the cgroup holds that reclaim cannot free: anon, shmem, kernel memory and
    unreclaimable slab, from memory.stat. `kernel` already holds the slab on the kernels
    that have it, so only the reclaimable part comes off; older ones have no `kernel`,
    and its parts are added one by one. None without a usable memory.stat."""
    stat = {}
    try:
        for line in (cgroup / "memory.stat").read_text().splitlines():
            key, value = line.split()[:2]
            stat[key] = int(value)
    except (OSError, ValueError):
        return None
    if "anon" not in stat:
        return None
    used = stat["anon"] + stat.get("shmem", 0)
    if "kernel" in stat:
        return used + max(0, stat["kernel"] - stat.get("slab_reclaimable", 0))
    return used + stat.get("kernel_stack", 0) + stat.get("pagetables", 0) + stat.get("percpu", 0) \
        + stat.get("slab_unreclaimable", 0)


def memory(proc: Path = Path("/proc"), cgroup: Path = CGROUP) -> dict:
    """meminfo, with the container's limits where they are lower than the host's.

    Page cache counts as used in memory.current, and the kernel gives it back, active
    or not: a compile that has read the whole tree holds tens of GB of it. So what is
    available is the limit less what cannot be reclaimed (unreclaimable())."""
    m = meminfo(proc)
    limit = _cgroup_value(cgroup / "memory.max")
    current = _cgroup_value(cgroup / "memory.current")
    if limit is not None and current is not None and limit // 2**20 < m["MemTotal"]:
        used = unreclaimable(cgroup)
        if used is None:
            used = current
        m["MemTotal"] = limit // 2**20
        m["MemAvailable"] = min(m["MemAvailable"], max(0, limit - used) // 2**20)
        swap_limit = _cgroup_value(cgroup / "memory.swap.max")
        if swap_limit is not None:
            swap_used = _cgroup_value(cgroup / "memory.swap.current") or 0
            m["SwapTotal"] = min(m["SwapTotal"], swap_limit // 2**20)
            m["SwapFree"] = min(m["SwapFree"], max(0, swap_limit - swap_used) // 2**20)
    return m


def huge_pages_mb(proc: Path = Path("/proc"), cgroup: Path = CGROUP):
    """Anonymous memory in transparent huge pages: this container's, or the machine's
    where it has no memory.stat. None where neither says. It tells whether the node's
    linker got the huge pages it asks for (scripts/ci/linker_shim.py)."""
    try:
        for line in (cgroup / "memory.stat").read_text().splitlines():
            if line.startswith("anon_thp "):
                return int(line.split()[1]) // 2**20
    except (OSError, ValueError, IndexError):
        pass
    try:
        return meminfo(proc).get("AnonHugePages")
    except (OSError, ValueError, IndexError):
        return None


def cpu_limit(cgroup: Path = CGROUP) -> int:
    """The CPUs this container may use: cpu.max's quota, or what the kernel reports."""
    cpus = os.cpu_count() or 1
    try:
        quota, period = (cgroup / "cpu.max").read_text().split()[:2]
        cpus = min(cpus, max(1, int(quota) // int(period)))
    except (OSError, ValueError):
        pass
    return cpus


def thresholds(total_mb: int) -> tuple:
    """(low, kill) in MB: the measured ones, in proportion on a larger machine."""
    return (max(LOW_MB, LOW_MB * total_mb // BASE_MB), max(KILL_MB, KILL_MB * total_mb // BASE_MB))


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


def cpu_sampler(path: Path):
    """cpu_watch.py's sampler, or None: this file's job does not depend on that one."""
    try:
        import cpu_watch
        return cpu_watch.Sampler(path, available=cpu_limit())
    except Exception as exc:
        print("[cpu] no CPU samples: %s" % exc, flush=True)
        return None


def cpu_tick(sampler, say):
    """One tick of the sampler; what to keep for the next. An error costs the CPU
    samples and nothing else: this process keeps the runner alive."""
    if sampler is None:
        return None
    try:
        line = sampler.tick()
        if line:
            say(line)
        return sampler
    except Exception as exc:
        say("[cpu] no more CPU samples: %s" % exc)
        return None


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--every", type=float, default=5.0)
    ap.add_argument("--cpu-samples", type=Path, default=Path("cpuwatch.jsonl"))
    args = ap.parse_args(argv)
    log = open(args.log, "a", encoding="utf-8")
    cpu = cpu_sampler(args.cpu_samples)

    def say(text):
        line = "%s %s" % (time.strftime("%H:%M:%S"), text)
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    last_status = last_top = 0.0
    starving = 0
    while True:
        m = memory()
        low_mb, kill_mb = thresholds(m["MemTotal"])
        avail = m["MemAvailable"]
        swap_used = m["SwapTotal"] - m["SwapFree"]
        now = time.monotonic()
        if now - last_status >= 60:
            disk = shutil.disk_usage("/").free // 2**30
            thp = huge_pages_mb()
            say("[mem] available %d/%d MB, swap used %d/%d MB, disk free %d GB%s"
                % (avail, m["MemTotal"], swap_used, m["SwapTotal"], disk, "" if thp is None else ", huge pages %d MB" % thp))
            last_status = now
        if avail < low_mb and now - last_top >= 15:
            top = processes()[:6]
            say("[mem] available %d MB, largest: %s" % (
                avail, "; ".join("%d MB pid %d %s" % (r, p, c[:160]) for r, p, c in top)))
            last_top = now
        starving = starving + 1 if left_mb(m) < kill_mb else 0
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
        cpu = cpu_tick(cpu, say)
        time.sleep(args.every)


if __name__ == "__main__":
    sys.exit(main())
