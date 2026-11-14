#!/usr/bin/env python3
"""Is this run still the head of its branch? The guard that stands in front of the node (release.yml, job `checks`, last step).

The rule is written in docs/decisions/ci-release-17: «un nuovo push annulla la prova vecchia». release.yml has a `concurrency` group per branch for it,
which cancels a run that a newer one replaces. This is the other half: a run that was queued behind the new one, or started in the seconds
between, is not cancelled by the group, and would turn a server on (an hour paid) for a commit nobody is waiting for. So before the node is
turned on the run asks the repository what the head of its branch is NOW, and if it is not the commit the run builds, the run is cancelled.

It judges only what a newer push can replace: a CANDIDATE on a branch (a push, the weekly run, a dispatch of a candidate). It says nothing, and exits 0, for:
  * a RELEASE (kind=release): it builds the commit it was asked for, whatever the branch does after;
  * a re-launch (relaunch_of): the run that replaces a run whose machine was lost;
  * a ref that is not a branch (a tag).

    python3 scripts/ci/still_head.py --event push --ref refs/heads/main --sha <sha> --kind '' --relaunch-of '' --repository owner/repo

Exit 0: go on. Exit 1: superseded (it says by which commit). Exit 2: the head could not be read; the run goes on (a guard that cannot see is not a reason to stop a build).
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys

BRANCH = "refs/heads/"
#: The only branch whose pushes start a run of this workflow (`on.push.branches`). On any other branch a newer push starts nothing, so a run that
#: is cancelled because the branch moved is replaced by nobody (a candidate on a latitude/<name> branch is dispatched by hand).
PUSH_BRANCH = "refs/heads/main"
#: `on.push.paths-ignore`: a push that touches only these runs no candidate.
IGNORED_SUFFIXES = (".md",)
COMPARE_LIMIT = 300      # the compare API lists at most this many files: a list this long is not Markdown only


def applies(event: str, ref: str, kind: str, relaunch_of: str) -> bool:
    """Whether a newer push can replace this run: a candidate on the branch whose pushes start candidates."""
    if kind == "release" or relaunch_of:
        return False
    return event in ("push", "schedule", "workflow_dispatch") and ref == PUSH_BRANCH


def head_of(repository: str, ref: str, run=subprocess.run) -> str:
    """The commit the branch points at now, as the repository says it (public: no token), or '' if it cannot be read."""
    try:
        done = run(["git", "ls-remote", "https://github.com/%s" % repository, ref], capture_output=True, text=True, timeout=60)
    except (subprocess.SubprocessError, OSError, UnicodeError):    # a hang past the timeout is "could not read", never "superseded"
        return ""
    if done.returncode != 0:
        return ""
    for line in done.stdout.splitlines():
        sha, _, name = line.partition("\t")
        if name.strip() == ref:
            return sha.strip()
    return ""


def changed_files(repository: str, base: str, head: str, run=subprocess.run):
    """The paths that differ between two commits, from the compare API (`gh api`, the run's token); None when they cannot be read."""
    try:
        done = run(["gh", "api", "repos/%s/compare/%s...%s" % (repository, base, head)], capture_output=True, text=True, timeout=60)
        if done.returncode != 0:
            return None
        document = json.loads(done.stdout)
        files = [entry["filename"] for entry in document.get("files", [])]
    except (subprocess.SubprocessError, OSError, UnicodeError, ValueError, KeyError, TypeError):
        return None
    return files if len(files) < COMPARE_LIMIT else files + ["<more than %d files>" % COMPARE_LIMIT]


def judge(event: str, ref: str, sha: str, kind: str, relaunch_of: str, repository: str, head=head_of, changed=changed_files) -> tuple:
    """(exit code, what to say)."""
    if not applies(event, ref, kind, relaunch_of):
        return 0, "not judged: a release, a re-launch or a ref whose pushes start no candidate (only main's do) is never replaced by a push"
    now = head(repository, ref)
    if not now:
        return 2, "the head of %s could not be read: the run goes on" % ref
    if now == sha:
        return 0, "still the head of %s (%s)" % (ref, sha[:12])
    # A newer head replaces this run only if the push that made it STARTED a run: one that touches only Markdown (`paths-ignore`) starts none, and
    # cancelling this run for it would leave the head of the branch with no candidate at all.
    files = changed(repository, sha, now)
    if files is None:
        return 2, "the paths between %s and %s could not be read: the run goes on" % (sha[:12], now[:12])
    if files and all(path.lower().endswith(IGNORED_SUFFIXES) for path in files):
        return 0, "%s is now at %s, but what it added since %s is Markdown only and starts no candidate: this run stays" % (ref, now[:12], sha[:12])
    return 1, "superseded: %s is now at %s and this run builds %s" % (ref, now[:12], sha[:12])


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    for name in ("event", "ref", "sha", "kind", "relaunch-of", "repository"):
        ap.add_argument("--" + name, required=True)
    a = ap.parse_args(argv)
    try:
        code, said = judge(a.event, a.ref, a.sha, a.kind, a.relaunch_of, a.repository)
    except Exception as exc:        # nothing unexpected may read as "superseded" (exit 1 cancels the run): 2 is "go on"
        code, said = 2, "the guard failed (%s: %s): the run goes on" % (type(exc).__name__, exc)
    print(said)
    return code


if __name__ == "__main__":
    sys.exit(main())
