#!/usr/bin/env python3
"""Put this leg's libxul link on one group of cores, and say what the machine is.

    python3 scripts/ci/link_pin.py --mode large-l3|small-l3|off --log linkpin.log

Most of the libxul link is one thread (full LTO), and its speed is that core's. The
node's CPU, an EPYC 4484PX, has two groups of cores (CCDs), each with its own L3, of
different sizes; the same program ran 9.2 s on one core and 11.8-12.2 s on others
(measured 2026-11-04), and the two legs' links, together on one node, took 16.2 min
each against 13.5 alone. This finds the groups from sysfs, writes the machine's
topology and CPU frequency policy to the log, then waits for the process that links
libxul.so or xul.dll, and keeps it, and every thread it starts, on the group asked for.

Only a process of this leg's own user: the other leg runs on the same machine.
Affinity says where the linker runs, not what it writes: the bytes are the same.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time
from pathlib import Path

CPUS = Path("/sys/devices/system/cpu")
PROC = Path("/proc")
CPUINFO = PROC / "cpuinfo"
LINKERS = ("ld.lld", "lld-link")
OUTPUTS = re.compile(r"(^|[/\\:])(libxul\.so|xul\.dll)$")


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def parse_list(text: str) -> list:
    out = []
    for part in text.split(","):
        if "-" in part:
            a, b = part.split("-")
            out += list(range(int(a), int(b) + 1))
        elif part.strip():
            out.append(int(part))
    return out


def size_kb(text: str) -> int:
    m = re.match(r"^(\d+)([KMG])", text or "")
    return int(m.group(1)) * {"K": 1, "M": 1024, "G": 1024 * 1024}[m.group(2)] if m else 0


def groups(cpus: Path = CPUS) -> list:
    """[(L3 size in KB, [cpu, ...]), ...], the largest L3 first."""
    found = {}
    for cpu in sorted(cpus.glob("cpu[0-9]*")):
        for index in cpu.glob("cache/index*"):
            if read(index / "level") == "3":
                shared = read(index / "shared_cpu_list")
                found[shared] = size_kb(read(index / "size"))
    return sorted(((kb, parse_list(shared)) for shared, kb in found.items()), key=lambda g: (-g[0], g[1]))


def cpu_model(cpuinfo: Path = CPUINFO) -> str:
    """The CPU's name: the link took 13.9-16 min across nodes of one plan (04/11), and the
    plan does not say which CPU it gives."""
    for line in read(cpuinfo).splitlines():
        if line.lower().startswith("model name"):
            return line.split(":", 1)[1].strip()
    return "not exposed"


def machine(cpus: Path = CPUS, cpuinfo: Path = CPUINFO) -> list:
    """What decides a single thread's speed, one line each."""
    lines = ["cpu: %s" % cpu_model(cpuinfo),
             "L3 groups: %s" % "; ".join("%d MB on cpus %s" % (kb // 1024, ",".join(map(str, c))) for kb, c in groups(cpus))]
    for name in ("scaling_driver", "scaling_governor", "energy_performance_preference", "cpuinfo_max_freq", "scaling_max_freq"):
        values = sorted({read(c / "cpufreq" / name) for c in cpus.glob("cpu[0-9]*")} - {""})
        lines.append("%s: %s" % (name, ", ".join(values) or "not exposed"))
    lines.append("amd_pstate: %s" % (read(cpus / "amd_pstate" / "status") or "not exposed"))
    lines.append("boost: %s" % (read(cpus / "cpufreq" / "boost") or "not exposed"))
    lines.append("smt: %s" % (read(cpus / "smt" / "active") or "not exposed"))
    return lines


def links(uid: int, proc: Path = PROC) -> list:
    """pids of this user's processes that link libxul."""
    found = []
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid != uid:
                continue
            argv = (entry / "cmdline").read_bytes().split(b"\0")
        except OSError:
            continue
        if not argv or os.path.basename(argv[0].decode("utf-8", "replace")) not in LINKERS:
            continue
        words = [a.decode("utf-8", "replace") for a in argv]
        outs = [words[i + 1] for i, w in enumerate(words[:-1]) if w == "-o"] + [w.split(":", 1)[1] for w in words if w.lower().startswith(("/out:", "-out:"))]
        if any(OUTPUTS.search(o) for o in outs):
            found.append(int(entry.name))
    return found


def describe(pid: int, proc: Path = PROC) -> str:
    """Which binary runs the link, its parent, and its options: on 04/11 the Linux link
    ran with --time-trace and wrote no trace where lld writes one, and two processes
    named ld.lld linked libxul at once. Object files and libraries left out, they are
    thousands."""
    d = proc / str(pid)
    try:
        exe = os.readlink(str(d / "exe"))
    except OSError:
        exe = "unreadable"
    try:
        ppid = (d / "stat").read_text(encoding="utf-8", errors="replace").rsplit(")", 1)[1].split()[1]
    except (OSError, IndexError):
        ppid = "?"
    try:
        words = [a.decode("utf-8", "replace") for a in (d / "cmdline").read_bytes().split(b"\0") if a]
    except OSError:
        words = []
    options = [w for w in words[1:] if w.startswith("-") or w.startswith("/")
               and not w.endswith((".o", ".a", ".obj", ".lib", ".rlib"))]
    return "exe %s, parent %s, argv[0] %s, %d words, options: %s" % (
        exe, ppid, words[0] if words else "?", len(words), " ".join(options)[:1500])


def pin(pid: int, cpus: list, proc: Path = PROC, setaffinity=os.sched_setaffinity, getaffinity=os.sched_getaffinity) -> int:
    """Every thread of pid on cpus; how many were moved."""
    moved = 0
    try:
        tasks = [int(t.name) for t in (proc / str(pid) / "task").iterdir()]
    except OSError:
        return 0
    for tid in tasks:
        try:
            if getaffinity(tid) != set(cpus):
                setaffinity(tid, cpus)
                moved += 1
        except OSError:
            pass
    return moved


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=("large-l3", "small-l3", "off"), required=True)
    ap.add_argument("--log", type=Path, required=True)
    ap.add_argument("--poll", type=float, default=2.0)
    ap.add_argument("--limit-s", type=float, default=4 * 3600)
    a = ap.parse_args(argv)

    def say(text):
        line = "%s link_pin: %s" % (time.strftime("%H:%M:%S", time.gmtime()), text)
        print(line, flush=True)
        with a.log.open("a", encoding="utf-8") as f:
            f.write(line + "\n")

    for line in machine():
        say(line)
    g = groups()
    if a.mode == "off" or len(g) < 2:
        say("mode %s, %d L3 groups: nothing pinned" % (a.mode, len(g)))
        return 0
    target = g[0][1] if a.mode == "large-l3" else g[-1][1]
    say("mode %s: the libxul link goes on cpus %s" % (a.mode, ",".join(map(str, target))))
    seen, start = {}, time.time()
    while time.time() - start < a.limit_s:
        for pid in links(os.getuid()):
            moved = pin(pid, target)
            if pid not in seen:
                seen[pid] = time.time()
                say("pid %d links libxul: on cpus %s from now (%d threads moved)" % (pid, ",".join(map(str, target)), moved))
                say("pid %d: %s" % (pid, describe(pid)))
        for pid in [p for p in seen if not (PROC / str(p)).exists()]:
            say("pid %d done after %.1f min" % (pid, (time.time() - seen.pop(pid)) / 60))
            if not seen:
                return 0
        time.sleep(a.poll)
    say("time limit reached")
    return 0


if __name__ == "__main__":
    sys.exit(main())
