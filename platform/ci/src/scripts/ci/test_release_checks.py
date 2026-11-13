#!/usr/bin/env python3
"""release.yml: what does not depend on the binary is checked BEFORE a node is turned on, a re-launched or newer run does not pay for a server it cannot use,
and a newer push replaces the old candidate (and never a release).

Three things, each red on the workflow as it was:

  * the job `checks`: asset names, declared upstream source, Juggler's tests without a browser and the release scripts' known-bad inputs. They used to run in
    the build job, on the node, minutes after it had been ordered (run 43156862361: 7.9 minutes of a paid hour for «Juggler's tests without a browser»).
    The node needs `checks` as well as `plan`;
  * a `concurrency` group per branch, whose newer candidate cancels the older, and a group of their own for a release and a re-launch, so that no push cancels
    a release or the run a re-launch replaces (republish.yml has none);
  * the guard `scripts/ci/still_head.py`, last step of `checks`: a candidate that was queued behind a newer push is cancelled before the node starts.

    python3 scripts/ci/test_release_checks.py
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import datetime as dt  # noqa: E402

import gh_expr  # noqa: E402
import reuse_run  # noqa: E402
import still_head  # noqa: E402

WORKFLOWS = HERE.parent.parent / ".github" / "workflows"
CHECKS = ("Check asset names match the version this tree builds",
          "Check the declared upstream source matches the version this tree builds",
          "Juggler's tests without a browser",
          "The release scripts refuse what they must")
SHA = "a" * 40
NEWER = "b" * 40


def load(name):
    import yaml

    return yaml.safe_load((WORKFLOWS / name).read_text(encoding="utf-8"))


def group_of(doc, inputs, ref="refs/heads/main", run_id=7):
    """The concurrency group and cancel-in-progress GitHub gives a run, by the workflow's own expressions."""
    context = {"inputs": inputs, "github": {"ref": ref, "run_id": run_id}}
    block = doc["concurrency"]
    return gh_expr.render(block["group"], context), gh_expr.evaluate(block["cancel-in-progress"].strip()[3:-2].strip(), context)


def _cases() -> list:
    out = []
    doc = load("release.yml")
    jobs = doc["jobs"]
    checks, node, build = jobs["checks"], jobs["node"], jobs["build"]
    names = [s.get("name") for s in checks["steps"]]

    # --- the job `checks` -------------------------------------------------------------------------------------------------------------------------
    out.append(("checks: a job of its own, in parallel with the plan (it needs nothing), that the node waits for besides the plan",
                "needs" not in checks and node["needs"] == ["plan", "checks"] and build["needs"] == ["plan", "node"]))
    out.append(("checks: the four checks that do not read the binary are in it, in this order, none conditioned on a leg",
                [n for n in names if n in CHECKS] == list(CHECKS) and not any("matrix" in str(s.get("if", "")) for s in checks["steps"])))
    out.append(("checks: none of them is left in the build job, on the node",
                not any(s.get("name") in CHECKS for s in build["steps"])))
    text = " ".join(s.get("run", "") for s in checks["steps"])
    out.append(("checks: what they run is what the build used to run",
                "scripts/check_release_naming.py" in text and "check_upstream_source.py --online" in text and "node --test juggler/tests/*.test.mjs" in text
                and "validate_release.py --selftest" in text and "test_make_seal.py" in text and "test_workflow_cache_rules.py" in text))
    out.append(("checks: the tree is materialized from the series before they run, on a GitHub runner",
                "engine.py materialize --into ." in checks["steps"][1]["run"] and checks["runs-on"] == "ubuntu-24.04" and names.index(CHECKS[0]) > 1))
    out.append(("checks: no secret and no environment (the node job alone has them), and a token that may cancel its own run and read",
                "secrets." not in str(checks) and "environment" not in checks and checks["permissions"] == {"actions": "write", "contents": "read"}))
    out.append(("checks: it has a limit", 0 < checks.get("timeout-minutes", 0) <= 30))
    out.append(("checks: the guard that a newer push has not replaced the run is its LAST step, the one that can cancel",
                names[-1] == "Still the head of the branch" and "gh run cancel" in checks["steps"][-1]["run"]
                and "sleep 240" in checks["steps"][-1]["run"] and "still_head.py" in checks["steps"][-1]["run"]))
    guard = checks["steps"][-1]
    out.append(("checks: the guard hands over the event, the ref, the commit, the kind and the re-launch, as the plan reads them",
                guard["env"]["EVENT"] == "${{ github.event_name }}" and guard["env"]["REF"] == "${{ github.ref }}" and guard["env"]["SHA"] == "${{ github.sha }}"
                and guard["env"]["KIND"] == "${{ inputs.kind }}" and guard["env"]["RELAUNCH_OF"] == "${{ inputs.relaunch_of }}"))
    run = guard["run"]
    out.append(("checks: only an exit of 1 cancels; the head that cannot be read (2) goes on",
                '[ "$CODE" = 1 ]' in run and run.rstrip().endswith("exit 0") and "set +e" in run))
    out.append(("node: still turned on only by the plan's attempt, and the permissions and steps the other tests hold it to are untouched",
                node["if"].startswith("needs.plan.outputs.attempt == github.run_attempt") and node["permissions"] == {"actions": "read", "contents": "read"}
                and not any("if" in s for s in node["steps"])))

    # --- the concurrency --------------------------------------------------------------------------------------------------------------------------
    push = group_of(doc, {})
    candidate = group_of(doc, {"kind": "candidate"})
    other = group_of(doc, {"kind": "candidate"}, run_id=8)
    elsewhere = group_of(doc, {"kind": "candidate"}, ref="refs/heads/latitude/x", run_id=9)
    release = group_of(doc, {"kind": "release"}, run_id=10)
    release2 = group_of(doc, {"kind": "release"}, run_id=11)
    relaunch = group_of(doc, {"kind": "candidate", "relaunch_of": "123"}, run_id=12)
    out.append(("concurrency: a push and a dispatch of a candidate on one branch share a group, and a newer run cancels the older one in it",
                push[0] == candidate[0] == other[0] == "candidate-refs/heads/main" and push[1] is True and candidate[1] is True and other[1] is True))
    out.append(("concurrency: another branch is another group", elsewhere[0] == "candidate-refs/heads/latitude/x" and elsewhere[0] != push[0]))
    out.append(("concurrency: a release has a group of its own, per run, and is never cancelled by a push; two releases do not share one",
                release[0] == "release-10" and release2[0] == "release-11" and release[1] is False and release[0] != push[0]))
    out.append(("concurrency: a re-launch has a group of its own too: it does not cancel the run it replaces, which is still reporting, and is not cancelled",
                relaunch[0] == "relaunch-12" and relaunch[1] is False))
    out.append(("concurrency: republish.yml, which publishes a run's archives, has none: no push touches it", "concurrency" not in load("republish.yml")))
    out.append(("concurrency: no other workflow of the build got one by mistake",
                all("concurrency" not in load(n) for n in ("gate.yml", "publish.yml"))))

    # --- the guard --------------------------------------------------------------------------------------------------------------------------------
    def head(value):
        return lambda repository, ref: value

    def judged(event="push", ref="refs/heads/main", sha=SHA, kind="", relaunch="", now=SHA, files=("dom/base/Foo.cpp",)):
        return still_head.judge(event, ref, sha, kind, relaunch, "o/r", head=head(now), changed=lambda repository, base, tip: None if files is None else list(files))

    out.append(("guard: the head is the commit the run builds: go on", judged()[0] == 0))
    code, said = judged(now=NEWER)
    out.append(("guard: a newer commit is the head: superseded, and it says which", code == 1 and NEWER[:12] in said and SHA[:12] in said))
    out.append(("guard: the weekly run and a dispatch of a candidate are judged the same",
                judged(event="schedule", now=NEWER)[0] == 1 and judged(event="workflow_dispatch", kind="candidate", now=NEWER)[0] == 1))
    out.append(("guard: a RELEASE is never judged, whatever the branch did after", judged(event="workflow_dispatch", kind="release", now=NEWER)[0] == 0))
    out.append(("guard: a re-launch is never judged", judged(event="workflow_dispatch", relaunch="123", now=NEWER)[0] == 0))
    out.append(("guard: a tag is not a branch: not judged", judged(ref="refs/tags/firefox-63", now=NEWER)[0] == 0))
    out.append(("guard: a branch whose pushes start no candidate (a latitude/<name> proof) is not judged: nobody would replace the run",
                judged(event="workflow_dispatch", kind="candidate", ref="refs/heads/latitude/proof", now=NEWER)[0] == 0))
    out.append(("guard: a newer head that touches only Markdown starts no candidate (paths-ignore): this run stays, it is not cancelled for nothing",
                judged(now=NEWER, files=("README.md", "docs/a.MD"))[0] == 0 and "Markdown" in judged(now=NEWER, files=("README.md",))[1]))
    out.append(("guard: a newer head with ONE other path among Markdown ones replaces the run",
                judged(now=NEWER, files=("README.md", "dom/base/Foo.cpp"))[0] == 1
                and judged(now=NEWER, files=("a.md", "b.md", "scripts/ci/x.py"))[0] == 1))
    out.append(("guard: when the paths of the newer head cannot be read the run goes on (2), and an empty list is not Markdown only",
                judged(now=NEWER, files=None)[0] == 2 and judged(now=NEWER, files=())[0] == 1))
    out.append(("guard: a compare of 300 files is never Markdown only",
                still_head.changed_files("o/r", SHA, NEWER, run=lambda cmd, **kw: type("D", (), {"returncode": 0, "stdout": '{"files": [%s]}' % ",".join(
                    '{"filename": "f%d.md"}' % i for i in range(300))})())[-1].startswith("<more than")))
    out.append(("guard: the compare API's answer is read for the file names; a failure, or a broken answer, is None",
                still_head.changed_files("o/r", SHA, NEWER, run=lambda cmd, **kw: type("D", (), {"returncode": 0, "stdout": '{"files": [{"filename": "a.md"}]}'})()) == ["a.md"]
                and still_head.changed_files("o/r", SHA, NEWER, run=lambda cmd, **kw: type("D", (), {"returncode": 1, "stdout": ""})()) is None
                and still_head.changed_files("o/r", SHA, NEWER, run=lambda cmd, **kw: type("D", (), {"returncode": 0, "stdout": "not json"})()) is None))
    out.append(("guard: a head that cannot be read is 2 and the run goes on", judged(now="")[0] == 2))
    asked = []
    still_head.judge("workflow_dispatch", "refs/heads/main", SHA, "release", "", "o/r", head=lambda r, f: asked.append(f) or NEWER)
    out.append(("guard: what it does not judge it does not ask the repository about", asked == []))

    import subprocess

    def hang(cmd, **kw):
        raise subprocess.TimeoutExpired(cmd, 60)

    out.append(("guard: a `git ls-remote` that hangs past its timeout is a head that cannot be read (""), never a reason to cancel",
                still_head.head_of("o/r", "refs/heads/main", run=hang) == ""
                and still_head.judge("push", "refs/heads/main", SHA, "candidate", "", "o/r", head=lambda r, f: still_head.head_of(r, f, run=hang))[0] == 2))
    import contextlib
    import io

    def guard_main(**patches):
        saved = {k: getattr(still_head, k) for k in patches}
        for k, v in patches.items():
            setattr(still_head, k, v)
        buffer = io.StringIO()
        try:
            with contextlib.redirect_stdout(buffer):
                return still_head.main(["--event", "push", "--ref", "refs/heads/main", "--sha", SHA, "--kind", "candidate", "--relaunch-of", "", "--repository", "o/r"])
        finally:
            for k, v in saved.items():
                setattr(still_head, k, v)

    def boom(*args, **kw):
        raise RuntimeError("anything")

    out.append(("guard: main() never turns an unexpected failure into exit 1 (which cancels the run): it is 2", guard_main(judge=boom) == 2))

    class Done:
        def __init__(self, code, stdout):
            self.returncode, self.stdout = code, stdout

    ref = "refs/heads/main"
    out.append(("guard: the head is read from `git ls-remote` of the public repository, the line of the ref and no other",
                still_head.head_of("o/r", ref, run=lambda cmd, **kw: Done(0, "%s\trefs/heads/maintenance\n%s\t%s\n" % (NEWER, SHA, ref))) == SHA
                and still_head.head_of("o/r", ref, run=lambda cmd, **kw: Done(128, "")) == ""
                and still_head.head_of("o/r", ref, run=lambda cmd, **kw: Done(0, "")) == ""))
    out += _reuse_cases(doc)
    return out


def _reuse_cases(doc) -> list:
    """A push to main whose tree a green run has already built does not turn a node on: the gate runs again on those bytes."""
    out = []
    jobs = doc["jobs"]
    plan_names = [s.get("name") for s in jobs["plan"]["steps"]]
    step = next(s for s in jobs["plan"]["steps"] if s.get("id") == "reuse")
    out.append(("reuse: the plan looks for a green run right after it has decided the kind, and hands the id on",
                plan_names.index(step["name"]) == plan_names.index("Plan") + 1 and jobs["plan"]["outputs"]["reuse_run_id"] == "${{ steps.reuse.outputs.reuse_run_id }}"
                and jobs["plan"]["permissions"] == {"actions": "read", "contents": "read"}))
    out.append(("reuse: it is told the event, the ref, the kind and the gtests the plan decided, and the tree object and the commit it materialized",
                step["env"]["KIND"] == "${{ steps.plan.outputs.kind }}" and step["env"]["GTESTS"] == "${{ steps.plan.outputs.gtests }}"
                and step["env"]["EVENT"] == "${{ github.event_name }}" and step["env"]["REF"] == "${{ github.ref }}"
                and "HEAD^{tree}" in step["run"] and "rev-parse HEAD" in step["run"] and '"$GITHUB_OUTPUT"' in step["run"]))
    out.append(("reuse: no node when a run is reused, and nothing else about the node's condition changed",
                jobs["node"]["if"] == "needs.plan.outputs.attempt == github.run_attempt && needs.plan.outputs.reuse_run_id == ''"))
    reuse_gate = jobs["gate-reuse"]
    out.append(("reuse: the gate runs again on the reused run's archives with the refs of today, and only when there is one",
                reuse_gate["if"] == "needs.plan.outputs.reuse_run_id != ''" and reuse_gate["uses"] == "./.github/workflows/gate.yml"
                and reuse_gate["with"]["build_run_id"] == "${{ needs.plan.outputs.reuse_run_id }}"
                and reuse_gate["with"]["wrapper_ref"] == "${{ needs.plan.outputs.wrapper_ref }}" and reuse_gate["with"]["core_ref"] == "${{ needs.plan.outputs.core_ref }}"
                and reuse_gate["with"]["seal_tag"] == "${{ needs.plan.outputs.seal_tag }}" and reuse_gate["needs"] == "plan"))
    out.append(("reuse: the ordinary gate still waits for the build (a run that built is judged on what it built)",
                jobs["gate"]["needs"] == ["plan", "build"] and jobs["gate"]["with"]["build_run_id"] == "${{ github.run_id }}"))
    out.append(("reuse: the report of the legs' events does not run for a run that built nothing", jobs["report"]["if"] == "always() && needs.build.result != 'skipped'"))
    record = next(x for x in jobs["build"]["steps"] if x.get("name") == "Record source commit (for auto release notes)")
    upload = next(x for x in jobs["build"]["steps"] if x.get("name") == "Upload source-commit artifact")
    out.append(("reuse: the build writes the TREE beside the commit, in the artifact a later run reads",
                "HEAD^{tree}" in record["run"] and "source-tree.txt" in record["run"] and "source-tree.txt" in upload["with"]["path"]
                and "source-commit.txt" in upload["with"]["path"] and "series-commit.txt" in upload["with"]["path"]))

    # --- scripts/ci/reuse_run.py ------------------------------------------------------------------------------------------------------------------
    now = dt.datetime(2026, 11, 12, 15, 0, tzinfo=dt.timezone.utc)
    TREE, COMMIT = "t" * 40, "c" * 40
    FILTER = "StealthA.*:StealthB.*"
    full = ["asset-linux-x86_64", "asset-win-x86_64", "source-commit", "gtest-reports-linux-x86_64", "gtest-reports-win-x86_64"]

    def iso(days):
        return (now - dt.timedelta(days=days)).strftime("%Y-%m-%dT%H:%M:%SZ")

    def world(runs, arts=None, sources=None):
        """runs: [(id, days old)]; arts: {id: [names]} (a name with a leading ! is expired); sources: {id: {'commit': ..., 'tree': ...}}."""
        arts, sources = arts or {}, sources or {}
        run_list = [{"id": i, "created_at": iso(d), "event": "push", "head_branch": "main"} for i, d in runs]

        def artifacts_of(run_id):
            return [{"id": run_id * 10, "name": n.lstrip("!"), "expired": n.startswith("!")} for n in arts.get(run_id, full)]

        return run_list, artifacts_of, lambda artifact_id: sources.get(artifact_id // 10, {"commit": COMMIT, "tree": TREE, "gtest_filter": FILTER})

    def pick(runs, arts=None, sources=None, gtests=True, current=99, wanted=FILTER, counted=None):
        """`counted`: {artifact id // 10: tests counted in its reports}; by default every report holds 30 tests."""
        run_list, artifacts_of, source_of = world(runs, arts, sources)
        counted = counted or {}
        return reuse_run.choose(run_list, artifacts_of, source_of, TREE, COMMIT, now, current, gtests, gtest_filter=wanted,
                                tests_of=lambda artifact_id: counted.get(artifact_id // 10, 30))[0]

    other = {"commit": "x" * 40, "tree": "y" * 40, "gtest_filter": FILTER}
    out.append(("reuse_run: the newest green run with the same tree, its archives alive and its gtests done, is the one", pick([(5, 1), (4, 2)]) == "5"))
    out.append(("reuse_run: it is never the run that asks", pick([(99, 0), (4, 2)]) == "4"))
    out.append(("reuse_run: another tree is not reused", pick([(5, 1)], sources={5: other}) == ""))
    out.append(("reuse_run: the tree is the identity, not the commit: a run with another commit and the same tree is reused",
                pick([(5, 1)], sources={5: {"commit": "x" * 40, "tree": TREE, "gtest_filter": FILTER}}) == "5"))
    out.append(("reuse_run: a run from before the tree was written is compared by commit",
                pick([(5, 1)], sources={5: {"commit": COMMIT, "tree": "", "gtest_filter": FILTER}}) == "5"
                and pick([(5, 1)], sources={5: {"commit": "x" * 40, "tree": "", "gtest_filter": FILTER}}) == ""))
    out.append(("reuse_run: an expired archive is not reused, the next run is looked at",
                pick([(5, 1), (4, 2)], arts={5: ["!asset-linux-x86_64"] + full[1:]}) == "4"
                and pick([(5, 1)], arts={5: ["asset-linux-x86_64", "!asset-win-x86_64", "source-commit", "gtest-reports-linux-x86_64"]}) == ""))
    out.append(("reuse_run: a run that lacks an archive or the source record is not reused",
                pick([(5, 1)], arts={5: ["asset-linux-x86_64", "source-commit", "gtest-reports-linux-x86_64"]}) == ""
                and pick([(5, 1)], arts={5: ["asset-linux-x86_64", "asset-win-x86_64", "gtest-reports-linux-x86_64"]}) == ""))
    out.append(("reuse_run: when this run would run the gtests, the reused one must have run them, on BOTH legs",
                pick([(5, 1)], arts={5: full[:3]}) == "" and pick([(5, 1)], arts={5: full[:3]}, gtests=False) == "5"
                and pick([(5, 1)], arts={5: full[:4]}) == "" and pick([(5, 1)], arts={5: full[:3] + [full[4]]}) == ""))
    # --- which gtests the reused run ran (the proof candidate with a narrow filter that was fast-forwarded to main) ---------------------------------
    narrow = {"commit": COMMIT, "tree": TREE, "gtest_filter": "StealthA.*"}
    out.append(("reuse_run: a run whose gtests were a NARROWER filter than this run's is not reused, the next run is looked at",
                pick([(5, 1)], sources={5: narrow}) == "" and pick([(5, 1), (4, 2)], sources={5: narrow}) == "4"))
    out.append(("reuse_run: the same filter, a longer one that contains it, and `*` are reused; the order of the patterns does not matter",
                pick([(5, 1)], sources={5: {"commit": COMMIT, "tree": TREE, "gtest_filter": "StealthB.*:StealthA.*"}}) == "5"
                and pick([(5, 1)], sources={5: {"commit": COMMIT, "tree": TREE, "gtest_filter": FILTER + ":StealthC.*"}}) == "5"
                and pick([(5, 1)], sources={5: {"commit": COMMIT, "tree": TREE, "gtest_filter": "*"}}) == "5"))
    out.append(("reuse_run: a run with no record of its filter (a `source-commit` from before the file) is not reused",
                pick([(5, 1)], sources={5: {"commit": COMMIT, "tree": TREE, "gtest_filter": None}}) == ""
                and pick([(5, 1)], sources={5: {"commit": COMMIT, "tree": TREE}}) == ""
                and pick([(5, 1)], sources={5: {"commit": COMMIT, "tree": TREE, "gtest_filter": None}}, gtests=False) == ""))
    out.append(("reuse_run: a filter with an exclusion is covered only by the identical text",
                pick([(5, 1)], sources={5: {"commit": COMMIT, "tree": TREE, "gtest_filter": "*-StealthA.*"}}) == ""
                and pick([(5, 1)], sources={5: {"commit": COMMIT, "tree": TREE, "gtest_filter": "*-StealthA.*"}}, wanted="*-StealthA.*") == "5"))
    out.append(("reuse_run: reports that counted no test are not gtests that ran",
                pick([(5, 1)], counted={5: 0}) == "" and pick([(5, 1), (4, 2)], counted={5: 0}) == "4"))
    out.append(("reuse_run: covers(): a pattern of the wanted filter missing from the previous one is not covered, an empty side never is",
                reuse_run.covers("A.*:B.*", "A.*") and not reuse_run.covers("A.*", "A.*:B.*") and not reuse_run.covers("", "A.*")
                and not reuse_run.covers("A.*", "") and reuse_run.covers("*", "A.*:B.*")))
    out.append(("reuse_run: older than six days is not reused, whatever its artifacts (they are kept seven, the gate has to download them)",
                pick([(5, 6.5)]) == "" and pick([(5, 5.5)]) == "5"))
    asked = []
    old_run = [{"id": 1, "created_at": iso(7), "event": "push", "head_branch": "main"}]
    out.append(("reuse_run: it looks at nothing it does not need: past the age it stops and reads nothing",
                reuse_run.choose(old_run, lambda r: asked.append(r) or [], lambda a: asked.append(a) or {}, TREE, COMMIT, now, 99, True)[0] == "" and asked == []))
    out.append(("reuse_run: only the push to main of a candidate is judged: the weekly run, a dispatch, a release, a tag, another branch are not",
                reuse_run.applies("push", "refs/heads/main", "candidate") and not any(
                    reuse_run.applies(e, r, k) for e, r, k in (("schedule", "refs/heads/main", "candidate"), ("workflow_dispatch", "refs/heads/main", "candidate"),
                                                               ("push", "refs/heads/main", "release"), ("push", "refs/tags/firefox-63", "candidate"),
                                                               ("push", "refs/heads/latitude/x", "candidate")))))

    class Fake:
        def __init__(self, runs, fail=False):
            self.runs, self.fail, self.calls = runs, fail, 0

        def green_runs(self):
            self.calls += 1
            if self.fail:
                raise OSError("api down")
            return self.runs

        def artifacts(self, run_id):
            return [{"id": run_id * 10, "name": n, "expired": False} for n in full]

        def source(self, artifact_id):
            return {"commit": COMMIT, "tree": TREE, "gtest_filter": FILTER}

        def tests_counted(self, artifact_id):
            return 30

    import contextlib
    import io

    def run_main(event, fake, ref="refs/heads/main", kind="candidate"):
        buffer, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(buffer), contextlib.redirect_stderr(err):
            code = reuse_run.main(["--event", event, "--ref", ref, "--kind", kind, "--gtests", "true", "--gtest-filter", FILTER, "--repository", "o/r",
                                   "--run-id", "99", "--tree", TREE, "--commit", COMMIT], github=fake, now=now)
        return code, buffer.getvalue().strip()

    runs = [{"id": 5, "created_at": iso(1), "event": "push", "head_branch": "main"}]
    out.append(("reuse_run: main() prints the id for the output file", run_main("push", Fake(runs)) == (0, "reuse_run_id=5")))
    schedule = Fake(runs)
    out.append(("reuse_run: the weekly run prints an empty id and does not even ask GitHub", run_main("schedule", schedule) == (0, "reuse_run_id=") and schedule.calls == 0))
    out.append(("reuse_run: a GitHub that does not answer is a run that builds, never a failure of the plan", run_main("push", Fake(runs, fail=True)) == (0, "reuse_run_id=")))

    class Slow(Fake):
        def green_runs(self):
            import subprocess
            raise subprocess.TimeoutExpired("gh api", 120)

    out.append(("reuse_run: a `gh api` that times out is a run that builds too, not a red plan", run_main("push", Slow(runs)) == (0, "reuse_run_id=")))
    out.append(("reuse_run: main() hands the filter of the plan to the choice: a run of a narrower filter is not reused through main() either",
                run_main("push", type("Narrow", (Fake,), {"source": lambda self, i: {"commit": COMMIT, "tree": TREE, "gtest_filter": "StealthA.*"}})(runs)) == (0, "reuse_run_id=")))
    # --- the workflow records and passes the filter ------------------------------------------------------------------------------------------------
    out.append(("reuse: the plan passes the plan's gtest filter to reuse_run.py", step["env"].get("GTEST_FILTER") == "${{ steps.plan.outputs.gtest_filter }}"
                and '--gtest-filter "$GTEST_FILTER"' in step["run"]))
    out.append(("reuse: the build records the filter it ran (empty without gtests) in the artifact a later run reads",
                record.get("env", {}).get("GTEST_FILTER") == "${{ needs.plan.outputs.gtest_filter }}" and record["env"].get("GTESTS") == "${{ needs.plan.outputs.gtests }}"
                and "gtest-filter.txt" in record["run"] and "gtest-filter.txt" in upload["with"]["path"]))
    return out


def cases() -> list:
    """The cases; a workflow that does not have what they read is a FAIL that names it, not a crash of the caller."""
    try:
        return _cases()
    except (KeyError, IndexError, TypeError, AttributeError, StopIteration) as exc:
        return [("release checks: stopped on %s: %s (the workflow lacks what these cases read)" % (type(exc).__name__, exc), False)]


def main() -> int:
    failures = 0
    found = cases()
    for name, ok in found:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(found) - failures, len(found)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
