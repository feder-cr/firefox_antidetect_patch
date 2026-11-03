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
    9. the summary's lines on what enters a compile's key, and only that.

    python3 scripts/ci/test_cache_rules.py
"""
from __future__ import annotations

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


def entry(cache_id, key, ref=MAIN, created="2026-11-03T10:00:00Z"):
    return {"id": cache_id, "key": key, "ref": ref, "created_at": created}


def day(n):
    return "2026-11-%02dT10:00:00Z" % n


def cases() -> list:
    out = []
    t = c.NEW_OBJECTS_WORTH_SAVING
    out.append(("worth: the threshold is a real number of objects", 0 < t < 5500))
    out.append(("worth: at the threshold it saves", c.worth_saving(STATS % (t, 0))[0] is True))
    out.append(("worth: one below it does not", c.worth_saving(STATS % (t - 1, 0))[0] is False))
    out.append(("worth: the count is in what it says", str(t - 1) in c.worth_saving(STATS % (t - 1, 0))[1]))
    out.append(("worth: objects it failed to store do not count", c.new_objects(STATS % (t + 5, 10)) == t - 5))
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
