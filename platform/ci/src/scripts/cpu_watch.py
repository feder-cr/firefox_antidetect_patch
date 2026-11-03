"""Whether a node build uses the CPU well: samples for the log, a table for the summary.

memory_watch.py calls Sampler every few seconds, in the same process and with every
error swallowed: nothing here may ever touch a build. Each sample goes to
cpuwatch.jsonl, and about once a minute to the log as a "[cpu]" line: our use in cores
(the cgroup's cpu.stat, or the process tree where that cannot be read), the whole
machine's busy, iowait and steal from /proc/stat, runnable tasks, the top process and
its thread count.

    python3 scripts/ci cpu-report [--samples cpuwatch.jsonl] [--events events.jsonl]

at the end of the job writes the per-phase table and its warnings to the step summary.
The phases come from what the build did, never from how long it usually takes:

  build, package, symbols...  scripts/ci/phase.py's start and end events
  configure, compile          until mach's "Finished reading N moz.build files", then
                              until the first long link
  rust                        from cargo's first "Compiling" to its last "Finished"
  link <file>                 a linker process seen alive, by pid, named by its -o

Warnings, never failures: a phase over LONG_S outside the link whose mean is below
UNDERUSED_SHARE of the available cores, and a phase where the machine is busy with
something that is not us (the owner of the PC using it).

Its cases: scripts/test_cpu_watch.py.
"""
from __future__ import annotations

import json
import os
import re
import time
from pathlib import Path

CGROUP = Path("/sys/fs/cgroup")
SAMPLE_S = 15
LINE_S = 60
LONG_S = 600
# Mean cores below this share of the available ones, in a long phase outside the link.
UNDERUSED_SHARE = 0.5
# Cores busy with what is not ours: at least this many, and this share of the machine.
OTHERS_CORES = 4
OTHERS_SHARE = 0.15
# A link seen in fewer samples than this is too short to have a row.
LINK_MIN_SAMPLES = 2
LINKERS = ("ld", "ld.lld", "ld64.lld", "lld", "lld-link", "rust-lld")
OUTPUT = re.compile(r"(?:^|\s)(?:-o\s+|[-/]OUT:)(\S+)", re.I)

# What phase.py looks for in a build's output (mach's elapsed time comes first).
CONFIGURED = re.compile(r"Finished reading \d+ moz\.build files")
RUST_START = re.compile(r"^\s*[\d:.]*\s*Compiling \S+ v\d")
RUST_END = re.compile(r"^\s*[\d:.]*\s*Finished `\w+` profile")


def build_mark(line: str):
    """The marker a line of a build's output is, or None."""
    if CONFIGURED.search(line):
        return "configured"
    if RUST_START.search(line):
        return "rust-start"
    if RUST_END.search(line):
        return "rust-end"
    return None


def machine_counters(proc: Path) -> dict:
    """/proc/stat: the machine's ticks, its CPUs and its runnable tasks."""
    out = {"ncpu": 0, "running": 0}
    for line in (proc / "stat").read_text().splitlines():
        fields = line.split()
        if fields[0] == "cpu":
            # user nice system idle iowait irq softirq steal
            values = [int(v) for v in fields[1:9]] + [0] * 8
            out.update(total=sum(values[:8]), idle=values[3], iowait=values[4], steal=values[7])
        elif re.fullmatch(r"cpu\d+", fields[0]):
            out["ncpu"] += 1
        elif fields[0] == "procs_running":
            out["running"] = int(fields[1])
    return out


def cgroup_seconds(cgroup: Path):
    """The container's CPU time from cpu.stat; None where it cannot be read."""
    try:
        for line in (cgroup / "cpu.stat").read_text().splitlines():
            if line.startswith("usage_usec "):
                return int(line.split()[1]) / 1e6
    except (OSError, ValueError, IndexError):
        pass
    return None


def process_table(proc: Path) -> dict:
    """pid -> (comm, own ticks, ticks with reaped children, threads). A process that
    ends is counted in its parent's children's ticks, so the sum keeps growing."""
    table = {}
    for entry in proc.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            text = (entry / "stat").read_text()
            comm = text[text.index("(") + 1:text.rindex(")")]
            fields = text[text.rindex(")") + 2:].split()
            own = int(fields[11]) + int(fields[12])
            table[int(entry.name)] = (comm, own, own + int(fields[13]) + int(fields[14]), int(fields[17]))
        except (OSError, ValueError, IndexError):
            continue  # gone meanwhile
    return table


def link_output(proc: Path, pid: int) -> str:
    try:
        cmd = (proc / str(pid) / "cmdline").read_bytes().replace(b"\0", b" ").decode("utf-8", "replace")
    except OSError:
        return ""
    m = OUTPUT.search(cmd)
    return os.path.basename(m.group(1)) if m else ""


class Sampler:
    def __init__(self, path: Path, available=None, proc: Path = Path("/proc"), cgroup: Path = CGROUP, clock=time.time):
        self.path, self.available, self.proc, self.cgroup, self.clock = path, available, proc, cgroup, clock
        self.last = None
        self.last_sample = self.last_line = 0.0

    def tick(self):
        """A sample if one is due; returns the "[cpu]" line if one is due."""
        now = self.clock()
        if self.last is not None and now - self.last_sample < SAMPLE_S:
            return None
        self.last_sample = now
        sample = self.sample(now)
        if sample is None or now - self.last_line < LINE_S:
            return None
        self.last_line = now
        return self.line(sample)

    def sample(self, now):
        counters = machine_counters(self.proc)
        table = process_table(self.proc)
        used, source = cgroup_seconds(self.cgroup), "cgroup"
        if used is None:
            source = "tree"
        current = {"t": now, "counters": counters, "table": table, "used": used,
                   "tree": sum(row[2] for row in table.values())}
        previous, self.last = self.last, current
        if previous is None:
            return None
        ticks = counters["total"] - previous["counters"]["total"]
        ncpu = counters["ncpu"] or os.cpu_count() or 1
        if ticks <= 0 or now <= previous["t"]:
            return None
        ticks_per_core = ticks / ncpu

        def cores(key):
            return (counters[key] - previous["counters"][key]) / ticks_per_core

        if used is not None and previous["used"] is not None:
            ours = max(0.0, used - previous["used"]) / (now - previous["t"])
        else:
            ours, source = max(0.0, current["tree"] - previous["tree"]) / ticks_per_core, "tree"
        top = max(table, default=None, key=lambda p: table[p][1] - previous["table"].get(p, ("", 0))[1])
        sample = {"t": round(now, 1), "ours": round(ours, 2), "src": source, "busy": round(ncpu - cores("idle") - cores("iowait"), 2),
                  "iowait": round(cores("iowait"), 2), "steal": round(cores("steal"), 2), "total": ncpu,
                  "avail": self.available or ncpu, "run": counters["running"],
                  "link": [[pid, link_output(self.proc, pid) or row[0]] for pid, row in sorted(table.items()) if row[0] in LINKERS]}
        if top is not None:
            sample["top"] = [table[top][0], round((table[top][1] - previous["table"].get(top, ("", 0))[1]) / ticks_per_core, 2), table[top][3]]
        with self.path.open("a", encoding="utf-8") as f:
            f.write(json.dumps(sample) + "\n")
        return sample

    @staticmethod
    def line(s: dict) -> str:
        top = s.get("top") or ["-", 0, 0]
        return ("[cpu] ours %.1f of %d cores (%s), machine busy %.1f, iowait %.1f, steal %.1f, runnable %d, top %s %.1f cores %d threads"
                % (s["ours"], s["avail"], s["src"], s["busy"], s["iowait"], s["steal"], s["run"], top[0], top[1], top[2]))


def read_jsonl(path: Path) -> list:
    out = []
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                out.append(json.loads(line))
            except ValueError:
                continue
    except OSError:
        pass
    return out


def _epoch(text):
    import calendar
    m = re.match(r"^(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d)", text or "")
    return calendar.timegm(tuple(int(g) for g in m.groups()) + (0, 0, 0)) if m else None


def phases(samples: list, events: list) -> list:
    """(name, kind, start, end) of what the build did. kind: link, or phase."""
    if not samples:
        return []
    last = samples[-1]["t"]
    out, starts = [], {}
    for e in events:
        t = _epoch(e.get("t"))
        if t is None or e.get("event") not in ("start", "end"):
            continue
        if e["event"] == "start":
            starts[e["phase"]] = t
        elif e["phase"] in starts:
            out.append((e["phase"], "phase", starts.pop(e["phase"]), t))
    # A phase with a start and no end was cut short: as far as the samples go.
    out += [(name, "phase", t, last) for name, t in starts.items()]
    seen = {}
    for s in samples:
        for pid, name in s.get("link") or []:
            seen.setdefault(pid, []).append((s["t"], name))
    links = [("link %s" % times[0][1], "link", times[0][0], times[-1][0])
             for times in seen.values() if len(times) >= LINK_MIN_SAMPLES]
    build = next((p for p in out if p[0] == "build"), None)
    marks = {}
    for e in events:
        if e.get("event") == "mark":
            marks.setdefault(e.get("mark"), []).append(_epoch(e.get("t")))
    if build:
        configured = (marks.get("configured") or [None])[0]
        inside = sorted(l[2] for l in links if build[2] <= l[2] <= build[3])
        compile_end = inside[0] if inside else build[3]
        if configured is not None:
            out.append(("configure", "phase", build[2], configured))
            out.append(("compile", "phase", configured, compile_end))
        rust = marks.get("rust-start")
        if rust:
            out.append(("rust", "phase", rust[0], (marks.get("rust-end") or [build[3]])[-1]))
    return sorted(out + links, key=lambda p: (p[2], p[3]))


def measure(phase, samples: list):
    """One row of the table, or None with no sample inside the phase."""
    name, kind, start, end = phase
    # A sample's time is the end of what it measured, and a link's first sample is the
    # link's already: the end belongs to what comes next.
    inside = [s for s in samples if start <= s["t"] < end]
    if not inside:
        return None
    n = len(inside)
    return {"name": name, "kind": kind, "seconds": end - start, "mean": sum(s["ours"] for s in inside) / n,
            "peak": max(s["ours"] for s in inside), "busy": sum(s["busy"] for s in inside) / n,
            "iowait": sum(s["iowait"] for s in inside) / n, "steal": sum(s["steal"] for s in inside) / n,
            "total": inside[-1]["total"], "avail": inside[-1]["avail"], "src": inside[-1]["src"]}


def warnings(rows: list) -> list:
    out = []
    for r in rows:
        minutes = r["seconds"] / 60
        if r["kind"] != "link" and r["seconds"] > LONG_S and r["mean"] < UNDERUSED_SHARE * r["avail"]:
            out.append("%s: %.0f min at a mean of %.1f cores, below %d%% of the %d available"
                       % (r["name"], minutes, r["mean"], UNDERUSED_SHARE * 100, r["avail"]))
        others = r["busy"] - r["mean"]
        if others >= max(OTHERS_CORES, OTHERS_SHARE * r["total"]):
            out.append("%s: the machine was busy on %.1f cores and we used %.1f: something else, the owner's PC, took %.1f"
                       % (r["name"], r["busy"], r["mean"], others))
    return out


def clock(seconds: float) -> str:
    return "%dh%02d" % (seconds // 3600, seconds % 3600 // 60) if seconds >= 3600 else "%dm%02d" % (seconds // 60, seconds % 60)


def render(rows: list) -> str:
    if not rows:
        return "**CPU:** no samples for this leg."
    lines = ["**CPU use, by phase** (%d cores on the machine, %d available, measured from %s)"
             % (rows[0]["total"], rows[0]["avail"], rows[0]["src"]), "",
             "| phase | time | mean cores | peak cores | machine busy | iowait | steal |", "|---|---|---|---|---|---|---|"]
    for r in rows:
        lines.append("| %s | %s | %.1f | %.1f | %.1f | %.1f | %.1f |"
                     % (r["name"], clock(r["seconds"]), r["mean"], r["peak"], r["busy"], r["iowait"], r["steal"]))
    found = warnings(rows)
    if found:
        lines += [""] + ["⚠️ CPU: %s" % w for w in found]
    return "\n".join(lines)


def report(samples_path: Path, events_path: Path) -> str:
    samples = read_jsonl(samples_path)
    rows = [measure(p, samples) for p in phases(samples, read_jsonl(events_path))]
    return render([r for r in rows if r])
