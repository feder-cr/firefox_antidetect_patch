#!/usr/bin/env python3
"""release.yml's build and release path, with its expressions evaluated (gh_expr.py).

  1. a red or cancelled gtest stops the gate and the publish; gtests=false does not;
  2. the Linux gtests run as `mach gtest` ran them on the green candidate;
  3. lavapipe's job count is the build's;
  4. the key of the sccache binary tells the node from GitHub's runners;
  5. what a cancel, a failure and a build that never started do to the cache's saves;
  6. a re-run of failed jobs says plainly what it cannot do, and a full re-run works;
  7. only the sccache server logs, and only the names leave the node;
  8. xz's threads on the node, the linker's huge pages and threads, materialize's index;
  9. on the node, what needs no result runs beside what takes long, and its failure still
     fails the leg: background.py with real processes, the steps' order from the file.

    python3 scripts/ci/test_release_path.py
"""
from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import background  # noqa: E402
import gh_expr as e  # noqa: E402
import gtest_run  # noqa: E402
import legs  # noqa: E402
import linker_shim  # noqa: E402

ROOT = HERE.parent.parent
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
# The filter of run 41402916724, from its log.
GREEN_FILTER = ("StealthGfxFeatureStatus.*:StealthWebGPUAdapter.*:StealthWebGPUDeclaration.*:StealthWebGPUBundledVulkan.*:"
                "StealthWebGPUInstanceEnv.*:StealthFullscreen.*:StealthScaleDeclaration.*:StealthWindowGeometry.*:StealthGLMultisample.*")
GTEST_JOBS = ("gtest-linux", "gtest-cross")
# configure's files, for which options exist; the ones the tree has at the plan's stage too.
CONFIGURE_FILES = ("moz.configure", "toolkit/moz.configure", "js/moz.configure", "browser/moz.configure", "build/moz.configure/*.configure")


def context(gtests="true", attempt="1", gtest_filter=GREEN_FILTER, environment="self-hosted", node=True, leg="linux-x86_64"):
    # The gtests and the filter come from the plan's outputs (an automatic run has no
    # inputs); `inputs` stays for what only a run by hand carries.
    return {"github": {"event": {"inputs": {"gtests": gtests, "gtest_filter": gtest_filter}}, "run_attempt": attempt, "run_id": "7"},
            "inputs": {"gtest_filter": gtest_filter}, "runner": {"environment": environment, "os": "Linux", "arch": "X64"},
            "needs": {"plan": {"outputs": {"gtests": gtests, "gtest_filter": gtest_filter}}},
            "matrix": {"leg": leg, "node": node}}


def outcome(jobs, forced, gtests="true", kind="release", cancelled=False, attempt="1", plan_attempt="1"):
    outputs = {"plan": {"kind": kind, "attempt": plan_attempt, "node_label": "latitude-7-%s" % plan_attempt,
                        "gtests": gtests, "gtest_filter": GREEN_FILTER}}
    base = {"plan": "success", "node": "success", "build": "success"}
    return e.jobs_after(jobs, dict(base, **forced), context(gtests, attempt), outputs, cancelled)


def group_expressions() -> list:
    ev = e.evaluate
    return [("expressions: '' is falsy, so `cond && '' || 'x'` is x whatever cond is",
             ev("true && '' || 'x'", {}) == "x" and ev("false && '' || 'x'", {}) == "x"),
            ("expressions: && and || give an operand, == ignores case, a missing value is null",
             ev("'a' && 'b'", {}) == "b" and ev("'' || 'b'", {}) == "b" and ev("'ABC' == 'abc'", {}) is True
             and ev("a.b == null", {}) is True and ev("!a.b", {}) is True),
            ("expressions: a boolean is not the string 'true'", ev("a == 'true'", {"a": True}) is False),
            ("expressions: a job without a status function has the implicit success()",
             e.condition("x == 1") == "success() && (x == 1)" and e.condition("always() && x") == "always() && x"
             and e.condition(None) == "success()")]


def group_gate(doc) -> list:
    jobs = doc["jobs"]
    out = []
    green = outcome(jobs, {})
    out.append(("gate: all green, the gate and the publish run", green["gate"] == "success" and green["publish"] == "success"))
    for job in GTEST_JOBS:
        for result in ("failure", "cancelled"):
            got = outcome(jobs, {job: result})
            out.append(("gate: %s %s stops the gate and the publish" % (job, result), got["gate"] == "skipped" and got["publish"] == "skipped"))
    off = outcome(jobs, {}, gtests="false")
    out.append(("gate: gtests=false stops neither", off["gate"] == "success" and off["publish"] == "success"))
    out.append(("gate: gtests=false runs no gtest step, and the jobs are there for the gate to need",
                all(off[j] == "success" and not any(e.step_runs(s, context("false")) for s in jobs[j]["steps"]) for j in GTEST_JOBS)))
    out.append(("gate: gtests=true runs every gtest step", all(all(e.step_runs(s, context("true")) for s in jobs[j]["steps"]) for j in GTEST_JOBS)))
    out.append(("gate: no gtest job can skip by itself: a skip reaches every job below it (actions/runner#2205)",
                all("if" not in jobs[j] and "continue-on-error" not in jobs[j] for j in GTEST_JOBS)))
    broken = outcome(jobs, {"build": "failure"})
    out.append(("gate: a failed build stops the gtests, the gate and the publish",
                broken["gtest-linux"] == "skipped" and broken["gate"] == "skipped" and broken["publish"] == "skipped"))
    red_gate = outcome(jobs, {"gate": "failure"})
    out.append(("gate: a red gate stops the publish", red_gate["publish"] == "skipped"))
    out.append(("gate: the graph: publish needs gate, gate needs build and both gtest jobs, each gtest job needs the plan and the build",
                "gate" in jobs["publish"]["needs"] and "build" in jobs["gate"]["needs"]
                and all(j in jobs["gate"]["needs"] for j in GTEST_JOBS) and all(jobs[j]["needs"] == ["plan", "build"] for j in GTEST_JOBS)))
    out.append(("gate: neither has an `if` that would run it past what it needs",
                "if" not in jobs["gate"] and not e.STATUS.search(str(jobs["publish"].get("if", "")))))
    candidate = outcome(jobs, {}, kind="candidate")
    out.append(("gate: a candidate gates and publishes nothing", candidate["gate"] == "success" and candidate["publish"] == "skipped"))
    return out


def fake_firefox(path: Path, fail_suite="", record=None):
    """As much of `firefox -unittest` as gtest_run.py meets."""
    path.write_text("""#!/usr/bin/env python3
import json, os, sys
env = os.environ
if "--gtest_list_tests" in sys.argv:
    wanted = env["GTEST_FILTER"]
    print("*** You are running in headless mode.")
    for suite in env.get("FAKE_SUITES", "").split():
        print(suite + ".")
        print("  One")
    sys.exit(0)
with open(%r, "a") as f:
    f.write(json.dumps({"argv": sys.argv[1:], "cwd": os.getcwd(), "env": dict(env)}) + "\\n")
suite = env["GTEST_FILTER"]
with open(env["GTEST_OUTPUT"][5:], "w") as f:
    json.dump({"tests": 1, "failures": 0, "disabled": 0, "errors": 0, "testsuites": []}, f)
sys.exit(3 if env.get("FAKE_FAIL") and suite.startswith(env["FAKE_FAIL"]) else 0)
""" % str(record), encoding="utf-8", newline="\n")
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


def group_gtest(doc) -> list:
    jobs = doc["jobs"]
    out = []
    inputs = doc[True]["workflow_dispatch"]["inputs"]
    out.append(("gtests: the default filter is what the green candidate ran", inputs["gtest_filter"]["default"] == GREEN_FILTER))
    values = []
    build_steps = {s.get("name", ""): s for s in jobs["build"]["steps"]}
    for job, steps in (("gtest-cross", jobs["gtest-cross"]["steps"]), ("gtest-linux", jobs["gtest-linux"]["steps"]),
                       ("build", jobs["build"]["steps"])):
        for s in steps:
            for key, value in s.get("env", {}).items():
                if key.startswith("GTEST_FILTER"):
                    values.append((job, str(value)))
    out.append(("gtests: an empty filter never turns into *: no job falls back on its own (%d read it)" % len(values),
                len(values) == 3 and all(e.render(v, context(gtest_filter="")) == "" for _, v in values)
                and all(e.render(v, context()) == GREEN_FILTER for _, v in values)))
    linux = jobs["gtest-linux"]
    steps = {s.get("name", ""): s for s in linux["steps"]}
    run = steps["Engine gtests (gtests=true only)"]["run"]
    out.append(("gtests: Linux has the old step's limit, 180, which held the link too", linux["timeout-minutes"] == 180))
    for job, leg in (("gtest-linux", "linux-x86_64"), ("gtest-cross", "win-x86_64")):
        reports = next((s for s in jobs[job]["steps"] if s.get("name") == "What each suite did"), {})
        out.append(("gtests: %s keeps each suite's report as an artifact, also when it is red" % job,
                    reports.get("with", {}).get("name") == "gtest-reports-%s" % leg and "always()" in reports.get("if", "")
                    and any("--reports gtest-reports" in s.get("run", "") and "gtest_run.py" in s.get("run", "") for s in jobs[job]["steps"])))
    window = {s.get("name", ""): s for s in jobs["gtest-cross"]["steps"]}
    out.append(("gtests: Windows runs gtest_run.py, from the series' copy", "gtest_run.py --root gtest-run" in window["Engine gtests (gtests=true only)"]["run"]
                and "actions/checkout" in window["The series (this repository, this commit)"]["uses"]))
    script = re.search(r"\.series/platform/ci/src/scripts/ci/(\S+)", run)
    out.append(("gtests: Linux runs gtest_run.py under a display, from the series' copy, which needs no tree",
                "xvfb-run -a python3 " in run and script and (HERE / script.group(1)).is_file()
                and "actions/checkout" in steps["The series (this repository, this commit)"]["uses"]))
    names = list(steps)
    apt = steps["apt - network timeouts and retries"]
    display = steps["A display"]["run"]
    out.append(("gtests: the protected apt path: the same timeouts, an update, a limit, before the gtests",
                apt["run"].strip() == build_steps["apt - network timeouts and retries"]["run"].strip()
                and display.index("apt-get update") < display.index("apt-get install -y xvfb")
                and steps["A display"]["timeout-minutes"] == 20
                and names.index("apt - network timeouts and retries") < names.index("A display") < names.index("Engine gtests (gtests=true only)")))
    with tempfile.TemporaryDirectory() as tmp:
        tree = Path(tmp)
        (tree / "obj-rel" / "dist" / "bin").mkdir(parents=True)
        (tree / "obj-rel" / "dist" / "bin" / "firefox").write_text("x")
        make = build_steps["The node - its gtest binaries, for a job on GitHub's runners"]["run"].splitlines()
        lines = [l for l in make if l.strip() and "phase gtest" not in l and not l.strip().startswith("#")]
        done = subprocess.run(["bash", "-ec", "\n".join(lines)], cwd=tree, capture_output=True, text=True)
        listed = subprocess.run(["tar", "-tf", str(tree / "gtest-bin.tar")], capture_output=True, text=True).stdout.split()
        out.append(("gtests: the node's tar holds bin and the directory mach ran them from, also when that is empty (%s)" % done.stderr[:80],
                    done.returncode == 0 and "bin/firefox" in listed and "gtest/" in listed))

        root = tree / "run"
        (root / "bin").mkdir(parents=True)
        record = tree / "calls.jsonl"
        fake_firefox(root / "bin" / "firefox", record=record)

        def go(gtest_filter, suites="SuiteA SuiteB", fail=""):
            record.write_text("")
            old = dict(os.environ)
            os.environ.update(FAKE_SUITES=suites, FAKE_FAIL=fail)
            said = []
            try:
                code = gtest_run.run(root, gtest_filter, out=said.append)
            finally:
                os.environ.clear()
                os.environ.update(old)
            return code, [json.loads(l) for l in record.read_text().splitlines()], said

        code, calls, said = go("Suite*")
        env = calls[0]["env"] if calls else {}
        out.append(("gtests: one process per suite, one after the other, as mach's -unittest and no more",
                    code == 0 and len(calls) == 2 and all(c["argv"] == ["-unittest"] for c in calls)))
        bin_dir = str(root / "bin")
        out.append(("gtests: what mach sets: MOZ_XRE_DIR, MOZ_GMP_PATH, MOZ_RUN_GTEST, PYTHON, MOZ_WEBRENDER=0, MOZ_ENABLE_INC_ORIGIN_INIT=0",
                    env.get("MOZ_XRE_DIR") == bin_dir and env.get("MOZ_RUN_GTEST") == "True" and env.get("MOZ_WEBRENDER") == "0"
                    and env.get("MOZ_ENABLE_INC_ORIGIN_INIT") == "0" and env.get("PYTHON") == sys.executable
                    and env.get("MOZ_GMP_PATH") == os.pathsep.join(os.path.join(bin_dir, p, "1.0") for p in ("gmp-fake", "gmp-fakeopenh264"))))
        out.append(("gtests: from the directory mach runs them from", all(c["cwd"] == str(root / "gtest") for c in calls)))
        out.append(("gtests: each process has the filter narrowed to its suite", [c["env"]["GTEST_FILTER"] for c in calls]
                    == ["Suite*-SuiteB.*", "Suite*-SuiteA.*"]))
        out.append(("gtests: what ran is said", any("Ran 2 test(s) from 2 test suite(s)" in l for l in said)))
        code, calls, said = go("Suite*", fail="Suite*-SuiteA")
        out.append(("gtests: a red suite is a red job, and the others still ran", code == 3 and len(calls) == 2))
        code, calls, said = go("Nothing*", suites="")
        out.append(("gtests: a filter that matches nothing is red, not a pass", code != 0 and not calls))
        out.append(("gtests: an empty filter is refused", go("")[0] != 0))
        code, calls, said = go("SuiteA.*:SuiteB.*:Gone.*")
        out.append(("gtests: a suite the filter names and that is not there is red, naming it, and the others ran",
                    code != 0 and len(calls) == 2 and any("Gone" in l for l in said)))
        out.append(("gtests: a pattern is not a name", go("SuiteA.*:Gone*")[0] == 0))
        out.append(("gtests: a name in the negative part is not asked for", go("SuiteA.*:SuiteB.*-Gone.*")[0] == 0))
        out.append(("gtests: a suite of the other platform only is no loss here, of this one it is",
                    go("SuiteA.*:StealthWebGPUAdapter.*")[0] == 0 and go("SuiteA.*:StealthWebGPUBundledVulkan.*")[0] != 0))
        kept = tree / "kept"

        def go_kept():
            old = dict(os.environ)
            os.environ.update(FAKE_SUITES="SuiteA SuiteB/x", FAKE_FAIL="")
            try:
                return gtest_run.run(root, "Suite*", reports=kept, out=lambda _: None)
            finally:
                os.environ.clear()
                os.environ.update(old)
        code = go_kept()
        out.append(("gtests: each suite's report stays where it was asked to", code == 0 and sorted(p.name for p in kept.iterdir()) == ["SuiteA.json", "SuiteB_x.json"]))
        (root / "bin" / "firefox").rename(root / "bin" / "firefox.exe")
        code, calls, said = go("SuiteA.*:SuiteB.*")
        out.append(("gtests: Windows: firefox.exe with the arguments the green candidate gave it, the launcher waits",
                    code == 0 and len(calls) == 2 and all(c["argv"] == ["-unittest", "--gtest_death_test_style=threadsafe", "--wait-for-browser"] for c in calls)
                    and calls[0]["env"].get("MOZ_GMP_PATH", "").count("gmp-fake") == 2 and all(c["cwd"] == str(root / "gtest") for c in calls)))
        out.append(("gtests: Windows: the suite only Linux has is no loss, one Windows has is",
                    go("SuiteA.*:StealthWebGPUBundledVulkan.*")[0] == 0 and go("SuiteA.*:StealthWebGPUAdapter.*")[0] != 0))
    suites_py = ROOT / "testing" / "gtest" / "suites.py"
    if suites_py.is_file():
        spec = {}
        exec(compile(suites_py.read_text(encoding="utf-8"), str(suites_py), "exec"), spec)
        names = ["A", "B", "C/D"]
        made = list(spec["suite_filters"](names))
        # Mozilla's leaves a ":" on the last suite's list, and an empty pattern matches nothing.
        same = all(f.create(flt).rstrip(":") == gtest_run.only_suite(flt, names, f.suite)
                   for f in made for flt in ("*", "A*:B*", "A*-A.x", GREEN_FILTER))
        regex = "SuiteA.\n  One\nC/D.  # TypeParam = int\n  Two\nnoise\n"
        out.append(("gtests: the same filters and suite names as Mozilla's testing/gtest/suites.py",
                    same and gtest_run.suite_names(regex) == ["SuiteA", "C/D"]))
    return out


def lavapipe_jobs(run: str):
    words = shlex.split(run.splitlines()[0] if "lavapipe_build.py" in run.splitlines()[0] else
                        next(l for l in run.splitlines() if "lavapipe_build.py" in l))
    at = words.index("scripts/ci/lavapipe_build.py")
    rest = words[at:]
    return rest[rest.index("--jobs") + 1] if "--jobs" in rest else None


def group_build_steps(doc) -> list:
    jobs = doc["jobs"]
    steps = {s.get("name", ""): s for s in jobs["build"]["steps"]}
    names = list(steps)
    out = []
    out.append(("lavapipe: the build's job count, one source: CI_PARALLEL, and 4 where the build's is 4",
                lavapipe_jobs(steps["WebGPU's Vulkan driver (linux-x86_64) - build"]["run"]) == "${CI_PARALLEL:-4}"
                and "CI_PARALLEL=$PARALLEL" in steps["The node - compile jobs from its limits"]["run"]))
    key = steps["Compile cache - the sccache binary, if built before"]["with"]["key"]
    hosted = e.render(key, context(environment="github-hosted", node=False, leg="linux-x86_64"))
    node = e.render(key, context(environment="self-hosted"))
    out.append(("sccache binary: -node on the node, and not on GitHub's runners (%s)" % hosted, "-node" in node and "-node" not in hosted))

    save = steps["Compile cache - save this leg's"]
    tools = steps["Toolchains - save"]
    worth = steps["Compile cache - is this leg's upload worth it"]

    def ctx(outcome_, worth_="true", environment="self-hosted", hit=""):
        c = context(environment=environment)
        c["needs"] = {"plan": {"outputs": {"compile_cache": "gha"}}}
        c["steps"] = {"build": {"outcome": outcome_, "outputs": {"compiled": "true"}}, "cache-worth": {"outputs": {"save": worth_}},
                      "toolchains": {"outputs": {"cache-hit": hit}}}
        return c
    out.append(("cancel: no save starts: GitHub cuts the job 5:00 after it, and the upload takes 15",
                not e.step_runs(save, ctx("cancelled"), cancelled=True) and not e.step_runs(save, ctx("success"), cancelled=True)
                and not e.step_runs(tools, ctx("success"), cancelled=True) and not e.step_runs(worth, ctx("success"), cancelled=True)))
    out.append(("cancel: a failed build still saves, and a green one, if worth it",
                e.step_runs(save, ctx("failure"), failed=True) and e.step_runs(save, ctx("success"))
                and e.step_runs(worth, ctx("failure"), failed=True) and not e.step_runs(save, ctx("success", "false"))))
    out.append(("cancel: a build that never started saves nothing", not e.step_runs(save, ctx("skipped"), failed=True)))
    a, b = [e.render(save["with"]["key"], dict(context(attempt=n), github=dict(context(attempt=n)["github"], run_attempt=n))) for n in ("1", "2")]
    out.append(("cache key: another attempt another key: a second save on one key is refused and the step stays green", a != b))
    restore = steps["Compile cache - restore this leg's last one"]
    out.append(("cache key: and the entry a run saves is the one its restore names, read by the leg's prefix",
                restore["with"]["key"] == save["with"]["key"] and a.startswith(e.render(restore["with"]["restore-keys"], context()))))
    text = WORKFLOW.read_text(encoding="utf-8")
    out.append(("cancel: the file no longer promises a save on a cancel", "they are the ones the cache is for" in text and "or was cancelled" not in text))
    for leg_env, label in (("github-hosted", "GitHub's runners"), ("self-hosted", "the node")):
        out.append(("worth: on %s too: the newest entry is what the next run restores" % label,
                    e.step_runs(worth, ctx("success", environment=leg_env))))
    out.append(("worth: it reads the disk against what the restore brought, and the snapshot comes before the build",
                "--dir" in worth["run"] and "$RUNNER_TEMP/sccache-restored.json" in worth["run"]
                and "$RUNNER_TEMP/sccache-restored.json" in steps["Compile cache - what this build starts from"]["run"]
                and names.index("Compile cache - what this build starts from") < names.index("Build")))
    start = steps["Compile cache - what this build starts from"]["run"]
    heat = steps["Compile cache - cold or warm, from its hits"]
    out.append(("heat: a restored entry does not read as warm; the summary says cold or warm from the hits, after the stats",
                "starts from" not in start and "cache_rules.py heat" in heat["run"] and "GITHUB_STEP_SUMMARY" in heat["run"]
                and names.index("Compile cache - the build used the pinned sccache") < names.index("Compile cache - cold or warm, from its hits")))

    runs_on = jobs["build"]["runs-on"]
    x86 = dict(context(attempt="2"), needs={"plan": {"outputs": {"attempt": "1", "node_label": "latitude-7-1"}}})
    x86["matrix"] = {"runner": "", "leg": "linux-x86_64"}
    out.append(("rerun: of failed jobs, an x86 leg's runs-on is empty, with no runner to fall back to", e.render(runs_on, x86) == ""))
    refused = outcome(jobs, {"build": "failure"}, attempt="2", plan_attempt="1")
    first = outcome(jobs, {"build": "failure"}, attempt="1", plan_attempt="1")
    full = outcome(jobs, {"build": "failure"}, attempt="2", plan_attempt="2")
    out.append(("rerun: rerun-refused speaks on a re-run of failed jobs, not on the first attempt, not on a full re-run",
                refused["rerun-refused"] == "success" and first["rerun-refused"] == "skipped" and full["rerun-refused"] == "skipped"))
    x86["needs"]["plan"]["outputs"].update(attempt="2", node_label="latitude-7-2")
    out.append(("rerun: a full re-run gets the new node's label", e.render(runs_on, x86) == "latitude-7-2-linux-x86_64"))
    job = jobs["rerun-refused"]
    said = json.dumps(job["steps"])
    out.append(("rerun: it is GitHub's runner, it needs the build so that it is re-run with it, and it fails saying how",
                job["runs-on"] == "ubuntu-24.04" and "build" in job["needs"] and "gh run rerun" in said and "::error::" in said and "exit 1" in said))
    out.append(("rerun: the file no longer promises a rerun of the fallen leg alone", "ricompila la sola gamba" not in text and "refuses the job at once" not in text))

    disk = steps["Compile cache - sccache keeps it on the runner's disk"]["run"]
    server = steps["Compile cache - the node's server, which alone logs"]
    others = json.dumps(jobs["build"].get("env", {})) + json.dumps([s.get("env", {}) for s in jobs["build"]["steps"] if s.get("name") != "Build"])
    build_log = (steps["Build"].get("env") or {}).get("SCCACHE_LOG", "")
    # The clients compile here (SCCACHE_CLIENT_SIDE without MOZ_AUTOMATION), so the names of
    # the misses can only come from them: the Build step alone, the one module that says hit
    # or miss, on the node; never all of sccache's debug in the build's output.
    out.append(("sccache log: the clients get the hit-or-miss lines alone, in the Build step alone, and no step SCCACHE_ERROR_LOG",
                "SCCACHE_LOG" not in disk and "SCCACHE_ERROR_LOG" not in disk and "SCCACHE_LOG" not in others
                and "SCCACHE_ERROR_LOG" not in others and "sccache::compiler::compiler=debug" in build_log and "matrix.node" in build_log
                and "'debug'" not in build_log))
    words = shlex.split(server["run"])
    out.append(("sccache log: the server alone, on the node, started with them, by the pinned path",
                words[:2] == ["SCCACHE_LOG=debug", 'SCCACHE_ERROR_LOG=$RUNNER_TEMP/sccache-debug.log'] and words[2:] == ["$SCCACHE_PATH", "--start-server"]
                and "matrix.node" in server["if"]))
    out.append(("sccache log: after the restore, which its index reads, and before anything compiles",
                names.index("Compile cache - restore again if the first brought nothing") < names.index("Compile cache - the node's server, which alone logs") < names.index("Build")))
    misses = steps["Compile cache - which objects missed, by name"]["run"]
    events = steps["Upload this leg's events"]["with"]["path"]
    out.append(("sccache log: only the names leave the node", "$RUNNER_TEMP/sccache-debug.log" in misses and "Cache miss in" in misses
                and "sccache-misses.txt" in events and "sccache-debug" not in events))

    package = steps["Package + validate"]["env"]["XZ_OPT"]
    value = e.render(package, context())
    out.append(("xz: threads and blocks they can share on the node, nothing on GitHub's runners", "-T16" in value and "--block-size=32MiB" in value
                and e.render(package, context(node=False)) == ""))
    scripts = (HERE / "package_linux.sh").read_text(encoding="utf-8") + (HERE / "package_win.sh").read_text(encoding="utf-8")
    out.append(("xz: nothing the legs ship is the tar.xz: its own tar.gz of dist/firefox", "tar.xz" not in scripts and "-czf" in scripts and "out/$ASSET" in scripts))
    if shutil.which("xz"):
        with tempfile.TemporaryDirectory() as tmp:
            data = Path(tmp) / "in"
            data.write_bytes(os.urandom(1 << 20) * 64)
            env = dict(os.environ, XZ_OPT=value)
            # With PGO, as toolkit/moz.configure asks and python/mozbuild/mozbuild/action/package.py says it.
            done = subprocess.run(["xz", "--compress", "--stdout", "-9", "--extreme", "-vv", str(data)], env=env, capture_output=True)
            back = subprocess.run(["xz", "--decompress", "--stdout"], input=done.stdout, capture_output=True)
            mib = re.search(r"([\d,]+) MiB of memory", done.stderr.decode())
            used = int(mib.group(1).replace(",", "")) if mib else None
            threads = re.search(r"Using up to (\d+) threads", done.stderr.decode())
            out.append(("xz: its own count at -9e keeps the 16 threads, under the limit and the machine's free memory (%s MiB), and the bytes come back" % used,
                        back.stdout == data.read_bytes() and used is not None and used <= 16384 and threads and threads.group(1) == "16"))

    shim = steps["The node - its linker asks for huge pages"]
    def leg_ctx(leg, node):
        c = context(node=node, leg=leg, environment="self-hosted" if node else "github-hosted")
        c["matrix"]["family"] = "win" if leg.startswith("win") else "linux"
        return c
    # A Linux job on GitHub's runners: no build leg runs there since firefox-56, and none may get the stand-in.
    linux_ctx, win_ctx, hosted_ctx = leg_ctx("linux-x86_64", True), leg_ctx("win-x86_64", True), leg_ctx("linux-x86_64", False)
    out.append(("huge pages: the stand-in on the Linux node only, before anything links",
                e.step_runs(shim, linux_ctx) and not e.step_runs(shim, win_ctx) and not e.step_runs(shim, hosted_ctx)
                and names.index("The node - its linker asks for huge pages") < names.index("Build")))
    tunable = steps["Build"]["env"]["GLIBC_TUNABLES"]
    out.append(("huge pages: in the build's environment on Windows only: Linux's has it from its stand-in, GitHub's runners do not",
                e.render(tunable, win_ctx) == "glibc.malloc.hugetlb=1" and e.render(tunable, linux_ctx) == "" and e.render(tunable, hosted_ctx) == ""))
    gathered = set(jobs["build"].get("env", {}))
    for s in jobs["build"]["steps"]:
        gathered |= set(s.get("env", {}))
        gathered |= set(re.findall(r'echo "([A-Z][A-Z0-9_]+)=', s.get("run", "")))
    with tempfile.TemporaryDirectory() as shim_dir:
        sink = sys.stderr
        sys.stderr = open(os.devnull, "w", encoding="utf-8")
        try:
            said_env = {line.split("=", 1)[0] for line in linker_shim.write(Path(shim_dir), Path("/x"))}
        finally:
            sys.stderr.close()
            sys.stderr = sink
    out.append(("huge pages: LINKER is set nowhere: not by a step, not by the stand-in, not in a job's environment",
                said_env == {"COMPILER_PATH"} and "LINKER" not in gathered and not re.search(r"\bLINKER=", text)))
    gathered |= said_env | {"CI_PARALLEL"}
    found_options = set()
    files = 0
    for pattern in CONFIGURE_FILES:
        for path in ROOT.glob(pattern):
            files += 1
            found_options |= set(re.findall(r'env="([A-Z0-9_]+)"', path.read_text(encoding="utf-8")))
    reach = sorted(gathered & found_options)
    out.append(("options: every variable the build job sets is checked against configure's %d files: none is an option, so none reaches about:buildconfig (%s)" % (files, reach),
                files >= 5 and "LINKER" in found_options and not reach))
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        clang_bin = tmp / "clang" / "bin"
        clang_bin.mkdir(parents=True)
        for name in ("ld.lld", "lld-link"):
            real = clang_bin / name
            real.write_text('#!/bin/sh\necho "$GLIBC_TUNABLES $0 $*"\n', encoding="utf-8", newline="\n")
            real.chmod(0o755)
        sink = sys.stderr
        sys.stderr = open(os.devnull, "w", encoding="utf-8")
        try:
            linux = linker_shim.write(tmp / "a", clang_bin, threads=48)
        finally:
            sys.stderr.close()
            sys.stderr = sink
        env = {k: v for k, v in os.environ.items() if k != "GLIBC_TUNABLES"}
        said = subprocess.run([str(tmp / "a" / "ld.lld"), "-o", "libxul.so", "a b"], env=env, capture_output=True, text=True).stdout.strip()
        other = subprocess.run([str(tmp / "a" / "ld.lld"), "-o", "../../dist/bin/libmozsandbox.so", "a"], env=env, capture_output=True, text=True).stdout.strip()
        out.append(("huge pages: Linux's through COMPILER_PATH, the real linker gets the arguments and the tunable, and keeps its name",
                    linux == ["COMPILER_PATH=%s" % (tmp / "a")]
                    and said == "glibc.malloc.hugetlb=1 %s --threads=48 --time-trace --disable-verify --lto-partitions=12 -o libxul.so a b" % (clang_bin / "ld.lld")))
        out.append(("lld's threads: ld.lld caps itself at 16 when not asked, so it is asked, first, so a later flag would still win",
                    said.split()[2] == "--threads=48"))
        out.append(("the link's own times: --time-trace (libxul.so.time-trace), to tell LTO's optimiser from its code generation, "
                    "and the events artifact keeps the file",
                    said.split()[3] == "--time-trace"
                    and "obj-rel/dist/bin/libxul.so.time-trace" in str(steps["Upload this leg's events"]["with"]["path"])
                    and "**" not in str(steps["Upload this leg's events"]["with"]["path"])))
        # The trace of candidate 41912498170: 320 s of serial optimisation, then 428 s of code
        # generation on one thread. Federico chose partitions (04/11): as many as the threads of
        # the L3 group the link is pinned to, on both legs, and never through configure.
        # Not on Windows: there the split broke small executables, then xul.dll itself, an SEH
        # filter split from its function (llvm.localrecover, candidates 41989284692 and
        # 41993534158). Nothing in the workflow asks lld-link for partitions.
        text = WORKFLOW.read_text(encoding="utf-8")
        out.append(("lto partitions: 12 on libxul's ld.lld, with the verifier off; never on Windows, whose SEH filters they break; none in the mozconfig",
                    said.split()[5] == "--lto-partitions=12" and said.split()[4] == "--disable-verify" and linker_shim.LTO_PARTITIONS == 12
                    and "lldltopartitions" not in text and "_LINK_" not in text and "lld-link-shim" not in text
                    and "partitions" not in (HERE / "mozconfig.py").read_text(encoding="utf-8")))

    material = next(s for s in jobs["build"]["steps"] if "engine.py materialize" in s.get("run", ""))
    out.append(("source tree: on a node, the legs share one fetch and one series (NODE_SHARED, node_runner.sh); elsewhere as before",
                '[ -z "${NODE_SHARED:-}" ] || MATERIALIZE="$MATERIALIZE --shared $NODE_SHARED"' in material["run"]))
    pairs = {material["env"]["GIT_CONFIG_KEY_%d" % i]: material["env"]["GIT_CONFIG_VALUE_%d" % i]
             for i in range(int(material["env"]["GIT_CONFIG_COUNT"]))}
    position = [s.get("name", "") for s in jobs["build"]["steps"]]
    started = position.index(material["name"])
    waited = position.index("The source tree - materialized")
    between = jobs["build"]["steps"][started + 1:waited]
    out.append(("materialize: on the node only what needs no tree runs beside it, the compile cache's restores, and it is waited for before anything reads the tree",
                len(between) == 2 and all("actions/cache/restore" in s.get("uses", "") and "hashFiles" not in json.dumps(s) for s in between)
                and "background.py start materialize" in material["run"] and "matrix.node" in material["run"]
                and steps["The source tree - materialized"]["if"] == "matrix.node" and "background.py wait materialize" in steps["The source tree - materialized"]["run"]
                and (HERE / "background.py").is_file()))
    out.append(("materialize: only the index's form and the checkout's workers are configured",
                pairs == {"core.splitIndex": "true", "checkout.workers": "0"}))
    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp) / "r"
        repo.mkdir()

        def tree_with(extra):
            shutil.rmtree(repo / ".git", ignore_errors=True)
            base = dict(os.environ, GIT_INDEX_FILE=str(Path(tmp) / "index"), GIT_AUTHOR_NAME="a", GIT_AUTHOR_EMAIL="a@a",
                        GIT_COMMITTER_NAME="a", GIT_COMMITTER_EMAIL="a@a", **extra)
            if os.path.exists(base["GIT_INDEX_FILE"]):
                os.unlink(base["GIT_INDEX_FILE"])

            def git(*args, data=None):
                return subprocess.run(["git", "-C", str(repo), *args], env=base, input=data, capture_output=True, check=True).stdout.decode().strip()
            git("init", "-q")
            for i in range(30):
                sha = git("hash-object", "-w", "--stdin", data=b"file %d\n" % i)
                git("update-index", "--add", "--cacheinfo", "100644,%s,d/f%d" % (sha, i))
            return git("write-tree")
        out.append(("materialize: the same tree with and without it, on this git", tree_with({}) == tree_with(material["env"])))
    return out


def group_beside(doc) -> list:
    jobs = doc["jobs"]
    steps = {s.get("name", ""): s for s in jobs["build"]["steps"]}
    names = list(steps)
    at = names.index
    out = []
    save, worth = steps["Compile cache - save this leg's"], steps["Compile cache - is this leg's upload worth it"]
    tools, end = steps["Toolchains - save"], steps["Build - the link"]
    for step, name in ((worth, "worth"), (save, "the save"), (tools, "the toolchains' save")):
        out.append(("beside the link: %s has a limit under the link's time, and past it the job goes on" % name,
                    0 < step.get("timeout-minutes", 0) <= 35 and step.get("continue-on-error") is True))
    keep = steps["The node - keep them"]
    out.append(("gtests: the upload of the node's gtest binaries has a limit", 0 < keep.get("timeout-minutes", 0) <= 60))
    start, done = steps["WebGPU's Vulkan driver (linux-x86_64) - build"], steps["WebGPU's Vulkan driver (linux-x86_64) - done"]
    gate = "Compile cache - the build used the pinned sccache"
    out.append(("beside the link: the saves come between the compiles and the link's end, the statistics' gate after it",
                at("Build") < at(worth["name"]) < at(save["name"]) < at(tools["name"]) < at(end["name"]) < at(gate)))
    out.append(("beside the link: a failed save never costs the link: nothing there can fail the job, and the link's step runs regardless",
                all(s.get("continue-on-error") is True for s in (worth, save, tools))))
    c = context()
    c["steps"] = {"build": {"outcome": "success", "outputs": {"compiled": "false"}}, "toolchains": {"outputs": {"cache-hit": ""}}, "cache-worth": {"outputs": {"save": "true"}}}
    c["needs"] = {"plan": {"outputs": {"compile_cache": "gha"}}}
    out.append(("beside the link: its step waits on the node, not on GitHub's runners, not on a cancel",
                e.step_runs(end, c, failed=True) and not e.step_runs(end, c, cancelled=True)
                and not e.step_runs(end, dict(c, matrix={"node": False, "leg": "linux-x86_64"}))))
    out.append(("beside the link: toolchains saved only once they have compiled the whole build",
                not e.step_runs(tools, c) and e.step_runs(tools, dict(c, steps=dict(c["steps"], build={"outcome": "success", "outputs": {"compiled": "true"}})))))
    stop = steps["The node - a cancel or a failure ends what runs beside the steps"]
    after_build = [s for s in jobs["build"]["steps"][at("Build") + 1:at(stop["name"])]]
    cancel_ctx = dict(c, steps=dict(c["steps"], build={"outcome": "cancelled", "outputs": {"compiled": "true"}}))
    out.append(("cancel: nothing runs between the Build step and the stop, so it comes at once, and the 5:00 cut cannot reach it",
                not any(e.step_runs(s, cancel_ctx, cancelled=True) for s in after_build) and stop.get("timeout-minutes", 1) <= 2))
    out.append(("beside the link: a cancel or a failure ends the work beside the steps, a green job does not, and only on the node",
                e.step_runs(stop, c, cancelled=True) and e.step_runs(stop, c, failed=True) and not e.step_runs(stop, c)
                and not e.step_runs(stop, dict(c, matrix={"node": False}), cancelled=True) and at("Build - the link") < at(stop["name"]) < at(gate)))
    build = steps["Build"]["run"]
    out.append(("the build: in the background on the node, in the foreground on GitHub's runners, the input checked on both",
                'if [ "${{ matrix.node }}" = "true" ]; then' in build and "background.py start build" in build and "background.py until-compiled build" in build
                and "--verify --strict" in build and "--verify --strict" in end["run"] and "background.py wait build" in end["run"]
                and build.count("./mach build --keep-going") == 2))
    words = shlex.split(start["run"].replace("\\\n", " "))
    out.append(("lavapipe: starts when the compiles are over and libxul links, at nice 19, as a phase whose log is kept",
                at("Build") < at("WebGPU's Vulkan driver (linux-x86_64) - its inputs") < at("WebGPU's Vulkan driver (linux-x86_64) - built before") < at(start["name"])
                and at(start["name"]) < at(end["name"]) and words[words.index("--nice") + 1] == "19"
                and words[words.index("--") + 1:words.index("--") + 5] == ["python3", "scripts/ci", "phase", "lavapipe"]))
    def lava(compiled, leg="linux-x86_64"):
        l = dict(c, matrix={"node": True, "leg": leg})
        l["steps"] = dict(c["steps"], build={"outcome": "success", "outputs": {"compiled": compiled}})
        return l
    out.append(("lavapipe: a build that stops before the link does not start it; only on the Linux leg",
                e.step_runs(start, lava("true")) and not e.step_runs(start, lava("false")) and not e.step_runs(start, lava("true", "win-x86_64"))))
    out.append(("lavapipe: no configure of its own for its toolchain any more: by then the build has fetched it", "The toolchains, when their entry had none" not in steps))
    out.append(("lavapipe: a failure fails the leg: its wait has no way around it, and it comes before the package",
                done["run"].splitlines()[0] == "python3 scripts/ci/background.py wait lavapipe" and "continue-on-error" not in done
                and done["id"] == "lavapipe" and at(end["name"]) < at(done["name"]) < at("Package + validate")
                and done["run"].index("wait lavapipe") < done["run"].index("cp ")))
    out.append(("lavapipe: only the Linux leg, the one that ships it", "linux-x86_64" in start["if"]))
    upload = steps["Upload the logs of a failed build"]["with"]["path"]
    out.append(("lavapipe: its log is where the logs of a failed build are uploaded from", "logs/" in upload))

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        old = os.environ.get("BG_DIR")
        os.environ["BG_DIR"] = str(tmp / "bg")
        script = str(HERE / "background.py")

        def bg(*args):
            return subprocess.run([sys.executable, "-B", script, *args], capture_output=True, text=True, cwd=tmp)
        try:
            bg("start", "red", "--", "bash", "-c", "echo one; echo two; exit 7")
            said = bg("wait", "red")
            out.append(("background: its exit code and its output reach the step that waits", said.returncode == 7 and "one\ntwo\n" in said.stdout))
            bg("start", "n", "--nice", "19", "--", "bash", "-c", "nice")
            out.append(("background: the nice it was given", "19" in bg("wait", "n").stdout))
            bg("start", "green", "--", "bash", "-c", "echo first")
            first = bg("until-compiled", "green")
            second = bg("wait", "green")
            out.append(("background: until-compiled shows what it read, wait only the rest, and the code is still there", first.stdout.count("first") == 1
                        and second.stdout.count("first") == 0 and second.returncode == 0))
            out_file = tmp / "output"
            bg("start", "fail", "--", "bash", "-c", "exit 3")
            env = dict(os.environ, GITHUB_OUTPUT=str(out_file))
            done_ = subprocess.run([sys.executable, "-B", script, "until-compiled", "fail"], env=env, capture_output=True, text=True, cwd=tmp)
            out.append(("background: until-compiled exits 0, says compiled=false, and the failure waits for its step",
                        done_.returncode == 0 and out_file.read_text() == "compiled=false\n" and bg("wait", "fail").returncode == 3))
            child_file = tmp / "child"
            # As mozprocess runs make: a process group of its own, which killpg would miss.
            grouped = tmp / "grouped.py"
            grouped.write_text("import os, sys, time\nos.setpgid(0, 0)\nopen(sys.argv[1], 'w').write(str(os.getpid()))\ntime.sleep(120)\n", encoding="utf-8")
            bg("start", "long", "--", "bash", "-c", "python3 %s %s & wait" % (grouped, child_file))
            for _ in range(100):
                if child_file.exists() and child_file.read_text():
                    break
                time.sleep(0.05)
            child = int(child_file.read_text())
            group_differs = os.getpgid(child) == child
            bg("stop", "long", "nothing")
            gone = not Path("/proc/%d" % child).exists() or Path("/proc/%d/stat" % child).read_text().rsplit(")", 1)[1].split()[0] == "Z"
            out.append(("background: stop ends what runs in a process group of its own, a name with nothing behind it is no error, and its wait is not green",
                        group_differs and gone and bg("wait", "long").returncode != 0))
            (tmp / "bg" / "stale.pid").write_text(str(os.getpid()))
            bg("stop", "stale")
            out.append(("background: a pid file that outlived its process kills nothing: this test is still here", True))
            bg("start", "says", "--", "bash", "-c", 'echo "went wrong" >> "$GITHUB_STEP_SUMMARY"; exit 2')
            summary = tmp / "summary"
            env = dict(os.environ, GITHUB_STEP_SUMMARY=str(summary))
            done_ = subprocess.run([sys.executable, "-B", script, "wait", "says"], env=env, capture_output=True, text=True, cwd=tmp)
            out.append(("background: what it writes for the summary reaches the summary of the step that waits",
                        done_.returncode == 2 and summary.read_text() == "went wrong\n"))
        finally:
            if old is None:
                os.environ.pop("BG_DIR", None)
            else:
                os.environ["BG_DIR"] = old
        old_dir = os.environ.get("BG_DIR")
        os.environ["BG_DIR"] = str(tmp / "bg2")
        try:
            (tmp / "bg2").mkdir()
            (tmp / "bg2" / "dead.pid").write_text("999999")
            (tmp / "bg2" / "dead.out").write_text("")
            sink = sys.stdout
            sys.stdout = open(os.devnull, "w", encoding="utf-8")
            try:
                code = background.wait("dead", poll=0)
            finally:
                sys.stdout.close()
                sys.stdout = sink
            out.append(("background: gone with no exit code is a failure", code == 1))
            (tmp / "bg2" / "s.pid").write_text(str(os.getpid()))
            (tmp / "bg2" / "s.out").write_text("")
            clock_at = [0.0]

            def run_states(states):
                it = iter(states)

                def state():
                    clock_at[0] += 10
                    return next(it)
                sink = sys.stdout
                sys.stdout = open(os.devnull, "w", encoding="utf-8")
                try:
                    return background.until_compiled("s", poll=0, quiet_s=30, limit_s=100, state=state, clock=lambda: clock_at[0])
                finally:
                    sys.stdout.close()
                    sys.stdout = sink
            import signal as _signal
            keep = _signal.getsignal(_signal.SIGINT), _signal.getsignal(_signal.SIGTERM)
            seq = [(False, True)] * 3 + [(True, True)] * 2 + [(True, False)] * 4
            got = run_states(iter(seq))
            out.append(("until-compiled: compiling, then linking with compiles beside it, then quiet for its time: complete", got is True and clock_at[0] == 10 * len(seq)))
            clock_at[0] = 0.0
            seq = [(True, True)] * 11
            out.append(("until-compiled: a compile that never ends beside the link: the link is reason enough past the limit", run_states(iter(seq)) is True and clock_at[0] == 110))
            clock_at[0] = 0.0
            seq = [(True, False)] * 2 + [(True, True)] + [(True, False)] * 4
            out.append(("until-compiled: a compile in the quiet starts the quiet again", run_states(iter(seq)) is True and clock_at[0] == 70))
            _signal.signal(_signal.SIGINT, keep[0])
            _signal.signal(_signal.SIGTERM, keep[1])
        finally:
            if old_dir is None:
                os.environ.pop("BG_DIR", None)
            else:
                os.environ["BG_DIR"] = old_dir
        proc = tmp / "proc"

        def entry(pid, comm, cmd, nice=0):
            d = proc / str(pid)
            d.mkdir(parents=True)
            (d / "stat").write_text("%d (%s) S 1 1 1 0 -1 0 0 0 0 0 1 1 0 0 20 %d 1 0 1 1 1" % (pid, comm, nice))
            (d / "cmdline").write_bytes(cmd.replace(" ", "\0").encode())
        entry(1, "clang++", "clang++ -o libxul.so -shared a.o")
        entry(2, "ld.lld", "ld.lld --threads=48 -o libxul.so a.o")
        out.append(("compile state: libxul's link, and its waiting driver is not a compiler", background.compile_state(proc) == (True, False)))
        entry(3, "clang++", "clang++ -o b.o -c b.cpp")
        out.append(("compile state: a compile beside it is", background.compile_state(proc) == (True, True)))
        shutil.rmtree(proc)
        entry(4, "ld.lld", "ld.lld -o libxul_broken_buildid.so a.o")
        entry(5, "rustc", "rustc --crate-name x")
        out.append(("compile state: a stub is not libxul's link, and rustc compiles", background.compile_state(proc) == (False, True)))
        shutil.rmtree(proc)
        entry(6, "ld.lld", "ld.lld -o libxul.so a.o")
        entry(7, "clang++", "clang++ -o b.o -c b.cpp", nice=19)
        out.append(("compile state: a compiler at nice 19, lavapipe's, holds no save back", background.compile_state(proc) == (True, False)))
        shutil.rmtree(proc)
        entry(8, "lld-link", "lld-link -NOLOGO -DLL -OUT:../../../dist/bin/xul.dll a.obj")
        entry(9, "clang-cl", "clang-cl -Fob.obj -c b.cpp")
        out.append(("compile state: the Windows leg's link and compiles, in their own spellings", background.compile_state(proc) == (True, True)))
        out.append(("compile state: the quiet before a save is twice the longest silence seen between compiles, 50 s on Windows", background.QUIET_S >= 100))
    return out


def stage_case(differ: bool, prove: bool):
    """stage_package.sh with a make that stages a tree and a mach whose package stages
    it again, the same or one byte apart: (exit code, what it said, what ran)."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        ran = tmp / "ran"
        make = tmp / "make"
        make.write_text('#!/bin/bash\necho "make $*" >> "%s"\nd="$2/dist/firefox"; mkdir -p "$d/sub"\n'
                        'printf "one" > "$d/firefox"; printf "two" > "$d/sub/omni.ja"; ln -sfn firefox "$d/link"\n' % ran,
                        encoding="utf-8", newline="\n")
        mach = tmp / "mach"
        mach.write_text('#!/bin/bash\necho "mach $*" >> "%s"\n%s\n'
                        % (ran, 'printf "TWO" > "%s/dist/firefox/sub/omni.ja"' % (tmp / "obj") if differ else "true"),
                        encoding="utf-8", newline="\n")
        for f in (make, mach):
            f.chmod(0o755)
        env = dict(os.environ, CI_OBJDIR=str(tmp / "obj"), CI_MAKE=str(make), CI_MACH=str(mach),
                   CI_PACKAGE_PROVE="1" if prove else "")
        done = subprocess.run(["bash", str(HERE / "stage_package.sh")], env=env, capture_output=True, text=True, timeout=60)
        return done.returncode, done.stdout + done.stderr, ran.read_text(encoding="utf-8").splitlines() if ran.exists() else []


def group_stage_package(doc) -> list:
    """Both legs ship their own archive of dist/firefox. `mach package` staged it and then
    made archives nobody ships: on Windows a zip (32 s) and the installer's 7z on one
    core (89 s), on Linux a tar.xz (candidate 41892772401). stage_package.sh stages the
    tree only, and proves once that the tree is the same."""
    out = []
    scripts = {name: (HERE / name).read_text(encoding="utf-8") for name in ("package_linux.sh", "package_win.sh")}
    out.append(("package: both legs stage dist/firefox only, through stage_package.sh, and run no mach package themselves",
                all("bash scripts/ci/stage_package.sh" in s and "\n./mach package\n" not in s for s in scripts.values())))
    # The artifact quota filled on 04/11 (81 GB, ~950 MB a candidate) and every new run
    # failed at its first upload: a candidate's symbols (~400 MB) are kept 2 days.
    symbols = next(s for s in doc["jobs"]["build"]["steps"] if s.get("name") == "Upload crash symbols")
    out.append(("artifacts: a candidate's crash symbols are kept 2 days, a release's 14",
                str(symbols["with"]["retention-days"]).replace(" ", "") == "${{needs.plan.outputs.kind=='release'&&14||2}}"))
    if not sys.platform.startswith("linux"):
        print("skip  stage_package.sh: bash, Linux only")
        return out
    code, said, ran = stage_case(differ=False, prove=False)
    out.append(("package: it runs stage-package in the objdir, and nothing else (%s)" % ran,
                code == 0 and ran == ["make -C %s stage-package" % ran[0].split()[2]] if ran else False))
    code, said, ran = stage_case(differ=False, prove=True)
    out.append(("package: the proof runs mach package after it and passes on the same tree, file for file and link for link",
                code == 0 and [r.split()[0] for r in ran] == ["make", "mach"] and "proved" in said and "3 entries" in said))
    code, said, ran = stage_case(differ=True, prove=True)
    out.append(("package: one byte apart, the proof fails, and says which file",
                code != 0 and "omni.ja" in said and "differs" in said))
    return out


def cases() -> list:
    try:
        import yaml
    except ImportError:
        return [("release path: the workflow can be read (PyYAML is missing)", False)]
    doc = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    out = []
    for group, args in ((group_expressions, ()), (group_gate, (doc,)), (group_gtest, (doc,)), (group_build_steps, (doc,)), (group_beside, (doc,)),
                        (group_stage_package, (doc,))):
        try:
            out += group(*args)
        except Exception as exc:
            out.append(("%s: stopped on %s: %s" % (group.__name__, type(exc).__name__, exc), False))
    return out


def main() -> int:
    got = cases()
    failures = 0
    for name, ok in got:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(got) - failures, len(got)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
