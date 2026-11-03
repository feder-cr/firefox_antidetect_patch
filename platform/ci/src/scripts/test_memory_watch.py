#!/usr/bin/env python3
"""memory_watch.py against a fake /proc.

  1. meminfo is read in MB;
  2. processes come largest first, kernel threads (no command line) left out;
  3. the victim is the largest process that is not the runner: killing
     Runner.Worker is what loses the job, the failure this script exists for;
  4. a runner-only machine has no victim;
  5. a container's limits win over the host's meminfo (the Theta node's measured
     numbers): available and free swap are the cgroup's, page cache is not used;
  6. no limit, or a limit above the host: exactly meminfo, as on GitHub's runners;
  7. the thresholds are the measured ones on 16 GB and in proportion above it.

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


def huge_page_cases() -> list:
    out = []
    with tempfile.TemporaryDirectory() as tmp:
        proc, cg = Path(tmp) / "proc", Path(tmp) / "cg"
        proc.mkdir()
        cg.mkdir()
        (proc / "meminfo").write_text("MemTotal: 1024 kB\nAnonHugePages: 2097152 kB\n")
        out.append(("huge pages: the machine's where the container has no memory.stat", w.huge_pages_mb(proc, cg) == 2048))
        (cg / "memory.stat").write_text("anon 1\nanon_thp %d\n" % (512 * 2**20))
        out.append(("huge pages: the container's where it has one", w.huge_pages_mb(proc, cg) == 512))
        (proc / "meminfo").unlink()
        (cg / "memory.stat").unlink()
        out.append(("huge pages: nothing to read says nothing", w.huge_pages_mb(proc, cg) is None))
    return out


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

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        proc, cg = root / "proc", root / "cg"
        proc.mkdir()
        cg.mkdir()
        # A host with more memory and swap than the container may use.
        (proc / "meminfo").write_text("MemTotal: 131072000 kB\nMemAvailable: 120000000 kB\n"
                                      "SwapTotal: 8388608 kB\nSwapFree: 8388608 kB\n")
        (cg / "memory.max").write_text("67177832448\n")
        (cg / "memory.current").write_text("%d\n" % (40 * 2**30))
        # 40 GB in memory.current: 30 of anon, 10 of page cache.
        (cg / "memory.stat").write_text("anon %d\nfile %d\nactive_file %d\ninactive_file %d\n"
                                        % (30 * 2**30, 10 * 2**30, 7 * 2**30, 3 * 2**30))
        (cg / "memory.swap.max").write_text("0\n")
        (cg / "memory.swap.current").write_text("0\n")
        (cg / "cpu.max").write_text("4800000 100000\n")
        got = w.memory(proc, cg)
        cases.append(("the container's limit is the total, as measured on the node",
                      got["MemTotal"] == 67177832448 // 2**20))
        cases.append(("page cache the kernel gives back is available, active as well as inactive (40 GB used, 10 of it file)",
                      got["MemAvailable"] == (67177832448 - 30 * 2**30) // 2**20))
        # The false kill: a tree read by a compile is active page cache, and the limit
        # less anything but what cannot be reclaimed starves with memory to give back.
        (cg / "memory.current").write_text("%d\n" % 67177832448)
        (cg / "memory.stat").write_text("anon %d\nfile %d\nactive_file %d\ninactive_file %d\n"
                                        % (20 * 2**30, 42 * 2**30, 41 * 2**30, 2**30))
        cases.append(("a cgroup full of active page cache is not starving: no kill",
                      w.left_mb(w.memory(proc, cg)) > w.thresholds(64065)[1]))
        (cg / "memory.stat").write_text("anon %d\nshmem %d\nkernel %d\nslab_reclaimable %d\nslab_unreclaimable %d\nfile %d\n"
                                        % (2**30, 2 * 2**30, 8 * 2**30, 3 * 2**30, 2 * 2**30, 5 * 2**30))
        cases.append(("what cannot be reclaimed: anon + shmem + kernel, with the reclaimable slab off and the rest of it not counted twice",
                      w.unreclaimable(cg) == (1 + 2 + 8 - 3) * 2**30))
        (cg / "memory.stat").write_text("anon %d\nshmem %d\nkernel_stack %d\npagetables %d\npercpu %d\nslab_reclaimable %d\nslab_unreclaimable %d\n"
                                        % (2**30, 2 * 2**30, 2**20, 2**20, 2**20, 3 * 2**30, 2 * 2**30))
        cases.append(("an older kernel with no `kernel` line: its parts, and unreclaimable slab only",
                      w.unreclaimable(cg) == (1 + 2 + 2) * 2**30 + 3 * 2**20))
        (cg / "memory.stat").write_text("inactive_file 7\n")
        cases.append(("a memory.stat that says nothing of anon is not read as zero used", w.unreclaimable(cg) is None))
        (cg / "memory.current").write_text("%d\n" % (40 * 2**30))
        cases.append(("no swap for the container, whatever the host has",
                      got["SwapFree"] == 0 and got["SwapTotal"] == 0))
        (cg / "memory.current").write_text("%d\n" % 67177832448)
        (cg / "memory.stat").write_text("anon %d\n" % 67177832448)
        cases.append(("a container at its limit starves, with the host's memory free",
                      w.left_mb(w.memory(proc, cg)) < w.thresholds(64065)[1]))
        cases.append(("cpu.max is the CPU limit", w.cpu_limit(cg) == min(48, __import__("os").cpu_count() or 1)))
        (cg / "memory.max").write_text("max\n")
        cases.append(("memory.max = max: exactly meminfo", w.memory(proc, cg) == w.meminfo(proc)))
        (cg / "memory.max").write_text("%d\n" % (2**40))
        cases.append(("a limit above the host: exactly meminfo", w.memory(proc, cg) == w.meminfo(proc)))
        cases.append(("no cgroup at all: exactly meminfo", w.memory(proc, root / "none") == w.meminfo(proc)))
        cases.append(("a GitHub runner's thresholds are the measured ones",
                      w.thresholds(15989) == (3000, 600)))
        cases.append(("the node's thresholds keep their proportion", w.thresholds(64065) == (12012, 2402)))

    cases += huge_page_cases()
    failures = 0
    for name, ok in cases:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
