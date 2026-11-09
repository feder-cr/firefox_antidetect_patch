#!/usr/bin/env python3
"""release_plan.py, driven against the inputs it exists to refuse.

The plan decides before any build what a run is. These are the runs it must
refuse, each one a real way to get it wrong, and the runs it must accept:

  known-bad
    1. a candidate given a release tag - the old habit of naming a candidate
       `firefox-N` or `firefox-N-candidate-x`, which either collided with the
       real draft or died at the seal two hours later;
    2. a release by hand whose tag is not firefox-N (`firefox-49-candidate-b322`,
       the run that died at the seal twice);
    3. a release by hand with no tag at all;
    4. a pushed tag, any tag: the trigger is gone, and what a run from a tag saves
       nothing after it can read;
    5. a kind nobody declared;
    6. an event that is not a run by hand;
    7. a build dispatched from a tag ref, and a release from any branch but main;
    8. a compile cache nobody declared;
    8b. --builder: there is no such choice any more.
  the default gtest filter (gtest_coverage below)
    14. every gtest of ours - the TEST, TEST_F and TEST_P that the series added to the
        tree - is named by the filter a candidate runs by default; a suite that the
        filter does not name is built by the candidate and never run.
  known-good
    9. a candidate: nothing published, the gate's seal firefox-N-candidate, the cache on;
   10. a candidate with the cache off, a cold build;
   11. a release by hand: tag, number, previous tag, the seal under the tag,
       the cache on;
   12. a release by hand that asks for the cache off, or for it by name;
   13. a release from main, a candidate from a branch; republish.yml passes no ref
       type and is not asked.

Run it directly, like test_make_seal.py: `python3 scripts/test_release_plan.py`.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import release_plan as p  # noqa: E402
from our_gtests import gtest_matches, our_gtests, series_root  # noqa: E402

#: The release tags the workflow reads from the remote: out of order on purpose
#: (the number is what counts, not the sort), and with things that are not releases.
TAGS = "firefox-9 firefox-58 firefox-10 firefox-58-candidate v0.18.0 firefox-x"

REFUSED = [
    ("a candidate given a release tag", ("workflow_dispatch", "candidate", "firefox-49", "x")),
    ("a candidate given a candidate's name",
     ("workflow_dispatch", "candidate", "firefox-49-candidate-b322", "x")),
    ("a release whose tag is not firefox-N",
     ("workflow_dispatch", "release", "firefox-49-candidate-b322", "x")),
    ("a release with no tag", ("workflow_dispatch", "release", "", "x")),
    ("a pushed tag that is not firefox-N", ("push", "", "", "firefox-49-rc")),
    ("a pushed firefox-N tag: its cache entries nothing could read", ("push", "", "", "firefox-50")),
    ("a candidate dispatched from a tag", ("workflow_dispatch", "candidate", "", "firefox-50", "", "tag")),
    ("a release dispatched from a tag", ("workflow_dispatch", "release", "firefox-50", "firefox-50", "", "tag")),
    ("a release dispatched from another branch", ("workflow_dispatch", "release", "firefox-50", "theta/executor", "", "branch")),
    ("an undeclared kind", ("workflow_dispatch", "nightly", "", "x")),
    ("an empty kind by hand", ("workflow_dispatch", "", "", "x")),
    ("another event", ("pull_request", "release", "firefox-49", "x")),
    ("an undeclared compile cache", ("workflow_dispatch", "candidate", "", "x", "on")),
    # by itself: only from the branch main, and with no inputs
    ("a push to another branch", ("push", "", "", "theta/executor", "", "branch")),
    ("a timer on another branch", ("schedule", "", "", "next/firefox-57", "", "branch")),
    ("a push to main that names a kind", ("push", "release", "", "main", "", "branch")),
    ("a push to main that asks for a cold build", ("push", "", "", "main", "off", "branch")),
]

#: Refused only for want of release tags: a candidate cannot be named without them.
NO_TAGS = [
    ("a candidate when no firefox-N tag exists", ("workflow_dispatch", "candidate", "", "x"), ""),
    ("a candidate when the tags are not releases", ("workflow_dispatch", "candidate", "", "x"),
     "firefox-58-candidate v0.18.0 firefox-x"),
]

#: What every plan carries besides its kind: the gtests and the refs.
DEFAULTS = {"gtests": "false", "gtest_filter": p.DEFAULT_GTEST_FILTER,
            "core_ref": "main", "wrapper_ref": "main"}

ACCEPTED = [
    ("a candidate", ("workflow_dispatch", "candidate", "", "next/firefox-49"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-59-candidate",
      "compile_cache": "gha"}),
    ("a candidate without the cache",
     ("workflow_dispatch", "candidate", "", "next/firefox-49", "off"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-59-candidate",
      "compile_cache": "off"}),
    ("a release by hand", ("workflow_dispatch", "release", "firefox-49", "next/firefox-49"),
     {"kind": "release", "tag": "firefox-49", "num": "49", "prevtag": "firefox-48",
      "seal_tag": "firefox-49", "compile_cache": "gha"}),
    ("a push to main: a candidate with the gtests and the cache",
     ("push", "", "", "main", "", "branch"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-59-candidate",
      "compile_cache": "gha", "gtests": "true"}),
    ("the weekly timer on main: the same candidate",
     ("schedule", "", "", "main", "", "branch"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-59-candidate",
      "compile_cache": "gha", "gtests": "true"}),
    ("a candidate by hand with the gtests and its own refs",
     ("workflow_dispatch", "candidate", "", "main", "", "branch", "true",
      "StealthGfxFeatureStatus.*", "keyboard-per-locale", "remove-font-gate"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-59-candidate",
      "compile_cache": "gha", "gtests": "true", "gtest_filter": "StealthGfxFeatureStatus.*",
      "core_ref": "keyboard-per-locale", "wrapper_ref": "remove-font-gate"}),
    ("a release by hand asking for the cache",
     ("workflow_dispatch", "release", "firefox-49", "next/firefox-49", "gha"),
     {"kind": "release", "tag": "firefox-49", "num": "49", "prevtag": "firefox-48",
      "seal_tag": "firefox-49", "compile_cache": "gha"}),
    ("a release by hand asking for no cache",
     ("workflow_dispatch", "release", "firefox-49", "next/firefox-49", "off"),
     {"kind": "release", "tag": "firefox-49", "num": "49", "prevtag": "firefox-48",
      "seal_tag": "firefox-49", "compile_cache": "off"}),
    ("a release dispatched from main",
     ("workflow_dispatch", "release", "firefox-50", "main", "", "branch"),
     {"kind": "release", "tag": "firefox-50", "num": "50", "prevtag": "firefox-49",
      "seal_tag": "firefox-50", "compile_cache": "gha"}),
    ("a candidate dispatched from a branch",
     ("workflow_dispatch", "candidate", "", "theta/executor", "", "branch"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-59-candidate",
      "compile_cache": "gha"}),
]


ROOT = Path(__file__).resolve().parent.parent


def gtest_coverage(gtest_filter: str, series: Path | None = None) -> list:
    """The reasons the filter does not cover our gtests: [] when it does. A series that cannot be
    read, or that has no gtest of ours, is a reason too: a check that did not run is not a pass."""
    try:
        series = series or series_root()
        found = our_gtests(series)
    except OSError as exc:
        return ["the gtest coverage was NOT checked: %s" % exc]
    if not found:
        return ["the gtest coverage was NOT checked: no TEST/TEST_F/TEST_P of ours in %s (wrong series?)" % series]
    return ["%s: %s is not run by the default gtest filter (add its suite to DEFAULT_GTEST_FILTER in release_plan.py, "
            "to release.yml and to test_release_path.py)" % (f, n) for f, n, _ in found if not gtest_matches(gtest_filter, n)]


#: The matcher itself, against what gtest does: (filter, name, runs).
GTEST_MATCHING = [
    ("A*.*", "Alpha.One", True), ("A*.*", "Beta.One", False), ("A*.*:B*.*", "Beta.One", True),
    ("A*.*-A*.One", "Alpha.One", False), ("A*.*-A*.One", "Alpha.Two", True),
    ("*", "Any.Thing", True), ("-A*", "Alpha.One", False), ("-A*", "Beta.One", True),
    ("Suite.Test", "Suite.Test", True), ("Suite.Test", "Suite.Other", False),
    ("A?.*", "Ab.One", True), ("A?.*", "Abc.One", False), ("A.*", "X/A.One/0", False), ("*/A.*", "X/A.One/0", True),
]


def main() -> int:
    failures = 0
    for flt, name, want in GTEST_MATCHING:
        if gtest_matches(flt, name) != want:
            print(f"FAIL  the gtest matcher: {flt!r} on {name!r} should be {want}")
            failures += 1
    reasons = gtest_coverage(p.DEFAULT_GTEST_FILTER)
    for reason in reasons:
        print(f"FAIL  {reason}")
    if reasons:
        failures += 1
    else:
        print("ok    every gtest of ours is named by the default gtest filter")
    for name, args in REFUSED:
        try:
            got = p.plan(*args, release_tags=TAGS)
        except p.PlanError as exc:
            print(f"ok    refused: {name}: {exc}")
        else:
            print(f"FAIL  accepted: {name}: {got}")
            failures += 1
    for name, args, tags in NO_TAGS:
        try:
            got = p.plan(*args, release_tags=tags)
        except p.PlanError as exc:
            print(f"ok    refused: {name}: {exc}")
        else:
            print(f"FAIL  accepted: {name}: {got}")
            failures += 1
    for name, args, want in ACCEPTED:
        want = {**DEFAULTS, **want}
        try:
            got = p.plan(*args, release_tags=TAGS)
        except p.PlanError as exc:
            print(f"FAIL  refused: {name}: {exc}")
            failures += 1
            continue
        if got != want:
            print(f"FAIL  {name}: {got} != {want}")
            failures += 1
        else:
            print(f"ok    {name}")
    # the tags come in on the command line and name the candidate
    import contextlib
    import io
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        rc = p.main(["--event", "workflow_dispatch", "--kind", "candidate", "--ref-name", "x",
                     "--release-tags", TAGS.replace(" ", chr(10))])
    if rc != 0 or "seal_tag=firefox-59-candidate" not in out.getvalue():
        print("FAIL  the command line did not name the candidate from --release-tags")
        failures += 1
    # the command line: outputs on stdout for $GITHUB_OUTPUT, a refusal as exit 1
    if p.main(["--event", "workflow_dispatch", "--kind", "release",
               "--release-tag", "firefox-49-candidate-b322"]) != 1:
        print("FAIL  the command line accepted a bad release tag")
        failures += 1
    # No fallback: the choice of who compiles does not exist, so it cannot be made.
    stderr, sys.stderr = sys.stderr, open(os.devnull, "w")
    try:
        p.main(["--event", "workflow_dispatch", "--kind", "candidate", "--builder", "github"])
        took = True
    except SystemExit as exc:
        took = exc.code != 2
    finally:
        sys.stderr.close()
        sys.stderr = stderr
    if took:
        print("FAIL  the command line took a builder")
        failures += 1
    total = len(REFUSED) + len(NO_TAGS) + len(ACCEPTED) + 3 + len(GTEST_MATCHING) + 1
    print(f"{total - failures}/{total} cases as expected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
