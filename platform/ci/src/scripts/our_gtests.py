#!/usr/bin/env python3
"""The engine's own gtests, read from the series: every TEST, TEST_F and TEST_P that the
series adds to Mozilla's tree (a file it adds whole, from its MANIFEST.json and src/, and
the TEST lines its hooks.patch adds to a Mozilla file).

The series is the source of the list, never a `git diff` against Mozilla's tag: the tree a
job builds is made by `engine.py materialize`, which may borrow its objects from another
checkout and has no ref for the tag. A tree that is read through git can be unreadable; the
series is files, read the same everywhere, with or without git.

Two places need that list and read it from here, never from a copy of their own:
  - scripts/test_release_plan.py: every one is named by the default gtest filter;
  - scripts/ci/gtest_run.py: every one the filter selects was run, in the reports.

    python3 scripts/our_gtests.py [--series DIR] [--out FILE]

prints (or writes) one `Suite.Test` per line, a TEST_P as `X/Suite.Test/0`: the form
gtest_matches() takes. The build job writes it beside the binaries of the gtest jobs,
which have no source tree (gtest-run/our-gtests.txt); gtest_run.py reads it there.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

#: The series to read the gtests from. Default: the `.series` beside the tree the jobs
#: check out (see release.yml), else the series this script is in (the script is at
#: platform/ci/src/scripts/ in it).
SERIES_ROOT_ENV = "ENGINE_SERIES_ROOT"
GTEST_DEFINITION = re.compile(r"^\s*(TEST|TEST_F|TEST_P)\(\s*(\w+)\s*,\s*(\w+)\s*\)", re.MULTILINE)
SOURCE_SUFFIXES = (".cpp", ".cc", ".mm")
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


def is_series(path: Path) -> bool:
    return (path / "SERIES").is_file() and (path / "UPSTREAM.json").is_file()


def series_root() -> Path:
    """The series to read: ENGINE_SERIES_ROOT, else ROOT/.series, else the one this script is
    in. Raises OSError, saying what to set, when there is none."""
    if os.environ.get(SERIES_ROOT_ENV):
        return Path(os.environ[SERIES_ROOT_ENV])
    for candidate in [ROOT / ".series", *Path(__file__).resolve().parents]:
        if is_series(candidate):
            return candidate
    raise OSError("no series to read the gtests from (%s/.series, or a checkout of the series this script is in): set %s"
                  % (ROOT, SERIES_ROOT_ENV))


def series_folders(series: Path) -> list:
    """The folders of SERIES, in order: one line each, comments and blanks aside."""
    lines = (series / "SERIES").read_text(encoding="utf-8").splitlines()
    return [l.strip() for l in lines if l.strip() and not l.lstrip().startswith("#")]


def tests_in(text: str) -> list:
    """(`Suite.Test`, macro) for every definition in the text; a TEST_P runs as
    `Instantiation/Suite.Test/N`, any instantiation: the filter must name that."""
    found = []
    for m in GTEST_DEFINITION.finditer(text):
        full = "X/%s.%s/0" % (m.group(2), m.group(3)) if m.group(1) == "TEST_P" else "%s.%s" % (m.group(2), m.group(3))
        found.append((full, m.group(1)))
    return found


def added_lines(patch: str) -> dict:
    """The lines a unified diff adds, by file (the b/ side), for the C++ sources only."""
    added, name = {}, None
    for line in patch.splitlines():
        if line.startswith("+++ "):
            name = line[6:] if line.startswith("+++ b/") else None
            if name is not None and not name.endswith(SOURCE_SUFFIXES):
                name = None
        elif name and line.startswith("+"):
            added.setdefault(name, []).append(line[1:])
    return added


def our_gtests(series: Path) -> list:
    """(file, `Suite.Test`, macro) for every test the series adds to Mozilla's tree: the
    files of each folder's MANIFEST.json "added" (stored under the folder's src/), and the
    lines of its hooks.patch. Raises OSError with what to fix."""
    if not is_series(series):
        raise OSError("%s is not a series (no SERIES and UPSTREAM.json there): set %s" % (series, SERIES_ROOT_ENV))
    found = []
    for folder in series_folders(series):
        where = series / folder
        try:
            manifest = json.loads((where / "MANIFEST.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise OSError("%s/MANIFEST.json of the series cannot be read: %s" % (folder, exc)) from exc
        for path in sorted(manifest.get("added", {})):
            if not path.endswith(SOURCE_SUFFIXES):
                continue
            stored = where / "src" / path
            try:
                text = stored.read_text(encoding="utf-8", errors="replace")
            except OSError as exc:
                raise OSError("%s is in the MANIFEST.json of %s and is not stored under its src/: %s" % (path, folder, exc)) from exc
            found += [(path, n, m) for n, m in tests_in(text)]
        patch = where / "hooks.patch"
        if patch.is_file():
            hooks = added_lines(patch.read_text(encoding="utf-8", errors="replace"))
            for path, lines in sorted(hooks.items()):
                found += [(path, n, m) for n, m in tests_in(chr(10).join(lines))]
    return found


def our_test_names(series: Path | None = None) -> list:
    """The `Suite.Test` of every gtest of ours. Raises OSError, saying what to fix, when the
    series cannot be read or has none of ours: a list that was not read is not an empty one."""
    series = series or series_root()
    found = our_gtests(series)
    if not found:
        raise OSError("no TEST/TEST_F/TEST_P of ours in %s (wrong series?)" % series)
    return [n for _, n, _ in found]


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--series", type=Path, default=None, help="the series (default: ENGINE_SERIES_ROOT, else .series, else this one)")
    ap.add_argument("--out", type=Path, default=None, help="write here instead of stdout")
    args = ap.parse_args(argv)
    try:
        names = our_test_names(args.series.resolve() if args.series else None)
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
