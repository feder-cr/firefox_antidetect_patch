#!/usr/bin/env python3
"""check_upstream_source.py, driven against the files it exists to refuse.

  known-bad
    1. a file left on the previous release after the tree moved on - the drift
       a port makes by moving the base and forgetting the file;
    2. a missing value; 3. a value given twice; 4. an unknown key;
    5. a changeset that is not a 40-hex revision (a version number pasted in);
    6. a repo that is not an https URL.
  known-good
    7. the three values, with comments and blank lines, for the tree's version.

Run it directly, like test_release_plan.py:
`python3 scripts/test_check_upstream_source.py`.
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import check_upstream_source as c  # noqa: E402

REPO = "repo=https://hg.mozilla.org/releases/mozilla-release"
REV = "changeset=737ec77d798755890b062aab541a90d6c75afd19"
GOOD = f"# comment\n\nversion=157.0.1\n{REPO}\n{REV}\n"

REFUSED = [
    ("a file left on the previous release", f"version=156.0.1\n{REPO}\n{REV}\n", "157.0.1"),
    ("a missing changeset", f"version=157.0.1\n{REPO}\n", "157.0.1"),
    ("a value given twice", f"version=157.0.1\n{REPO}\n{REPO}\n{REV}\n", "157.0.1"),
    ("an unknown key", f"version=157.0.1\n{REPO}\n{REV}\nbranch=release\n", "157.0.1"),
    ("a changeset that is a version", f"version=157.0.1\n{REPO}\nchangeset=157.0.1\n", "157.0.1"),
    ("a repo that is not https", f"version=157.0.1\nrepo=hg.mozilla.org\n{REV}\n", "157.0.1"),
]


def main() -> int:
    failures = 0
    for name, text, tree in REFUSED:
        try:
            c.check(c.parse(text), tree)
        except c.SourceError as exc:
            print(f"ok    refused: {name}: {exc}")
        else:
            print(f"FAIL  accepted: {name}")
            failures += 1
    try:
        got = c.parse(GOOD)
        c.check(got, "157.0.1")
        assert got == {"version": "157.0.1",
                       "repo": "https://hg.mozilla.org/releases/mozilla-release",
                       "changeset": "737ec77d798755890b062aab541a90d6c75afd19"}, got
        print("ok    the tree's version with comments and blank lines")
    except (c.SourceError, AssertionError) as exc:
        print(f"FAIL  refused the good file: {exc}")
        failures += 1
    total = len(REFUSED) + 1
    print(f"{total - failures}/{total} cases as expected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
