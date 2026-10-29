"""Who touches a build input while the build runs.

    python3 scripts/ci input-watch --file F --log L      (in the background)
    python3 scripts/ci input-watch --file F --log L --verify

The first form samples F every second and, the first time its mtime or size
moves, writes the moment and the processes then running to L. The second,
after the build, refuses an F whose CONTENT changed since the watch began: the
profile is the retail's, verified against its chain of trust before the build,
and a build that read another one is not the build that was verified.

Why it exists: in run 40619608072 cargo rebuilt gkrust in the symbols step
because ~/pgo/merged.profdata had an mtime of 02:41:00, eighteen minutes into
the Build step, while the CI wrote it at 02:23:05 and nothing in the tree
writes it. The relinked xul is why the symbols of run 40593659072 described
another binary than the one shipped.
"""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path


def state(path: Path) -> tuple:
    st = path.stat()
    return st.st_mtime_ns, st.st_size


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def processes() -> str:
    cmd = (["tasklist", "/fo", "csv"] if sys.platform == "win32"
           else ["ps", "-eo", "pid,ppid,etimes,args", "--sort=etimes"])
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
    except Exception as exc:  # the record matters more than the listing
        return "processes unavailable: %r" % exc


def first_change(before: tuple, now: tuple) -> bool:
    return now != before


def watch(path: Path, log: Path, interval: float = 1.0, stop=lambda: False) -> None:
    begin = state(path)
    (log.with_suffix(".start.json")).write_text(json.dumps(
        {"mtime_ns": begin[0], "size": begin[1], "sha256": digest(path)}), encoding="utf-8")
    while not stop():
        time.sleep(interval)
        now = state(path)
        if first_change(begin, now):
            when = time.strftime("%H:%M:%S", time.gmtime())
            running = processes()
            with log.open("a", encoding="utf-8") as f:
                f.write("%s %s changed: mtime %d -> %d, size %d -> %d\n%s\n" % (
                    when, path, begin[0], now[0], begin[1], now[1], running))
            return


def verify(path: Path, log: Path, strict: bool = False) -> str:
    """`strict` refuses a moved mtime too: with the patched sccache nothing
    writes the profile, and a write is what made cargo rebuild."""
    record = log.with_suffix(".start.json")
    if not record.is_file():
        raise ValueError("the watch of %s never recorded its start: nothing to verify against" % path)
    start = json.loads(record.read_text(encoding="utf-8"))
    if digest(path) != start["sha256"]:
        raise ValueError("%s changed CONTENT during the build: the build did not read the profile "
                         "that was verified" % path)
    moved = state(path)[0] != start["mtime_ns"]
    if moved and strict:
        raise ValueError("%s was written during the build (same content, new mtime): cargo will "
                         "rebuild what read it. Who wrote it is in %s" % (path, log))
    return "%s unchanged%s" % (path, " (its mtime moved: see %s)" % log if moved else "")
