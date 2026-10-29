#!/usr/bin/env python3
"""The compile cache's rules in release.yml, read from the file itself.

  1. the save step runs only when the Build step started: run 40519477694,
     cancelled during checkout, saved an empty arm64 entry that the next run
     would have restored as the newest of its leg, and compiled cold;
  2. it still runs when the build failed or was cancelled mid-way (the runs
     that fix a port fail by design, and they are the ones the cache is for);
  3. the size per leg times three legs fits the repository's 150 GB, and is
     at least 30 GB: a PGO + LTO build writes more than 15, and a cache smaller
     than a build evicts itself (run 40557157874 reused 0.2%);
  4. at least 32 GB of swap: the Windows leg's full-LTO link used 24.4 of 24
     (run 40520853142).

Run it directly: `python3 scripts/test_workflow_cache_rules.py`.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

WORKFLOW = Path(__file__).resolve().parent.parent / ".github" / "workflows" / "release.yml"
REPO_LIMIT_GB = 150
LEGS = 3


def step(text: str, name: str) -> str:
    """The text of the step called `name`, up to the next step."""
    start = text.index("      - name: " + name + "\n")
    nxt = text.find("\n      - name: ", start + 1)
    return text[start:nxt if nxt > 0 else len(text)]


def main() -> int:
    text = WORKFLOW.read_text(encoding="utf-8").replace("\r\n", "\n")
    build = step(text, "Build")
    save = step(text, "Compile cache (candidates only) - save this leg's")
    cond = re.search(r"^\s+if: (.*)$", save, re.M).group(1)
    size = int(re.search(r"SCCACHE_CACHE_SIZE=(\d+)G", text).group(1))
    swap = int(re.search(r"fallocate -l (\d+)G /swapfile", text).group(1))
    cases = [
        ("the Build step has id build", re.search(r"^\s+id: build$", build, re.M) is not None),
        ("the save waits for the Build step to have started",
         "steps.build.outcome" in cond and "skipped" not in cond),
        ("the save still runs after a failed or cancelled build",
         "always()" in cond and "'failure'" in cond and "'cancelled'" in cond),
        ("%d legs of %d GB fit the repository's %d GB" % (LEGS, size, REPO_LIMIT_GB),
         LEGS * size <= REPO_LIMIT_GB),
        ("the cache per leg (%d GB) holds a PGO + LTO build" % size, size >= 30),
        ("the swap (%d GB) holds the Windows full-LTO link" % swap, swap >= 32),
    ]
    failures = 0
    for name, ok in cases:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
