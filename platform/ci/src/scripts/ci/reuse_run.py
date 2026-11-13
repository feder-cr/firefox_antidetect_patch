#!/usr/bin/env python3
"""Is there a green run of release.yml whose build is THIS tree? Then the run does not turn a node on to compile it again.

The merge of a pull request on main started a candidate (a node, an hour paid, 25–45 minutes of compiling) for a tree identical to the one a green
candidate had just compiled (run 43241083352 against 43212…, 12 November): the series changed, the engine did not. What that run needs is the gate
again, with the refs of today, on the bytes that exist. republish.yml does that by hand; this does it for the push that finds the bytes.

It reuses a run only when ALL of this holds, and otherwise the run builds as it always did:
  * the event is a PUSH to MAIN and the plan says it is a candidate. Never the weekly run (the compile cache is per branch and dies after seven days
    unused: Monday's run must really compile, to keep main's warm: a release is 43 minutes with the cache and 75 without), never a dispatch, never a
    release, never a re-launch;
  * the run was green (the whole run: its gate, its gtests), is less than MAX_AGE_DAYS old, and is not this one;
  * its artifacts still exist: `asset-linux-x86_64`, `asset-win-x86_64` and `source-commit` (retention 7 days) are not expired;
  * its gtests ran, when this run's would: the reports of BOTH legs exist (`gtest-reports-linux-x86_64`, `gtest-reports-win-x86_64`) and each one counts
    at least one test that was not disabled (a green run without them proves nothing about this tree's gtests);
  * and they were THESE gtests: the run says which filter it ran (`gtest-filter.txt` in its `source-commit` artifact) and that filter is the one this run
    would use, or one that contains every pattern of it (`*` contains all). A run with no such file is not reused: a proof candidate on a branch can be
    green on `StealthFullscreen.*` alone, and the fast-forward of that branch to main is a push of the same tree;
  * it built the same TREE. The identity is the tree object (`git rev-parse HEAD^{tree}`, in `source-tree.txt` of the `source-commit` artifact since this
    file exists), not the commit, which also moves for a folder's message; a run from before that file is compared by commit.

    python3 scripts/ci/reuse_run.py --event push --ref refs/heads/main --kind candidate --gtests true --repository o/r --run-id 7
        prints `reuse_run_id=<id>` (empty when none) and, on stderr, why.
"""
from __future__ import annotations

import argparse
import datetime as dt
import io
import json
import subprocess
import sys
import zipfile

MAX_AGE_DAYS = 6          # the archives are kept 7: the gate that follows has to download them
RUNS_LOOKED_AT = 30
NEEDED = ("asset-linux-x86_64", "asset-win-x86_64", "source-commit")
GTEST_REPORTS = ("gtest-reports-linux-x86_64", "gtest-reports-win-x86_64")


def applies(event: str, ref: str, kind: str) -> bool:
    return event == "push" and ref == "refs/heads/main" and kind == "candidate"


def covers(previous: str, wanted: str) -> bool:
    """Whether the gtests a run ran (`previous`, a gtest filter as `mach gtest` takes it) include the ones this run would run (`wanted`).

    Patterns are compared as written: the same filter, or every colon-separated pattern of `wanted` among those of `previous`, or `*` in `previous`. A
    filter with exclusions (a `-`) is covered only by the identical text: what it leaves out cannot be told by comparing strings."""
    previous, wanted = previous.strip(), wanted.strip()
    if not previous or not wanted:
        return False
    if previous == wanted:
        return True
    if "-" in previous or "-" in wanted:
        return False
    have = {pattern for pattern in previous.split(":") if pattern}
    if "*" in have:
        return True
    return {pattern for pattern in wanted.split(":") if pattern} <= have


def gh(args, binary=False):
    done = subprocess.run(["gh", "api"] + list(args), capture_output=True, timeout=120)
    if done.returncode != 0:
        raise OSError("gh api %s: %s" % (" ".join(args)[:80], done.stderr.decode("utf-8", "replace")[-200:]))
    return done.stdout if binary else json.loads(done.stdout.decode("utf-8"))


class Github:
    def __init__(self, repository: str):
        self.repository = repository

    def green_runs(self) -> list:
        data = gh(["repos/%s/actions/workflows/release.yml/runs?status=success&per_page=%d" % (self.repository, RUNS_LOOKED_AT)])
        return data.get("workflow_runs", [])

    def artifacts(self, run_id: int) -> list:
        return gh(["repos/%s/actions/runs/%d/artifacts?per_page=100" % (self.repository, run_id)]).get("artifacts", [])

    def tests_counted(self, artifact_id: int) -> int:
        """The tests that ran (not disabled) in the reports of one leg: the sum over the json of each suite."""
        blob = gh(["repos/%s/actions/artifacts/%d/zip" % (self.repository, artifact_id)], binary=True)
        total = 0
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            for name in z.namelist():
                if name.endswith(".json"):
                    report = json.loads(z.read(name).decode("utf-8"))
                    total += int(report.get("tests", 0)) - int(report.get("disabled", 0))
        return total

    def source(self, artifact_id: int) -> dict:
        """{'commit': …, 'tree': … or '', 'gtest_filter': … or None (no such file)} from the `source-commit` artifact."""
        blob = gh(["repos/%s/actions/artifacts/%d/zip" % (self.repository, artifact_id)], binary=True)
        with zipfile.ZipFile(io.BytesIO(blob)) as z:
            names = z.namelist()
            text = lambda n: z.read(n).decode("utf-8").strip() if n in names else ""  # noqa: E731
            return {"commit": text("source-commit.txt"), "tree": text("source-tree.txt"),
                    "gtest_filter": text("gtest-filter.txt") if "gtest-filter.txt" in names else None}


def choose(runs, artifacts_of, source_of, tree: str, commit: str, now, current_run: int, gtests: bool, max_age_days=MAX_AGE_DAYS,
           gtest_filter: str = "", tests_of=None) -> tuple:
    """(run id or '', why), looking at `runs` newest first. `gtest_filter` is the filter this run would use; `tests_of(artifact id)` counts the tests
    in one leg's reports (None: not asked, which is only for a caller that does not run gtests)."""
    looked = 0
    for run in runs:
        if run["id"] == current_run:
            continue
        created = dt.datetime.fromisoformat(run["created_at"].replace("Z", "+00:00"))
        if (now - created).total_seconds() > max_age_days * 86400:
            return "", "no green run within %d days has this tree (looked at %d)" % (max_age_days, looked)
        looked += 1
        have = {a["name"]: a for a in artifacts_of(run["id"]) if not a.get("expired")}
        if any(name not in have for name in NEEDED):
            continue
        if gtests and any(name not in have for name in GTEST_REPORTS):
            continue
        said = source_of(have["source-commit"]["id"])
        # which gtests that run ran: no record, no reuse (a run from before this file is built again once)
        if said.get("gtest_filter") is None:
            continue
        if gtests and not covers(said["gtest_filter"], gtest_filter):
            continue
        if gtests and (tests_of is None or any(tests_of(have[name]["id"]) <= 0 for name in GTEST_REPORTS)):
            continue
        if (said.get("tree") and said["tree"] == tree) or (not said.get("tree") and said.get("commit") and said["commit"] == commit):
            return str(run["id"]), "run %s (%s, %s) built this tree: %s" % (run["id"], run.get("event"), run.get("head_branch"), tree[:12])
    return "", "no green run has this tree (looked at %d)" % looked


def main(argv=None, github=None, now=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    for name in ("event", "ref", "kind", "gtests", "repository", "run-id", "tree", "commit", "gtest-filter"):
        ap.add_argument("--" + name, required=True)
    a = ap.parse_args(argv)
    if not applies(a.event, a.ref, a.kind):
        print("reuse_run_id=")
        print("no reuse: only the push to main of a candidate is judged (%s %s %s)" % (a.event, a.ref, a.kind), file=sys.stderr)
        return 0
    github = github or Github(a.repository)
    try:
        found, why = choose(github.green_runs(), github.artifacts, github.source, a.tree, a.commit,
                            now or dt.datetime.now(dt.timezone.utc), int(a.run_id), a.gtests == "true",
                            gtest_filter=a.gtest_filter, tests_of=github.tests_counted)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as exc:
        found, why = "", "the runs could not be read (%s): this run builds" % exc
    print("reuse_run_id=%s" % found)
    print(why, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
