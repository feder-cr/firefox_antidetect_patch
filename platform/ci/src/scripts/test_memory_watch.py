#!/usr/bin/env python3
"""memory_watch.py against a fake /proc.

  1. meminfo is read in MB;
  2. processes come largest first, kernel threads (no command line) left out;
  3. the victim is the largest process that is not the runner: killing
     Runner.Worker is what loses the job, the failure this script exists for;
  4. a runner-only machine has no victim.

Run it directly: `python3 scripts/test_memory_watch.py`.
"""
from __future__ import annotations

import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import memory_watch as w  # noqa: E402


def fake_proc(root: Path, procs):
    (root / "meminfo").write_text("MemTotal:       16373248 kB\nMemAvailable:     512000 kB\n"
                                  "SwapTotal:      16776188 kB\nSwapFree:       16776188 kB\n")
    for pid, rss_kb, cmd in procs:
        d = root / str(pid)
        d.mkdir()
        (d / "status").write_text("Name:\tx\nVmRSS:\t%d kB\n" % rss_kb if rss_kb is not None else "Name:\tx\n")
        (d / "cmdline").write_bytes(cmd.replace(" ", "\0").encode())


def main() -> int:
    cases = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fake_proc(root, [
            (10, 9_000_000, "/home/runner/actions-runner/bin/Runner.Worker spawnclient 1 2"),
            (20, 6_000_000, "/home/runner/.rustup/toolchains/1.95.0/bin/rustc --crate-name gkrust"),
            (30, 2_000_000, "clang-cl -c Unified_cpp_dom0.cpp"),
            (40, None, ""),
        ])
        m = w.meminfo(root)
        procs = w.processes(root)
        cases.append(("meminfo in MB", m["MemAvailable"] == 500 and m["MemTotal"] == 15989))
        # Run 40500378511: 600 MB of memory left, 16 GB of swap free, and the
        # first version killed the libxul link that swap would have carried.
        cases.append(("free swap counts: 500 MB of memory with 16 GB of swap free is not starving",
                      w.left_mb(m) >= w.KILL_MB))
        cases.append(("memory and swap both gone is starving",
                      w.left_mb({"MemAvailable": 300, "SwapFree": 200}) < w.KILL_MB))
        cases.append(("largest first, kernel threads out", [p for _, p, _ in procs] == [10, 20, 30]))
        hit = w.victim(procs, me=99)
        cases.append(("the runner is spared, the largest other process is the victim",
                      hit is not None and hit[1] == 20))
        cases.append(("itself is spared", w.victim([(5, 99, "python3 x")], me=99) is None))
        cases.append(("a runner-only machine has no victim",
                      w.victim([p for p in procs if p[1] == 10], me=99) is None))

    failures = 0
    for name, ok in cases:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
