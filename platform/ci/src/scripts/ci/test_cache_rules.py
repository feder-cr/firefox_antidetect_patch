#!/usr/bin/env python3
"""cache_rules.py against the cases it exists for.

  worth
    1. at the threshold it saves, one below it does not, and the count is said;
    2. statistics with no count save: a build's objects are worth more than an upload;
    3. objects sccache failed to store do not count;
  trim
    4. this leg and this ref keep their newest 2 and lose the rest;
    5. never another ref, never another leg, never the sccache binary's entries;
    6. nothing at all when this run saved nothing for that leg;
    7. the API's own filter is not trusted: what it returns is checked again;
    8. a failed listing is a warning, never a failed run;
  guard
    9. the summary's lines on what enters a compile's key, and only that;
  worth, on the disk
    10. a server that restarted: few misses counted, a cold build's objects on disk: saves;
    11. either witness saves; nothing grown and few misses does not;
    12. next to nothing in the directory never saves, whatever the count says;
    13. nothing to compare with saves, if the directory is not next to empty;
  heat
    14. cold or warm from the hits, real statistics included; too few counts say so;
  trim
    15. small newer entries do not push a good one out.

    python3 scripts/ci/test_cache_rules.py
"""
from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import cache_rules as c  # noqa: E402

STATS = """Compile requests                   5923
Compile requests executed          5610
Cache hits                         5000
Cache hits (C/C++)                 4000
Cache misses                       %d
Cache misses (C/C++)               1
Cache timeouts                        0
Cache write errors                    %d
"""
MAIN = "refs/heads/main"
# The statistics of the cold build on the node, 03/11.
REAL_COLD = """Compile requests                       0
Compile requests executed           5850
Cache hits                             3
Cache hits (C/C++)                     3
Cache misses                        5820
Cache misses (C/C++)                4609
Cache hits rate                     0.05 %
Cache write errors                     0
"""


def entry(cache_id, key, ref=MAIN, created="2026-11-03T10:00:00Z"):
    return {"id": cache_id, "key": key, "ref": ref, "created_at": created}


def day(n):
    return "2026-11-%02dT10:00:00Z" % n


def disk(names, size=1):
    return {"a/b/%d" % n: size for n in names}


def cases() -> list:
    out = []
    t = c.NEW_OBJECTS_WORTH_SAVING
    floor = c.ENTRY_OBJECTS_FLOOR
    restarted = STATS % (3, 0)
    good = disk(range(5000))
    good_names = set(good)
    more = dict(good, **disk(range(5000, 5000 + t)))
    out.append(("worth: after a restart, a handful counted and the objects on disk: saves",
                c.worth_saving(restarted, now=more, restored=good_names)[0] is True))
    out.append(("worth: a cold build with stats near zero and all its objects new: saves",
                c.worth_saving(restarted, now=good, restored=set())[0] is True))
    out.append(("worth: new objects counted by name, so an eviction beside them does not hide them",
                c.worth_saving(restarted, now=disk(range(t, 5000 + t)), restored=good_names)[0] is True))
    out.append(("worth: few new objects but over a GB of them: saves",
                c.worth_saving(restarted, now=dict(good, **{"r/ust/gkrust": c.NEW_BYTES_WORTH_SAVING}), restored=good_names)[0] is True))
    out.append(("worth: the count alone still saves, whatever the disk says", c.worth_saving(STATS % (t, 0), now=good, restored=good_names)[0] is True))
    small = c.worth_saving(STATS % (t - 1, 0), now=dict(good, **disk(range(5000, 5000 + t - 1))), restored=good_names)
    out.append(("worth: neither witness: does not save, and says both numbers", small[0] is False and str(t - 1) in small[1]))
    out.append(("worth: next to nothing never saves on any leg, whatever the count says",
                c.worth_saving(STATS % (5000, 0), now=disk(range(floor - 1)), restored=set())[0] is False
                and c.worth_saving("", now={}, restored=None)[0] is False))
    out.append(("worth: at the floor it may", c.worth_saving(STATS % (t, 0), now=disk(range(floor)), restored=set())[0] is True))
    out.append(("worth: the disk with nothing to compare it with saves", c.worth_saving(restarted, now=good, restored=None)[0] is True))
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp) / "sccache"
        for name in ("a/b/one", "preprocessor/a/two", ".stats"):
            (d / name).parent.mkdir(parents=True, exist_ok=True)
            (d / name).write_text("x", encoding="utf-8")
        out.append(("worth: objects are what it counts, not the preprocessor cache or its own files", list(c.objects_on_disk(d)) == ["a/b/one"]))
        out.append(("worth: a directory that is not there holds nothing", c.objects_on_disk(Path(tmp) / "none") == {}))
        snap = Path(tmp) / "snap.json"
        stats = Path(tmp) / "stats.txt"
        stats.write_text(restarted, encoding="utf-8")
        c.main(["snapshot", "--dir", str(d), "--out", str(snap)])
        for n in range(floor + t):
            (d / "c" / str(n)).parent.mkdir(exist_ok=True)
            (d / "c" / str(n)).write_text("x", encoding="utf-8")
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed), contextlib.redirect_stderr(io.StringIO()):
            c.main(["worth", "--stats", str(stats), "--dir", str(d), "--restored", str(snap)])
        out.append(("worth: the commands end to end: snapshot after the restore, save=true from the disk", printed.getvalue().strip() == "save=true"))
    real = REAL_COLD
    out.append(("heat: the cold build of 03/11, 3 hits of 5823, reads COLD with its count",
                "COLD" in c.heat(real, "linux-x86_64") and "3 hits of 5823" in c.heat(real, "linux-x86_64")))
    warm = c.heat("Cache hits 5000\nCache misses 610\n", "win-x86_64")
    out.append(("heat: most of them hit reads WARM", "WARM" in warm and "win-x86_64" in warm))
    out.append(("heat: a restarted server's near zero says it cannot tell, not cold",
                "cannot be said" in c.heat("Cache hits 3\nCache misses 7\n", "x") and "COLD" not in c.heat("Cache hits 3\nCache misses 7\n", "x")))
    out.append(("heat: no statistics say so", "cannot be said" in c.heat("", "x")))
    sized = [dict(entry(1, "sccache-l-100-1", created=day(1)), size_in_bytes=5 * 10**9),
             dict(entry(2, "sccache-l-200-1", created=day(2)), size_in_bytes=4 * 10**9),
             dict(entry(3, "sccache-l-300-1", created=day(3)), size_in_bytes=10**6),
             dict(entry(4, "sccache-l-400-1", created=day(4)), size_in_bytes=10**6)]
    out.append(("trim: two small newer entries do not push the good one out, and the older of its size goes",
                c.to_delete(sized, MAIN, "l", 400, 1) == [2]))
    even = [dict(e, size_in_bytes=5 * 10**9) for e in sized]
    out.append(("trim: entries of a like size keep the newest 2 and no more", sorted(c.to_delete(even, MAIN, "l", 400, 1)) == [1, 2]))
    out.append(("worth: the threshold is a real number of objects", 0 < t < 5500))
    out.append(("worth: at the threshold it saves", c.worth_saving(STATS % (t, 0))[0] is True))
    out.append(("worth: a set that misses on every run is saved, however small (the Windows leg's 284, the Linux leg's 60)",
                all(c.worth_saving(STATS % (n, 0))[0] is True for n in (284, 60, 1))
                and c.worth_saving(STATS % (284, 0), now=disk(range(5000)), restored=set(disk(range(5000 - 284))))[0] is True))
    out.append(("worth: one below it does not", c.worth_saving(STATS % (t - 1, 0))[0] is False))
    out.append(("worth: the count is in what it says", str(t - 1) in c.worth_saving(STATS % (t - 1, 0))[1]))
    out.append(("worth: objects it failed to store do not count", c.new_objects(STATS % (305, 10)) == 295))
    out.append(("worth: the total, not the C/C++ line", c.new_objects(STATS % (7, 0)) == 7))
    out.append(("worth: statistics with no count save", c.worth_saving("")[0] is True and c.worth_saving("garbage\n")[0] is True))

    leg = "linux-x86_64"
    mine = [entry(1, "sccache-%s-100-1" % leg, created=day(1)), entry(2, "sccache-%s-200-1" % leg, created=day(2)),
            entry(3, "sccache-%s-300-1" % leg, created=day(3)), entry(4, "sccache-%s-400-1" % leg, created=day(4))]
    others = [entry(10, "sccache-%s-100-1" % leg, ref="refs/heads/other", created=day(1)),
              entry(11, "sccache-win-x86_64-100-1", created=day(1)),
              entry(12, "sccache-bin-Linux-X64-v0.18.0-abc", created=day(1)),
              entry(13, "lavapipe-%s-abc" % leg, created=day(1)),
              entry(14, "sccache-%s-node-1" % leg, created=day(1))]
    out.append(("trim: this leg and ref keep the newest 2, whatever order they come in",
                sorted(c.to_delete(list(reversed(mine)) + others, MAIN, leg, 400, 1)) == [1, 2]))
    out.append(("trim: never another ref, another leg or another kind of entry",
                not set(c.to_delete(mine + others, MAIN, leg, 400, 1)) & {10, 11, 12, 13, 14}))
    out.append(("trim: nothing when this run saved nothing for the leg", c.to_delete(mine + others, MAIN, leg, 999, 1) == []))
    out.append(("trim: this run's entry on another ref is not this run's save",
                c.to_delete(mine + [entry(20, "sccache-%s-999-1" % leg, ref="refs/heads/other")], MAIN, leg, 999, 1) == []))
    out.append(("trim: two entries are left alone", c.to_delete(mine[2:], MAIN, leg, 400, 1) == []))

    calls = []

    def transport(method, url, headers):
        calls.append((method, url))
        if method == "DELETE":
            return 204, b""
        # An API that ignores its filters: the check is ours.
        is_linux = "sccache-linux-x86_64-" in url
        return 200, json.dumps({"actions_caches": mine + others if is_linux else others}).encode()
    deleted = c.trim("o/r", "T", MAIN, 400, 1, transport, out=lambda _: None)
    out.append(("trim: end to end, only the two old entries of the leg this run saved are deleted",
                sorted(deleted) == [1, 2] and sum(m == "DELETE" for m, _ in calls) == 2))
    out.append(("trim: it asks for this ref by name", all("ref=refs%2Fheads%2Fmain" in u for m, u in calls if m == "GET")))

    def refused(method, url, headers):
        return 500, b"{}"
    try:
        c.trim("o/r", "T", MAIN, 400, 1, refused, out=lambda _: None)
        raised = False
    except RuntimeError:
        raised = True
    out.append(("trim: a failed listing deletes nothing and is reported", raised))

    with tempfile.TemporaryDirectory() as tmp:
        home = Path(tmp) / "home"
        clang, driver = home / ".mozbuild" / "clang" / "bin" / "clang", Path(tmp) / "rust" / "lib" / "librustc_driver-abc.so"
        tarball = home / ".mozbuild" / "sysroot-x86_64-linux-gnu.tar.zst"
        tree = home / ".mozbuild" / "sysroot-x86_64-linux-gnu" / "usr" / "include" / "a.h"
        for path, text in ((clang, "clang"), (driver, "driver"), (tarball, "tar"), (tree, "h")):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        env = {"CARGO_HOME": "/home/runner/.cargo", "RUSTUP_HOME": "x", "SCCACHE_CACHE_SIZE": "30G", "CPATH": "y", "GITHUB_RUN_ID": "1", "SCCACHE_LOG": "debug"}
        lines = c.key_guard(home, Path("/home/runner/work/a/a"), env, str(driver.parent.parent))
        said = "\n".join(lines)
        out.append(("guard: the working directory, HOME, and a line for each compiler's file",
                    "cwd: /home/runner/work/a/a" in said and "HOME: %s" % home in said
                    and "clang: %s" % c.sha256_of(clang)[:16] in said and "librustc_driver: %s" % c.sha256_of(driver)[:16] in said
                    and "sysroot tarball: %s" % c.sha256_of(tarball)[:16] in said and "sysroot tree: absent" not in said))
        moved = c.key_guard(home, Path("/x"), dict(env, CARGO_TARGET_DIR="z"), "")
        same = c.key_guard(home, Path("/x"), dict(env, GITHUB_RUN_ID="2", SCCACHE_LOG="info"), "")
        out.append(("guard: the environment's hash moves with a CARGO_* variable, not with one that does not enter the key",
                    moved[2] != lines[2] and same[2] == lines[2]))
        out.append(("guard: what is not there says so, and nothing raises", "librustc_driver: absent" in moved[4]
                    and "clang: absent" in "\n".join(c.key_guard(Path(tmp) / "nobody", Path("/x"), {}, ""))))
    return out


def main() -> int:
    got = cases()
    failures = 0
    for name, ok in got:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(got) - failures, len(got)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
