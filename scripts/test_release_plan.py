#!/usr/bin/env python3
"""release_plan.py, driven against the inputs it exists to refuse.

The plan decides before any build what a run is. These are the runs it must
refuse, each one a real way to get it wrong, and the three it must accept:

  known-bad
    1. a candidate given a release tag - the old habit of naming a candidate
       `firefox-N` or `firefox-N-candidate-x`, which either collided with the
       real draft or died at the seal two hours later;
    2. a release by hand whose tag is not firefox-N (`firefox-49-candidate-b322`,
       the run that died at the seal twice);
    3. a release by hand with no tag at all;
    4. a pushed tag that is not firefox-N (the trigger is `firefox-*`, so
       `firefox-49-rc` would start a release);
    5. a kind nobody declared;
    6. an event that is neither a pushed tag nor a run by hand.
  known-good
    7. a candidate: nothing published, the gate's seal LOCAL;
    8. a release by hand: tag, number, previous tag, the seal under the tag;
    9. a pushed firefox-N tag, whatever the (absent) kind input says.

Run it directly, like test_make_seal.py: `python3 scripts/test_release_plan.py`.
"""
from __future__ import annotations

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
    ("an undeclared kind", ("workflow_dispatch", "nightly", "", "x")),
    ("an empty kind by hand", ("workflow_dispatch", "", "", "x")),
    ("another event", ("pull_request", "release", "firefox-49", "x")),
]

ACCEPTED = [
    ("a candidate", ("workflow_dispatch", "candidate", "", "next/firefox-49"),
     {"kind": "candidate", "tag": "", "num": "", "prevtag": "", "seal_tag": "local"}),
    ("a release by hand", ("workflow_dispatch", "release", "firefox-49", "next/firefox-49"),
     {"kind": "release", "tag": "firefox-49", "num": "49", "prevtag": "firefox-48",
      "seal_tag": "firefox-49"}),
    ("a pushed tag", ("push", "", "", "firefox-50"),
     {"kind": "release", "tag": "firefox-50", "num": "50", "prevtag": "firefox-49",
      "seal_tag": "firefox-50"}),
]


def main() -> int:
    failures = 0
    for name, args in REFUSED:
        try:
            got = p.plan(*args)
        except p.PlanError as exc:
            print(f"ok    refused: {name}: {exc}")
        else:
            print(f"FAIL  accepted: {name}: {got}")
            failures += 1
    for name, args, want in ACCEPTED:
        try:
            got = p.plan(*args)
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
    total = len(REFUSED) + len(ACCEPTED) + 1
    print(f"{total - failures}/{total} cases as expected")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
