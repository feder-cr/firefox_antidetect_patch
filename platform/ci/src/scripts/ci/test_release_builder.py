#!/usr/bin/env python3
"""Who compiles what in release.yml, read from the file itself.

The x86_64 legs compile on the Theta node and nowhere else: no input chooses, no
expression falls back, no leg names a hosted runner. linux-arm64 stays on GitHub's
runners, because the node is x86. Gates and tests stay on GitHub's runners. And the
guards around that: secrets behind an environment, labels of one run and one leg, no
workflow a pull request can start that reaches them, no cache entry the node writes and
GitHub's runners read.

    python3 scripts/ci/test_release_builder.py
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import legs  # noqa: E402
import theta_node  # noqa: E402

WORKFLOWS = HERE.parent.parent / ".github" / "workflows"
WORKFLOW = WORKFLOWS / "release.yml"
RUNS_ON = ("${{ matrix.runner || (needs.node.outputs.attempt == github.run_attempt && "
           "format('{0}-{1}', needs.node.outputs.runner, matrix.leg)) || '' }}")
ENVIRONMENT = "theta-builder"
# Everything a job on GitHub's runners may name. What a job with an expression names is
# looked at by hand below: only `build` and the gate's matrix have one.
HOSTED = re.compile(r"^(ubuntu-[\w.-]+|windows-[\w.-]+|macos-[\w.-]+)$")
PR_EVENTS = {"pull_request", "pull_request_target", "pull_request_review", "pull_request_review_comment",
             "issue_comment", "issues", "workflow_run", "merge_group", "discussion", "discussion_comment", "fork", "watch"}
WATCHES = ("watch-1", "watch-2", "watch-3")


def load(text: str) -> dict:
    import yaml
    return yaml.safe_load(text)


def triggers(doc: dict) -> set:
    # YAML reads the key `on` as true.
    on = doc.get(True, doc.get("on"))
    if isinstance(on, str):
        return {on}
    return set(on) if isinstance(on, (list, dict)) else set()


def cache_keys(doc: dict, leg, runner_arch: str, on_node: bool) -> set:
    """Every key and restore prefix this leg can read or write, in release.yml's cache
    steps, with this leg's own values in place of the expressions."""
    found = set()
    for step in doc["jobs"]["build"]["steps"]:
        if "actions/cache" not in step.get("uses", ""):
            continue
        cond = str(step.get("if", ""))
        if ("matrix.node" in cond and "!matrix.node" not in cond and not on_node) or ("matrix.leg == 'linux-x86_64'" in cond and leg.leg != "linux-x86_64"):
            continue
        for text in (step["with"].get("key", ""), step["with"].get("restore-keys", "")):
            for line in str(text).splitlines():
                line = line.replace("${{ matrix.leg }}", leg.leg).replace("${{ runner.os }}", "Linux").replace("${{ runner.arch }}", runner_arch)
                line = line.replace("${{ runner.environment == 'github-hosted' && '' || '-node' }}", "-node" if on_node else "")
                # Keys a step computes: a case below holds them to these.
                line = line.replace("${{ steps.toolchains-key.outputs.key }}", "toolchains-%s-*" % leg.leg)
                line = line.replace("${{ steps.lavapipe-key.outputs.key }}", "lavapipe-%s-*" % leg.leg)
                found.add(re.sub(r"\$\{\{.*?\}\}", "*", line))
    return found


def same_entry(a: str, b: str) -> bool:
    """Whether a key or prefix can reach an entry of the other: up to the first expression."""
    a, b = a.split("*")[0], b.split("*")[0]
    return a.startswith(b) or b.startswith(a)


def group_no_fallback(doc, jobs, build, steps) -> list:
    out = []
    inputs = doc[True]["workflow_dispatch"]["inputs"]
    out.append(("no fallback: no input chooses who compiles", "builder" not in inputs))
    out.append(("no fallback: the plan has no such output", "builder" not in jobs["plan"]["outputs"]))
    text = WORKFLOW.read_text(encoding="utf-8")
    out.append(("no fallback: nothing in the workflow reads one", "outputs.builder" not in text and "inputs.builder" not in text and "--builder" not in text))
    out.append(("no fallback: an x86_64 leg names no runner at all, so there is nothing for it to fall back to",
                {l.leg: l.runner for l in legs.LEGS} == {"linux-x86_64": "", "linux-arm64": "ubuntu-24.04-arm", "win-x86_64": ""}))
    out.append(("no fallback: x86_64 is on the node, arm64 is not, and the matrix tells the job",
                {l.leg: l.node for l in legs.LEGS} == {"linux-x86_64": True, "linux-arm64": False, "win-x86_64": True}
                and all("node" in e for e in json.loads(legs.matrix("1.0"))["include"])))
    out.append(("no fallback: the legs' runs-on has one way to a hosted runner: the matrix's own, which the x86_64 legs do not have, and it ends empty",
                build.get("runs-on") == RUNS_ON))
    expressions = re.findall(r"'([^']*)'", RUNS_ON)
    out.append(("no fallback: runs-on names no runner literally", not any(HOSTED.match(x) for x in expressions)))
    # What runs-on gives for each leg and each state of the node job.
    results = {}
    for leg in legs.LEGS:
        for runner, attempt in (("theta-5-1", "1"), ("", "1"), ("", ""), ("theta-5-1", "2")):
            value = leg.runner or (attempt == "1" and runner and "%s-%s" % (runner, leg.leg)) or ""
            results[(leg.leg, runner, attempt)] = value
    out.append(("no fallback: for each state of the node job an x86_64 leg gets its label of this attempt or nothing, never a hosted runner",
                all(not HOSTED.match(v) for (leg, _, _), v in results.items() if leg != "linux-arm64")
                and results[("win-x86_64", "theta-5-1", "1")] == "theta-5-1-win-x86_64"
                and results[("win-x86_64", "theta-5-1", "2")] == ""
                and results[("linux-arm64", "", "")] == "ubuntu-24.04-arm"))
    node = jobs["node"]
    out.append(("no fallback: a node job that fails fails the run: nothing swallows it, and the build needs it",
                "continue-on-error" not in node and "if" not in node and "node" in build["needs"]))
    node_steps = {s.get("name", ""): s for s in node["steps"]}
    out.append(("no fallback: the node job's steps always run", not any("if" in s for s in node["steps"])))
    theta = (HERE / "theta_node.py").read_text(encoding="utf-8")
    out.append(("no fallback: nor does the re-launch ask for one", "builder" not in theta.replace("test_release_builder", "")))
    gate = jobs["gate"]
    out.append(("gates and tests on GitHub: nothing else runs on a label of ours",
                all(j.get("runs-on") in ("ubuntu-24.04", "windows-latest") for n, j in jobs.items() if n != "build" and "runs-on" in j)
                and gate.get("uses") == "./.github/workflows/gate.yml"))
    out.append(("decision: builds stay full LTO and PGO, nothing here turns them off",
                "--disable-lto" not in text and "MOZ_LTO=" not in text and "disable-pgo" not in text))
    return out


def group_cache_sharing(doc, jobs, build, steps) -> list:
    """Finding 4: is anything the node writes read by the hosted arm64 leg?"""
    out = []
    arm = next(l for l in legs.LEGS if not l.node)
    arm_keys = cache_keys(doc, arm, "ARM64", False)
    node_keys = set()
    for leg in legs.LEGS:
        if leg.node:
            node_keys |= cache_keys(doc, leg, "X64", True)
    out.append(("cache: both sides have keys to compare", len(arm_keys) >= 2 and len(node_keys) >= 4))
    out.append(("cache: the keys a step computes start with their kind and their leg",
                'key=toolchains-${{ matrix.leg }}-' in steps.get("Toolchains - their versions", {}).get("run", "")
                and 'key=lavapipe-${{ matrix.leg }}-' in steps.get("WebGPU's Vulkan driver (linux-x86_64) - its inputs", {}).get("run", "")))
    clash = [(a, n) for a in arm_keys for n in node_keys if same_entry(a, n)]
    out.append(("cache: the arm64 leg, on GitHub's runners, reads and writes no entry the node reads or writes: no executable of the node's runs there (%s)" % clash, not clash))
    all_files = "\n".join(p.read_text(encoding="utf-8") for p in WORKFLOWS.glob("*.yml") if p.name != "release.yml")
    out.append(("cache: and no other workflow restores a cache at all", "actions/cache" not in all_files))
    lavapipe = steps.get("WebGPU's Vulkan driver (linux-x86_64) - built before", {})
    tool = steps.get("Toolchains - restore", {})
    out.append(("cache: the executables' entries are the node legs' only", "matrix.leg == 'linux-x86_64'" in lavapipe.get("if", "") and tool.get("if") == "matrix.node"))
    restore = steps.get("Compile cache - restore this leg's last one", {})
    save = steps.get("Compile cache - save this leg's", {})
    out.append(("cache: the compile cache on the node, by leg",
                "runner.environment" not in restore.get("if", "")
                and restore.get("with", {}).get("key") == "sccache-${{ matrix.leg }}-${{ github.run_id }}-${{ github.run_attempt }}"
                and restore.get("with", {}).get("restore-keys") == "sccache-${{ matrix.leg }}-"
                and "runner.environment" not in save.get("if", "")
                and save.get("with", {}).get("key") == restore.get("with", {}).get("key")))
    out.append(("cache: a save only if worth the upload, and never of a build that did not start",
                "steps.cache-worth.outputs.save != 'false'" in save.get("if", "") and "steps.build.outcome" in save.get("if", "")
                and "cache_rules.py worth" in steps.get("Compile cache - is the node's upload worth it", {}).get("run", "")))
    binary = steps.get("Compile cache - the sccache binary, if built before", {})
    out.append(("cache: the node's sccache binary is its own entry: glibc 2.35 cannot run one built on 24.04",
                "-node" in binary.get("with", {}).get("key", "")))
    text = WORKFLOW.read_text(encoding="utf-8")
    out.append(("cache: the comment on sccache's key says what its source does", "sccache's version\n          # enters their key" not in text.replace("\r\n", "\n")
                and "neither sccache's version nor" in text))
    tool_restore, tool_save = steps.get("Toolchains - restore", {}), steps.get("Toolchains - save", {})
    names = list(steps)
    out.append(("cache: the toolchains have their own entry, by their versions, restored before Rust and the build",
                "~/.mozbuild" in tool_restore.get("with", {}).get("path", "")
                and tool_restore.get("with", {}).get("key") == tool_save.get("with", {}).get("key")
                and "rust$RUST" in steps.get("Toolchains - their versions", {}).get("run", "")
                and names.index("Toolchains - restore") < names.index("Rust, the retail's release for this tree") < names.index("Build")))
    out.append(("cache: a toolchain entry only from a build that worked", "steps.build.outcome == 'success'" in tool_save.get("if", "")))
    env = build.get("env", {})
    out.append(("cache key: CARGO_HOME declared by the job, the same on the node and on GitHub's images", env.get("CARGO_HOME") == "/home/runner/.cargo"))
    guard = steps.get("Compile cache - what its key depends on", {})
    out.append(("cache key: the guard in the step summary, after the build",
                "cache_rules.py guard" in guard.get("run", "") and "GITHUB_STEP_SUMMARY" in guard.get("run", "")
                and names.index("Build") < names.index("Compile cache - what its key depends on")))
    disk = steps.get("Compile cache - sccache keeps it on the runner's disk", {})
    misses = steps.get("Compile cache - which objects missed, by name", {})
    events = steps.get("Upload this leg's events", {})
    out.append(("cache key: the node's first runs name their misses, out of the tree, only the names kept",
                "SCCACHE_LOG=debug" in disk.get("run", "") and "$RUNNER_TEMP/sccache-debug.log" in disk.get("run", "")
                and "matrix.node" in disk.get("run", "") and "Cache miss in" in misses.get("run", "")
                and "sccache-misses.txt" in events.get("with", {}).get("path", "") and "sccache-debug" not in events.get("with", {}).get("path", "")))
    return out


def group_secrets_and_labels(doc, jobs, build, steps) -> list:
    """Finding 5, 6, 15, and fork pull requests on a public repository."""
    out = []
    secret_jobs = sorted(n for n, j in jobs.items() if "secrets." in json.dumps(j))
    out.append(("secrets: only on the jobs that run on GitHub's runners (%s)" % ", ".join(secret_jobs),
                secret_jobs == ["node", "stop-node", "watch-1", "watch-2", "watch-3"]
                and all(jobs[n]["runs-on"] == "ubuntu-24.04" for n in secret_jobs)))
    out.append(("secrets: each of those jobs declares the environment that limits them to its branches",
                all(jobs[n].get("environment") == ENVIRONMENT for n in secret_jobs)))
    out.append(("secrets: on the steps that use them, not on the job", all("secrets." not in json.dumps(jobs[n].get("env", {})) for n in secret_jobs)))
    sweeper = load((WORKFLOWS / "theta-sweeper.yml").read_text(encoding="utf-8"))
    out.append(("secrets: the sweeper's job too", sweeper["jobs"]["sweep"].get("environment") == ENVIRONMENT))
    out.append(("secrets: and no job hands them on to another workflow", "secrets: inherit" not in WORKFLOW.read_text(encoding="utf-8")))
    out.append(("token: the workflow's default reads, the build job's reads, no job on the node can do more",
                doc.get("permissions") == {"contents": "read"} and build.get("permissions") == {"contents": "read"}))
    tree = {}
    for path in sorted(WORKFLOWS.glob("*.yml")):
        tree[path.name] = load(path.read_text(encoding="utf-8"))
    reachable = []
    for name, wf in tree.items():
        if triggers(wf) & PR_EVENTS:
            for job_name, job in wf["jobs"].items():
                runs_on = job.get("runs-on", "")
                if "uses" in job or not isinstance(runs_on, str) or not HOSTED.match(runs_on):
                    reachable.append("%s:%s:%s" % (name, job_name, runs_on or job.get("uses")))
    out.append(("fork PRs: every job a pull request or a comment can start names a hosted runner, literally (%s)" % reachable, not reachable))
    out.append(("fork PRs: and there is such a workflow to check, pr-handler.yml", "pull_request_target" in triggers(tree["pr-handler.yml"])))
    out.append(("fork PRs: release.yml, the sweeper and the workflows they call start by hand or by schedule only",
                triggers(doc) == {"workflow_dispatch"} and triggers(tree["theta-sweeper.yml"]) == {"schedule", "workflow_dispatch"}
                and triggers(tree["gate.yml"]) <= {"workflow_call", "workflow_dispatch"} and triggers(tree["publish.yml"]) == {"workflow_call"}))
    with_label = []
    for name, wf in tree.items():
        for job_name, job in wf["jobs"].items():
            runs_on = json.dumps(job.get("runs-on", ""))
            if "self-hosted" in runs_on or "theta-" in runs_on:
                with_label.append("%s:%s" % (name, job_name))
    out.append(("fork PRs: no workflow names self-hosted or one of our labels (%s)" % with_label, not with_label))
    clock = []

    class Hub:
        def __call__(self, method, url, headers, body):
            clock.append(json.loads(body))
            return 201, b'{"encoded_jit_config":"X"}'
    hub = theta_node.GitHub("o/r", "T", "A", Hub(), lambda s: None)
    sink = sys.stderr
    sys.stderr = open(os.devnull, "w", encoding="utf-8")
    try:
        hub.jit_config("ci5a1-1", theta_node.leg_label(5, 1, "linux-x86_64"))
    finally:
        sys.stderr.close()
        sys.stderr = sink
    out.append(("runner: its one label is this run's, this attempt's, this leg's, with no self-hosted among them",
                clock and clock[0]["labels"] == ["theta-5-1-linux-x86_64"]))
    out.append(("runner: a new attempt never matches an old runner, and two legs never share one",
                theta_node.leg_label(5, 1, "a") != theta_node.leg_label(5, 2, "a") != theta_node.leg_label(5, 2, "b")))
    provision = next(s for s in jobs["node"]["steps"] if s.get("id") == "node")
    out.append(("provision: a crash behind the tee fails the step: shell bash, which adds pipefail (finding 6)",
                provision.get("shell") == "bash" and "| tee -a" in provision.get("run", "")))
    keep = next(s for s in jobs["node"]["steps"] if "upload-artifact" in s.get("uses", ""))
    out.append(("provision: its token can read a run's state, which is how it tells a live run's node from a leftover",
                jobs["node"]["permissions"].get("actions") == "read"))
    out.append(("watch: a failed download of the access file leaves the watch watching",
                all(next(s for s in jobs[n]["steps"] if "download-artifact" in s.get("uses", "")).get("continue-on-error") is True for n in WATCHES)))
    out.append(("provision: only the encrypted access file is kept, for a day, per attempt",
                keep["with"]["path"] == "node-access.enc" and keep["with"]["retention-days"] == 1
                and keep["with"]["name"] == "node-access-${{ github.run_attempt }}"))
    return out


def group_watch_and_stop(doc, jobs, build, steps) -> list:
    out = []
    missing = [n for n in WATCHES if n not in jobs]
    out.append(("watch: three jobs in a row", not missing))
    if missing:
        return out
    out.append(("watch: the same steps in each", jobs["watch-1"]["steps"] == jobs["watch-2"]["steps"] == jobs["watch-3"]["steps"]))
    per_job = theta_node.STAGE_LIMIT_S // 60
    timeouts = [jobs[n]["timeout-minutes"] for n in WATCHES]
    build_limit = build["timeout-minutes"]
    out.append(("watch (finding 11): each stops by itself inside its job's limit and GitHub's 360 minutes",
                all(per_job < limit <= 355 for limit in timeouts)))
    out.append(("watch (finding 11): together they outlast two legs at their limit", 3 * per_job >= 2 * build_limit + 30))
    out.append(("watch: each survives a cancel and a crash of the one before; none follows a verdict",
                all(jobs[n]["if"].startswith("always()") and "needs.node.result == 'success'" in jobs[n]["if"] for n in WATCHES)
                and "needs.watch-1.outputs.verdict" in jobs["watch-2"]["if"]
                and "needs.watch-1.outputs.verdict" in jobs["watch-3"]["if"] and "needs.watch-2.outputs.verdict" in jobs["watch-3"]["if"]))
    out.append(("watch: it needs the node job only, so it does not wait for arm64", jobs["watch-1"]["needs"] == ["plan", "node"]))
    step = next(s for s in jobs["watch-1"]["steps"] if s.get("id") == "watch")
    out.append(("watch: its verdict survives its failure, and a crash is not swallowed", step.get("shell") == "bash" and 'tee -a "$GITHUB_OUTPUT"' in step["run"]))
    env = step["env"]
    out.append(("re-launch: every input by value, the refs never empty (finding 3)",
                env.get("CORE_REF") == "${{ inputs.core_ref || 'main' }}" and env.get("WRAPPER_REF") == "${{ inputs.wrapper_ref || 'main' }}"
                and env.get("KIND") == "${{ needs.plan.outputs.kind }}" and env.get("RELAUNCH_OF") == "${{ inputs.relaunch_of }}"
                and "GTESTS" in env and "GTEST_FILTER_INPUT" in env))
    out.append(("re-launch: refs as the gate reads them", jobs["gate"]["with"]["core_ref"] == "${{ inputs.core_ref || 'main' }}"))
    out.append(("re-launch: the run says so in its name, as the watch looks for it",
                theta_node.relaunch_title("{0}") in doc.get("run-name", "") and "relaunch_of" in doc[True]["workflow_dispatch"]["inputs"]))
    stop = jobs["stop-node"]
    delete = next(s for s in stop["steps"] if " off " in s.get("run", ""))
    out.append(("stop: follows a cancel and every watch, and does not wait for arm64",
                stop["if"].startswith("always()") and stop["needs"] == ["plan", "node"] + list(WATCHES) and delete["if"] == "always()"))
    out.append(("stop (finding 1): a watch that crashed is no reason to delete: with the node job done, only if the legs have finished",
                delete["env"]["ONLY_IF"] == "${{ needs.node.result == 'success' && '--if-legs-done' || '' }}" and "$ONLY_IF" in delete["run"]))
    trim = jobs.get("cache-trim", {})
    out.append(("cache-trim: on GitHub's runners with actions: write; the build job never has it",
                trim.get("runs-on") == "ubuntu-24.04" and trim.get("permissions", {}).get("actions") == "write"
                and "actions" not in build.get("permissions", {})))
    return out


def group_node_steps(doc, jobs, build, steps) -> list:
    out = []
    swap = steps.get("Free disk + 32G swap (Linux runners)", {})
    out.append(("node: no disk cleaning and no swap on it", swap.get("if") == "runner.environment == 'github-hosted'"))
    parallel = steps.get("The node - compile jobs from its limits", {})
    out.append(("node: its parallelism only on it, and an empty answer stops the step (finding 16)",
                parallel.get("if") == "runner.environment != 'github-hosted'"
                and 'PARALLEL="$(python3 scripts/ci node-parallel)"' in parallel.get("run", "") and '[ "$PARALLEL" -ge 1 ]' in parallel.get("run", "")))
    done = subprocess.run([sys.executable, "-B", str(HERE), "mozconfig", "--help"], env=dict(os.environ, CI_PARALLEL=""), capture_output=True, text=True)
    out.append(("node: an empty CI_PARALLEL does not crash the tool (finding 16)", done.returncode == 0))
    out.append(("node: the build job's token reads, no more", build.get("permissions") == {"contents": "read"}))
    gtests = steps.get("The node - its gtest binaries, for a job on GitHub's runners", {})
    out.append(("node: it only compiles its gtests, and a job on GitHub's runners runs Linux's",
                "recurse_gtest" in gtests.get("run", "") and "mach gtest" not in gtests.get("run", "")
                and jobs.get("gtest-linux", {}).get("runs-on") == "ubuntu-24.04"
                and any(s.get("with", {}).get("name") == "gtest-bin-linux-x86_64" for s in jobs.get("gtest-linux", {}).get("steps", []))))
    sweeper = load((WORKFLOWS / "theta-sweeper.yml").read_text(encoding="utf-8"))
    out.append(("sweeper: scheduled, and only the sweep", "schedule" in sweeper[True]
                and "theta_node.py sweep" in sweeper["jobs"]["sweep"]["steps"][-1]["run"]))
    publish = (WORKFLOWS / "publish.yml").read_text(encoding="utf-8")
    out.append(("publish: no refusal of a cached build, and nothing reads a builder record",
                "compile-cache" not in publish and "builder.txt" not in publish))
    report = steps.get("CPU use, by phase", {})
    build_step = steps.get("Build", {})
    events = steps.get("Upload this leg's events", {})
    names = list(steps)
    out.append(("CPU: the node's build watches its CPU, and its table can never fail the build",
                "memory_watch.py" in build_step.get("run", "") and "cpu-report" in report.get("run", "")
                and report.get("continue-on-error") is True and report.get("if") == "always() && matrix.node"
                and names.index("Package + validate") < names.index("CPU use, by phase") < names.index("Upload this leg's events")
                and "cpuwatch.jsonl" in events.get("with", {}).get("path", "")))
    tests = (HERE.parent / "ci" / "__main__.py").read_text(encoding="utf-8")
    out.append(("CPU: the job count stays min(CPU, RAM GB / 2)", (HERE / "mozconfig.py").read_text(encoding="utf-8").count("ram_bytes // (2 * 2**30)") == 1 and "cpu-report" in tests))
    return out


def cases() -> list:
    try:
        doc = load(WORKFLOW.read_text(encoding="utf-8"))
    except ImportError:
        return [("builder: the workflow can be read (PyYAML is missing)", False)]
    jobs = doc["jobs"]
    build = jobs.get("build", {})
    steps = {s.get("name", ""): s for s in build.get("steps", [])}
    out = []
    for group in (group_no_fallback, group_cache_sharing, group_secrets_and_labels, group_watch_and_stop, group_node_steps):
        try:
            out += group(doc, jobs, build, steps)
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
