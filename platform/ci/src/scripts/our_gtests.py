#!/usr/bin/env python3
"""The engine's own gtests, read from the source: every TEST, TEST_F and TEST_P that the
series adds to Mozilla's tree (a file we add whole, and the TEST lines we add to a Mozilla
file), by `git diff` against the tag of the Firefox it is applied to.

Two places need that list and read it from here, never from a copy of their own:
  - scripts/test_release_plan.py: every one is named by the default gtest filter;
  - scripts/ci/gtest_run.py: every one the filter selects was run, in the reports.

    python3 scripts/our_gtests.py --tree DIR [--base REF] [--out FILE]

prints (or writes) one `Suite.Test` per line, a TEST_P as `X/Suite.Test/0`: the form
gtest_matches() takes. The build job writes it beside the binaries of the gtest jobs,
which have no source tree (gtest-run/our-gtests.txt); gtest_run.py reads it there.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

#: The tree to read the gtests from (default: the tree this script is in) and the
#: git ref that is Mozilla's, to tell what is ours (default: the tag that
#: browser/config/upstream_source.txt names, FIREFOX_<version>_RELEASE).
GTEST_ROOT_ENV, GTEST_BASE_ENV = "ENGINE_GTEST_ROOT", "ENGINE_GTEST_BASE"
GTEST_DEFINITION = re.compile(r"^\s*(TEST|TEST_F|TEST_P)\(\s*(\w+)\s*,\s*(\w+)\s*\)", re.MULTILINE)
ROOT = Path(__file__).resolve().parent.parent


def gtest_matches(gtest_filter: str, name: str) -> bool:
    """gtest's own rule: `positive[-negative]`, patterns split by `:`, * any run, ? one
    character, the whole of `Suite.Test` matched; an empty positive part is `*`."""
    positive, _, negative = gtest_filter.partition("-")

    def one(pattern: str) -> bool:
        regex = "".join(".*" if c == "*" else "." if c == "?" else re.escape(c) for c in pattern)
        return re.fullmatch(regex, name) is not None

    return any(one(q) for q in (positive or "*").split(":")) and not any(one(q) for q in negative.split(":") if q)


def suite_of(name: str) -> str:
    """`Suite` of `Suite.Test`, of `X/Suite.Test/0` (what a TEST_P runs as) and of the listing's `X/Suite`."""
    return name.split(".", 1)[0].rsplit("/", 1)[-1]


def base_ref(root: Path) -> str:
    if os.environ.get(GTEST_BASE_ENV):
        return os.environ[GTEST_BASE_ENV]
    text = (root / "browser" / "config" / "upstream_source.txt").read_text(encoding="utf-8")
    version = next(l.split("=", 1)[1].strip() for l in text.splitlines() if l.startswith("version="))
    return "FIREFOX_%s_RELEASE" % version.replace(".", "_")


def our_gtests(root: Path, base: str) -> list:
    """(file, `Suite.Test`, macro) for every test the tree adds to Mozilla's `base`: a file
    we add whole, and the TEST lines we add to a Mozilla file, as committed (the tree a
    candidate builds is a commit: `engine.py materialize`). Raises OSError with what to fix."""
    if not root.is_dir():
        raise OSError("the tree to read the gtests from, %s, does not exist (set %s)" % (root, GTEST_ROOT_ENV))
    git = ["git", "-c", "safe.directory=*", "-C", str(root)]
    try:
        done = subprocess.run(git + ["diff", "-U0", "--no-color", "--no-ext-diff", base, "HEAD", "--", "*.cpp", "*.cc", "*.mm"],
                              capture_output=True, text=True, encoding="utf-8", errors="replace", check=False)
    except FileNotFoundError as exc:
        raise OSError("git is not installed: our gtests are what differs from Mozilla's %s" % base) from exc
    if done.returncode:
        raise OSError("git diff %s in %s failed (not a git tree, or no such ref; set %s): %s"
                      % (base, root, GTEST_BASE_ENV, done.stderr.strip()[-300:]))
    added, found, name = {}, [], None
    for line in done.stdout.splitlines():
        if line.startswith("+++ "):
            name = line[6:] if line.startswith("+++ b/") else None
        elif name and line.startswith("+"):
            added.setdefault(name, []).append(line[1:])
    for name, lines in sorted(added.items()):
        for m in GTEST_DEFINITION.finditer(chr(10).join(lines)):
            # a TEST_P runs as `Instantiation/Suite.Test/N`, any instantiation: the filter must name that
            full = "X/%s.%s/0" % (m.group(2), m.group(3)) if m.group(1) == "TEST_P" else "%s.%s" % (m.group(2), m.group(3))
            found.append((name, full, m.group(1)))
    return found


def our_test_names(root: Path | None = None, base: str | None = None) -> list:
    """The `Suite.Test` of every gtest of ours. Raises OSError, saying what to fix, when the
    tree cannot be read or has none of ours: a list that was not read is not an empty one."""
    root = root or Path(os.environ.get(GTEST_ROOT_ENV) or ROOT)
    try:
        found = our_gtests(root, base or base_ref(root))
    except StopIteration as exc:
        raise OSError("no version= in %s/browser/config/upstream_source.txt (set %s)" % (root, GTEST_BASE_ENV)) from exc
    if not found:
        raise OSError("no TEST/TEST_F/TEST_P of ours in %s (wrong tree or base?)" % root)
    return [n for _, n, _ in found]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tree", type=Path, default=None, help="the source tree (default: ENGINE_GTEST_ROOT, else this one)")
    ap.add_argument("--base", default=None, help="Mozilla's ref (default: the tag of upstream_source.txt)")
    ap.add_argument("--out", type=Path, default=None, help="write here instead of stdout")
    args = ap.parse_args(argv)
    try:
        names = our_test_names(args.tree.resolve() if args.tree else None, args.base)
    except OSError as exc:
        print("ERROR: our gtests were NOT read: %s" % exc, file=sys.stderr)
        return 1
    text = "".join(n + "\n" for n in names)
    if args.out:
        args.out.write_text(text, encoding="utf-8", newline="\n")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
