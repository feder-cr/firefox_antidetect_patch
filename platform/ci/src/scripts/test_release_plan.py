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
  known-good
    9. a candidate: nothing published, the gate's seal LOCAL and numbered
       (firefox-N-candidate, N the last release plus one), the cache on;
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
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import release_plan as p  # noqa: E402

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
    ("a release dispatched from another branch", ("workflow_dispatch", "release", "firefox-50", "latitude/executor", "", "branch")),
    ("an undeclared kind", ("workflow_dispatch", "nightly", "", "x")),
    ("an empty kind by hand", ("workflow_dispatch", "", "", "x")),
    ("another event", ("pull_request", "release", "firefox-49", "x")),
    ("an undeclared compile cache", ("workflow_dispatch", "candidate", "", "x", "on")),
    # by itself: only from the branch main, and with no inputs
    ("a push to another branch", ("push", "", "", "latitude/executor", "", "branch")),
    ("a timer on another branch", ("schedule", "", "", "next/firefox-57", "", "branch")),
    ("a push to main that names a kind", ("push", "release", "", "main", "", "branch")),
    ("a push to main that asks for a cold build", ("push", "", "", "main", "off", "branch")),
]

#: The last published release the workflow reads from the tags: a candidate is the engine after it.
LAST_RELEASE = "48"

#: What every plan carries besides its kind: the gtests and the refs.
DEFAULTS = {"gtests": "false", "gtest_filter": p.DEFAULT_GTEST_FILTER,
            "core_ref": "main", "wrapper_ref": "main"}

ACCEPTED = [
    ("a candidate", ("workflow_dispatch", "candidate", "", "next/firefox-49"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-49-candidate",
      "compile_cache": "gha"}),
    ("a candidate without the cache",
     ("workflow_dispatch", "candidate", "", "next/firefox-49", "off"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-49-candidate",
      "compile_cache": "off"}),
    ("a release by hand", ("workflow_dispatch", "release", "firefox-49", "next/firefox-49"),
     {"kind": "release", "tag": "firefox-49", "num": "49", "prevtag": "firefox-48",
      "seal_tag": "firefox-49", "compile_cache": "gha"}),
    ("a push to main: a candidate with the gtests and the cache",
     ("push", "", "", "main", "", "branch"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-49-candidate",
      "compile_cache": "gha", "gtests": "true"}),
    ("the weekly timer on main: the same candidate",
     ("schedule", "", "", "main", "", "branch"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-49-candidate",
      "compile_cache": "gha", "gtests": "true"}),
    ("a candidate by hand with the gtests and its own refs",
     ("workflow_dispatch", "candidate", "", "main", "", "branch", "true",
      "StealthGfxFeatureStatus.*", "keyboard-per-locale", "remove-font-gate"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-49-candidate",
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
     ("workflow_dispatch", "candidate", "", "latitude/executor", "", "branch"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "firefox-49-candidate",
      "compile_cache": "gha"}),
]


def main() -> int:
    failures = 0
    for name, args in REFUSED:
        try:
            got = p.plan(*args, last_release=LAST_RELEASE)
        except p.PlanError as exc:
            print(f"ok    refused: {name}: {exc}")
        else:
            print(f"FAIL  accepted: {name}: {got}")
            failures += 1
    for name, args, want in ACCEPTED:
        want = {**DEFAULTS, **want}
        try:
            got = p.plan(*args, last_release=LAST_RELEASE)
        except p.PlanError as exc:
            print(f"FAIL  refused: {name}: {exc}")
            failures += 1
            continue
        if got != want:
            print(f"FAIL  {name}: {got} != {want}")
            failures += 1
        else:
            print(f"ok    {name}")
    # the command line: outputs on stdout for $GITHUB_OUTPUT, a refusal as exit 1
    if p.main(["--event", "workflow_dispatch", "--kind", "release",
               "--release-tag", "firefox-49-candidate-b322"]) != 1:
        print("FAIL  the command line accepted a bad release tag")
        failures += 1
    # A candidate is sealed firefox-N-candidate (the wrapper's tests read N from the seal, and fail on `local`): N is the
    # last published release plus one, and a candidate without the last release is refused, not sealed `local`.
    for name, last, want in (("a candidate after firefox-62 is firefox-63-candidate", "62", "firefox-63-candidate"),
                             ("a candidate after firefox-9 is firefox-10-candidate (numbers, not strings)", "9", "firefox-10-candidate")):
        got = p.plan("workflow_dispatch", "candidate", "", "latitude/firefox-63-udp", "", "branch", last_release=last)
        if got["seal_tag"] != want:
            print(f"FAIL  {name}: {got['seal_tag']} != {want}")
            failures += 1
        else:
            print(f"ok    {name}")
    for name, last in (("a candidate without the last release", ""), ("a candidate with a last release that is no number", "firefox-62"),
                       ("a candidate with a negative last release", "-1")):
        try:
            got = p.plan("workflow_dispatch", "candidate", "", "latitude/firefox-63-udp", "", "branch", last_release=last)
        except p.PlanError as exc:
            print(f"ok    refused: {name}: {exc}")
        else:
            print(f"FAIL  accepted: {name}: {got}")
            failures += 1
    got = p.plan("workflow_dispatch", "release", "firefox-63", "main", "", "branch", last_release="62")
    if got["seal_tag"] != "firefox-63":
        print(f"FAIL  a release is sealed under its own tag whatever the last release: {got['seal_tag']}")
        failures += 1
    else:
        print("ok    a release is sealed under its own tag")

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
    total = len(REFUSED) + len(ACCEPTED) + 2 + 6
    print(f"{total - failures}/{total} cases as expected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
