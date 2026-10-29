"""Every leg's events, in one table: the run's summary.

    python3 scripts/ci report <dir with one events-<leg>/ folder per leg>

Each leg uploads events.jsonl, memwatch.log and sccache-stats.txt as
`events-<leg>`; the report job downloads them all and writes, to the run's
summary, one row per leg and phase: result, time, the leg's memory peak
(memory + swap in use), and how much of the compile came from the cache.
A leg with no events at all is a leg that never reached its first phase,
and says so instead of disappearing from the table.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

from legs import LEGS
from phase import clock

MEM = re.compile(r"\[mem\] available (\d+)/(\d+) MB, swap used (\d+)/")
HITS = re.compile(r"^Cache hits rate\s+([\d.]+) %", re.M)


def peak_mb(memwatch: str) -> int:
    peak = 0
    for m in MEM.finditer(memwatch):
        avail, total, swap = (int(x) for x in m.groups())
        peak = max(peak, total - avail + swap)
    return peak


def hit_rate(stats: str) -> str:
    m = HITS.search(stats)
    return "%s %%" % m.group(1) if m else "-"


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def rows(root: Path) -> list:
    out = []
    for leg in LEGS:
        d = root / ("events-%s" % leg.leg)
        events = [json.loads(l) for l in _read(d / "events.jsonl").splitlines() if l.strip()]
        peak = peak_mb(_read(d / "memwatch.log"))
        hits = hit_rate(_read(d / "sccache-stats.txt"))
        ends = {e["phase"]: e for e in events if e["event"] == "end"}
        starts = [e["phase"] for e in events if e["event"] == "start"]
        if not starts:
            out.append((leg.leg, "-", "⚪ never started", "-", "-", "-"))
            continue
        for phase in starts:
            end = ends.get(phase)
            if end is None:
                result, took = "⏹️ did not end (cancelled or runner lost)", "-"
            elif end["exit"] == 0:
                result, took = "✅", clock(end["seconds"])
            else:
                why = (end.get("killed") or end.get("errors") or ["exit %d" % end["exit"]])[0]
                result, took = "❌ %s" % why[:120].replace("|", "\\|"), clock(end["seconds"])
            out.append((leg.leg, phase, result, took,
                        "%.1f GB" % (peak / 1024) if peak else "-", hits))
    return out


def render(root: Path) -> str:
    lines = ["## Build run", "",
             "| leg | phase | result | time | memory peak | compile cache hits |",
             "|---|---|---|---|---|---|"]
    lines += ["| %s |" % " | ".join(r) for r in rows(root)]
    return "\n".join(lines) + "\n"
