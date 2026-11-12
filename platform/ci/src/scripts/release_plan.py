#!/usr/bin/env python3
"""What a run of release.yml or republish.yml is, and what comes out of it.

The ONE place that knows. Every job reads the plan's outputs instead of
deciding again from the event and the inputs.

  candidate  a manual run that builds and gates and publishes NOTHING: no
             seal, no draft, no tag. It has no release tag, and one given to
             it is refused rather than ignored. Its archives are the run's
             `asset-*` artifacts; if they are the ones to ship, republish.yml
             publishes them from this run's id, without a rebuild. The gate
             seals the binary with a LOCAL seal named `firefox-N-candidate`, N
             being the engine this candidate would become: the last published
             `firefox-N` plus one (`--last-release`, read from the tags by the
             workflow). The wrapper's tests ask the engine's number (a property of
             the UDP line is for firefox-63 or later), and a tag with no number
             (`local`) does not say which engine it is: they fail on it.
  release    a manual run with kind=release, or a republish: the seal and the
             DRAFT under a `firefox-N` tag, the tag checked here before any build
             starts.

The compile cache (`compile_cache`) is decided here too, for the same reason.
Every run compiles through sccache with GitHub's Actions cache behind it, so a
run after a fix recompiles only what the fix touched instead of the whole tree
(a cold leg is 1-2 hours; a port to a new Firefox needs many runs). Releases too,
since 2026-11-03 (Federico's decision): the cache's keys are the hash of the
preprocessed source, the arguments and the compiler. `--compile-cache off` asks
for a cold build.

Who compiles is no choice: the two legs, Linux and Windows x86_64, on a build node,
every run, candidates and releases alike (scripts/ci/latitude_node.py); gates and tests
always on GitHub. No linux-arm64 from firefox-56 on (Federico, 2026-11-04), and which
legs a run builds is no choice either: always both. So the plan has no say.

Where a run starts from is checked here too (`--ref-type`, release.yml's own). An
actions/cache entry belongs to its ref, and a run reads only its own ref's and the
default branch's: what a run from a tag saves, nothing after it can read. So no build
runs from a tag, and a release is dispatched from main, where every later run finds
its cache.

Why a plan and not a convention. Until 2026-10-17 a manual run decided only at
its END what it was: a candidate dispatched with a release_tag that was not
`firefox-N` built three legs and passed three gates in about two hours, then
died in publish-draft-release on make_seal.py's tag check, so every candidate
run finished "failure" by construction (twice in two days, for B322). A red
that is always there stops being read. The tag rule itself stays make_seal.py's
(`check_release_tag`): this file asks it, it does not repeat it.

    python3 scripts/release_plan.py --event <event> --kind <kind> \\
        --release-tag <tag> --ref-name <ref> [--compile-cache gha|off] \\
        [--ref-type branch|tag]

prints the outputs as `key=value` lines for $GITHUB_OUTPUT, or exits 1 naming
what is wrong. Its known-bad inputs: scripts/test_release_plan.py.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from make_seal import SealBuildError, check_release_tag  # noqa: E402

KINDS = ("candidate", "release")
# A candidate's seal tag: the number of the engine it would become, then "-candidate". It is not a release tag
# (the core reads it as a local seal), and the wrapper's tests read the number from it.
CANDIDATE_SEAL_TAG = "firefox-{n}-candidate"
# What the compile_cache output can say. "gha" is sccache on the GitHub Actions
# cache; "off" is a cold build. Anything else is refused.
COMPILE_CACHES = ("gha", "off")
# The only branch a release's cache is readable from afterwards.
RELEASE_BRANCH = "main"
# The events that run a candidate by themselves, from main, with the gtests: a push
# to main and the weekly timer. They keep main's compile cache warm (GitHub keeps a
# cache per branch and drops it after seven days unused; a cold release was 75 min
# against 43 warm, measured 2026-11-07).
AUTOMATIC_EVENTS = ("push", "schedule")
# What an automatic candidate runs, and what a run by hand runs when its input is
# empty: the same list as release.yml's gtest_filter default.
DEFAULT_GTEST_FILTER = ("StealthGfxFeatureStatus.*:StealthWebGPUAdapter.*:"
                        "StealthWebGPUDeclaration.*:StealthWebGPUBundledVulkan.*:"
                        "StealthWebGPUInstanceEnv.*:StealthFullscreen.*:"
                        "StealthScaleDeclaration.*:StealthWindowGeometry.*:"
                        "StealthGLMultisample.*:StealthFrameCrash.*:"
                        "StealthJugglerEventId.*:StealthSessionUdpProxy.*:StealthDeclaredNetwork.*:StealthSocksUdpPool.*:TestStealthTRRContext.*:TestStealthSocksCredentials.*:TestStealthSessionResolver.*:StealthIceResolverContext.*:StealthUdpProxyBirth.*")
DEFAULT_REF = "main"


class PlanError(ValueError):
    pass


def plan(event: str, kind: str, release_tag: str, ref_name: str,
         compile_cache: str = "", ref_type: str = "", gtests: str = "",
         gtest_filter: str = "", core_ref: str = "", wrapper_ref: str = "",
         last_release: str = "") -> dict[str, str]:
    """The outputs of a run, or PlanError.

    compile_cache is the run's request: "" takes the default. ref_type is what the
    run started from; republish.yml builds nothing, passes none and is not asked.
    gtests, gtest_filter, core_ref and wrapper_ref are the run's inputs, empty on an
    automatic run: it is a candidate from main with the gtests and the defaults.
    last_release is the number N of the last published `firefox-N` (the workflow reads
    it from the tags): a candidate is the engine after it."""
    if event in AUTOMATIC_EVENTS:
        if ref_type != "branch" or ref_name != RELEASE_BRANCH:
            raise PlanError(f"a run by itself is a candidate from the branch {RELEASE_BRANCH}, "
                            f"not from {ref_name!r} ({ref_type or 'no ref type'})")
        if kind or release_tag or compile_cache:
            raise PlanError("a run by itself takes no inputs: it is a candidate with the cache")
        kind, gtests = "candidate", "true"
    elif event != "workflow_dispatch":
        raise PlanError(f"release runs by hand, from main for a release, or by itself on "
                        f"{' and '.join(AUTOMATIC_EVENTS)} from {RELEASE_BRANCH}, not on {event!r}")
    common = {"gtests": "true" if gtests == "true" else "false",
              "gtest_filter": gtest_filter or DEFAULT_GTEST_FILTER,
              "core_ref": core_ref or DEFAULT_REF, "wrapper_ref": wrapper_ref or DEFAULT_REF}
    if kind not in KINDS:
        raise PlanError(f"kind must be one of {', '.join(KINDS)}, got {kind!r}")
    if compile_cache and compile_cache not in COMPILE_CACHES:
        raise PlanError(f"compile_cache must be one of {', '.join(COMPILE_CACHES)}, "
                        f"got {compile_cache!r}")
    if ref_type == "tag":
        raise PlanError(f"a build from the tag {ref_name!r} would save cache entries nothing can read: "
                        f"dispatch it from a branch, a release from {RELEASE_BRANCH}")
    if kind == "release" and ref_type and ref_name != RELEASE_BRANCH:
        raise PlanError(f"a release is dispatched from {RELEASE_BRANCH}, whose cache every later run reads, "
                        f"not from {ref_name!r}")
    if kind == "candidate":
        if release_tag:
            raise PlanError(
                f"a candidate publishes nothing, so it takes no release tag (got "
                f"{release_tag!r}). To publish a draft from this run set kind=release; "
                f"to publish a candidate's archives later, run republish.yml with its "
                f"run id.")
        if not re.fullmatch(r"[0-9]+", last_release):
            raise PlanError(f"a candidate is sealed firefox-N-candidate, N being the engine after the last published "
                            f"release: the number of the last release is needed (got {last_release!r})")
        return {"kind": "candidate", "tag": "", "num": "", "prevtag": "",
                "seal_tag": CANDIDATE_SEAL_TAG.format(n=int(last_release) + 1),
                "compile_cache": compile_cache or "gha", **common}
    try:
        n = check_release_tag(release_tag)
    except SealBuildError as exc:
        raise PlanError(f"a release needs a firefox-N tag: {exc}") from None
    return {"kind": "release", "tag": release_tag, "num": str(n),
            "prevtag": f"firefox-{n - 1}" if n > 0 else "", "seal_tag": release_tag,
            "compile_cache": compile_cache or "gha", **common}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="what a release run is")
    ap.add_argument("--event", required=True)
    ap.add_argument("--kind", default="")
    ap.add_argument("--release-tag", default="")
    ap.add_argument("--ref-name", default="")
    ap.add_argument("--compile-cache", default="")
    ap.add_argument("--ref-type", default="")
    ap.add_argument("--gtests", default="")
    ap.add_argument("--gtest-filter", default="")
    ap.add_argument("--core-ref", default="")
    ap.add_argument("--wrapper-ref", default="")
    ap.add_argument("--last-release", default="", help="N of the last published firefox-N tag (a candidate is N+1)")
    a = ap.parse_args(argv)
    try:
        outputs = plan(a.event, a.kind, a.release_tag.strip(), a.ref_name,
                       a.compile_cache.strip(), a.ref_type.strip(), a.gtests.strip(),
                       a.gtest_filter.strip(), a.core_ref.strip(), a.wrapper_ref.strip(),
                       a.last_release.strip())
    except PlanError as exc:
        print(f"PLAN REFUSED: {exc}", file=sys.stderr)
        return 1
    for key, value in outputs.items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
