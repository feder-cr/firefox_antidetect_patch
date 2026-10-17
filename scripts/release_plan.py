#!/usr/bin/env python3
"""What a run of release.yml or republish.yml is, and what comes out of it.

The ONE place that knows. Every job reads the plan's outputs instead of
deciding again from the event and the inputs.

  candidate  a manual run that builds and gates and publishes NOTHING: no
             seal, no draft, no tag. It has no release tag, and one given to
             it is refused rather than ignored. Its archives are the run's
             `asset-*` artifacts; if they are the ones to ship, republish.yml
             publishes them from this run's id, without a rebuild. The gate
             seals the binary with a LOCAL seal (tag `local`).
  release    a pushed `firefox-N` tag, a manual run with kind=release, or a
             republish: the seal and the DRAFT under a `firefox-N` tag, the
             tag checked here before any build starts.

Why a plan and not a convention. Until 2026-10-17 a manual run decided only at
its END what it was: a candidate dispatched with a release_tag that was not
`firefox-N` built three legs and passed three gates in about two hours, then
died in publish-draft-release on make_seal.py's tag check, so every candidate
run finished "failure" by construction (twice in two days, for B322). A red
that is always there stops being read. The tag rule itself stays make_seal.py's
(`check_release_tag`): this file asks it, it does not repeat it.

    python3 scripts/release_plan.py --event <event> --kind <kind> \\
        --release-tag <tag> --ref-name <ref>

prints the outputs as `key=value` lines for $GITHUB_OUTPUT, or exits 1 naming
what is wrong. Its known-bad inputs: scripts/test_release_plan.py.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from make_seal import SealBuildError, check_release_tag  # noqa: E402

KINDS = ("candidate", "release")
LOCAL_SEAL_TAG = "local"


class PlanError(ValueError):
    pass


def plan(event: str, kind: str, release_tag: str, ref_name: str) -> dict[str, str]:
    """The outputs of a run, or PlanError."""
    if event == "push":
        # a pushed tag is a release by definition, and its name is the tag
        kind, release_tag = "release", ref_name
    elif event != "workflow_dispatch":
        raise PlanError(f"release runs on a pushed tag or by hand, not on {event!r}")
    if kind not in KINDS:
        raise PlanError(f"kind must be one of {', '.join(KINDS)}, got {kind!r}")
    if kind == "candidate":
        if release_tag:
            raise PlanError(
                f"a candidate publishes nothing, so it takes no release tag (got "
                f"{release_tag!r}). To publish a draft from this run set kind=release; "
                f"to publish a candidate's archives later, run republish.yml with its "
                f"run id.")
        return {"kind": "candidate", "tag": "", "num": "", "prevtag": "",
                "seal_tag": LOCAL_SEAL_TAG}
    try:
        n = check_release_tag(release_tag)
    except SealBuildError as exc:
        raise PlanError(f"a release needs a firefox-N tag: {exc}") from None
    return {"kind": "release", "tag": release_tag, "num": str(n),
            "prevtag": f"firefox-{n - 1}" if n > 0 else "", "seal_tag": release_tag}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="what a release run is")
    ap.add_argument("--event", required=True)
    ap.add_argument("--kind", default="")
    ap.add_argument("--release-tag", default="")
    ap.add_argument("--ref-name", default="")
    a = ap.parse_args(argv)
    try:
        outputs = plan(a.event, a.kind, a.release_tag.strip(), a.ref_name)
    except PlanError as exc:
        print(f"PLAN REFUSED: {exc}", file=sys.stderr)
        return 1
    for key, value in outputs.items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
