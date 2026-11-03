#!/usr/bin/env python3
"""Who compiles what in release.yml, read from the file itself.

The x86_64 legs compile on the Theta node and nowhere else: no input chooses, no
expression falls back, no leg names a hosted runner. linux-arm64 stays on GitHub's
runners, because the node is x86. Gates and tests stay on GitHub's runners. And the
guards around that: secrets behind an environment, labels of one run and one leg, no
workflow on our branches that a pull request starts reaching them, no cache entry the
node writes and GitHub's runners read.

A fork's pull request runs the workflow files of its own merge commit, so it can name
any runs-on, a label of ours included: nothing here can rule that out. What stops it is
GitHub's approval of outside contributors, then the job hook (node_runner.sh).

The exposure group is static, and says what it cannot prove: a script a step calls may
print its own environment, a tool's debug output or a crash may carry a value, a value
may be encoded on its way out, and an action's code is not read at all.

    python3 scripts/ci/test_release_builder.py
"""
from __future__ import annotations

import ast
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gh_expr  # noqa: E402
import legs  # noqa: E402
import theta_node  # noqa: E402

WORKFLOWS = HERE.parent.parent / ".github" / "workflows"
WORKFLOW = WORKFLOWS / "release.yml"
RUNS_ON = ("${{ matrix.runner || (needs.plan.outputs.attempt == github.run_attempt && "
           "format('{0}-{1}', needs.plan.outputs.node_label, matrix.leg)) || '' }}")
ENVIRONMENT = "theta-builder"
# Everything a job on GitHub's runners may name. What a job with an expression names is
# looked at by hand below: only `build` and the gate's matrix have one.
HOSTED = re.compile(r"^(ubuntu-[\w.-]+|windows-[\w.-]+|macos-[\w.-]+)$")
PR_EVENTS = {"pull_request", "pull_request_target", "pull_request_review", "pull_request_review_comment",
             "issue_comment", "issues", "workflow_run", "merge_group", "discussion", "discussion_comment", "fork", "watch"}
WATCHES = ("watch-1", "watch-2")
INPUT_IN_EXPRESSION = re.compile(r"\$\{\{[^}]*(?:\binputs\.|\bgithub\.head_ref\b|\bgithub\.event\.)[^}]*\}\}")
SENSITIVE = ("THETA_API_KEY", "THETA_PROJECT", "RUNNER_ADMIN_TOKEN", "GH_TOKEN", "GITHUB_TOKEN", "NODE_ACCESS",
             "ACTIONS_RUNNER_INPUT_JITCONFIG", "encoded_jit_config")
PRINTED = re.compile(r"\b(?:echo|printf|print|tee|cat)\b.*\b(?:%s)\b" % "|".join(SENSITIVE))
TRACED = re.compile(r"\bset\s+-\w*x|\bset\s+-o\s+xtrace\b|\b(?:ba)?sh\s+-\w*x\b")
DUMPED = re.compile(r"^\s*(?:env|printenv|export(?:\s+-p)?|declare\s+-p|set)\s*(?:$|[|>])", re.M)
# Names in theta_node.py that hold the address or a secret: none is an argument of say() or print().
HOLDS_SECRET_OR_ADDRESS = {"address", "host", "port", "key", "config", "api_key", "token", "admin_token", "secret", "private_key"}
# What a job on the node would otherwise install with sudo: the bootstrap's to install.
NODE_JOB_PACKAGES = {"util-linux", "binutils", "cmake", "ninja-build", "bison", "flex", "patchelf"}


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
    steps, with this leg's own values in place of the expressions, each evaluated."""
    found = set()
    context = {"matrix": {"leg": leg.leg}, "runner": {"os": "Linux", "arch": runner_arch,
                                                      "environment": "self-hosted" if on_node else "github-hosted"}}

    def known(m):
        return gh_expr.to_text(gh_expr.evaluate(m.group(1), context)) if re.search(r"\b(matrix|runner)\.", m.group(1)) else m.group(0)
    for step in doc["jobs"]["build"]["steps"]:
        if "actions/cache" not in step.get("uses", ""):
            continue
        cond = str(step.get("if", ""))
        if ("matrix.node" in cond and "!matrix.node" not in cond and not on_node) or ("matrix.leg == 'linux-x86_64'" in cond and leg.leg != "linux-x86_64"):
            continue
        for text in (step["with"].get("key", ""), step["with"].get("restore-keys", "")):
            for line in str(text).splitlines():
                line = gh_expr.EXPRESSION.sub(known, line)
                # Keys a step computes: a case below holds them to these.
                line = line.replace("${{ steps.toolchains-key.outputs.key }}", "toolchains-%s-*" % leg.leg)
                line = line.replace("${{ steps.lavapipe-key.outputs.key }}", "lavapipe-%s-*" % leg.leg)
                found.add(re.sub(r"\$\{\{.*?\}\}", "*", line))
    return found


def series_root():
    """The series repository, whose root holds the workflows GitHub runs: the plan job's
    checkout, this file's own repository, SERIES_REPO, or C:/ff/engine beside a worktree."""
    tree = HERE.parent.parent
    places = [Path(os.environ["SERIES_REPO"])] if os.environ.get("SERIES_REPO") else []
    places += [tree / ".series"] + list(HERE.parents[4:5]) + [tree.parent / "engine"]
    for root in places:
        if (root / ".github" / "workflows" / "series.yml").is_file():
            return root
    raise RuntimeError("the series repository is not at %s: set SERIES_REPO" % ", ".join(str(p) for p in places))


def tree_workflows() -> dict:
    return {p.name: load(p.read_text(encoding="utf-8")) for p in sorted(WORKFLOWS.glob("*.yml"))}


def live_workflows() -> dict:
    """What GitHub runs once this tree is exported: the names at the series repository's
    root, with this tree's copy of each it has (tools/engine.py check holds the two equal).
    The source tree's other files, as Mozilla's pr-handler.yml, are on no branch of ours."""
    tree = tree_workflows()
    return {p.name: tree.get(p.name) or load(p.read_text(encoding="utf-8"))
            for p in sorted((series_root() / ".github" / "workflows").glob("*.yml"))}


def jobs_of_pull_requests(workflows: dict) -> list:
    """Each job a pull request or a comment starts that does not name a hosted runner, literally."""
    found = []
    for name, wf in workflows.items():
        if triggers(wf) & PR_EVENTS:
            for job_name, job in wf["jobs"].items():
                runs_on = job.get("runs-on", "")
                if "uses" in job or not isinstance(runs_on, str) or not HOSTED.match(runs_on):
                    found.append("%s:%s:%s" % (name, job_name, runs_on or job.get("uses")))
    return found


def steps_of(workflows: dict):
    for name, wf in workflows.items():
        for job_name, job in (wf.get("jobs") or {}).items():
            for step in job.get("steps") or []:
                yield "%s:%s:%s" % (name, job_name, step.get("name", "")), wf, job, step


def inputs_in_scripts(workflows: dict) -> list:
    """A value someone typed, pasted into a shell script before the shell reads it."""
    return ["%s: %s" % (where, hit) for where, _, _, step in steps_of(workflows)
            for hit in INPUT_IN_EXPRESSION.findall(step.get("run") or "")]


def printed_secrets(workflows: dict) -> list:
    """Steps that hold a secret and, as far as their own text shows, can print it."""
    found = []
    for where, wf, job, step in steps_of(workflows):
        run = step.get("run") or ""
        env = json.dumps([wf.get("env"), job.get("env"), step.get("env")])
        holds = "secrets." in env or "github.token" in env or any(name in env for name in SENSITIVE)
        if re.search(r"\$\{\{[^}]*\bsecrets\.", run):
            found.append("%s: a secret pasted into the script" % where)
        if holds and (TRACED.search(run) or TRACED.search(str(step.get("shell", "")))):
            found.append("%s: tracing, with a secret in the environment" % where)
        if holds and DUMPED.search(run):
            found.append("%s: dumps its environment" % where)
        found += ["%s: %s" % (where, line.strip()) for line in run.splitlines() if PRINTED.search(line)]
    return found


def printed_names(source: str) -> list:
    """say() and print() calls of theta_node.py given something that holds the address or a secret."""
    found = []
    for call in ast.walk(ast.parse(source)):
        if isinstance(call, ast.Call) and getattr(call.func, "id", "") in ("say", "print"):
            for node in (n for arg in call.args for n in ast.walk(arg)):
                name = getattr(node, "id", None) or getattr(node, "attr", None)
                if isinstance(node, ast.Subscript) and isinstance(node.slice, ast.Constant):
                    name = node.slice.value
                if name in HOLDS_SECRET_OR_ADDRESS:
                    found.append("line %d: %s" % (call.lineno, name))
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
    # What runs-on gives for each leg, in this attempt and in a rerun of failed jobs.
    prefix = jobs["plan"]["outputs"]["node_label"].replace("${{ github.run_id }}", "5").replace("${{ github.run_attempt }}", "1")
    results = {}
    for leg in legs.LEGS:
        for attempt in ("1", "2"):
            results[(leg.leg, attempt)] = leg.runner or (attempt == "1" and "%s-%s" % (prefix, leg.leg)) or ""
    out.append(("no fallback: an x86_64 leg gets the label its runner was registered with, or nothing on a rerun, never a hosted runner",
                all(not HOSTED.match(v) for (leg, _), v in results.items() if leg != "linux-arm64")
                and all(results[(leg, "1")] == theta_node.leg_label("5", "1", leg) for leg in theta_node.node_legs())
                and results[("win-x86_64", "2")] == "" and results[("linux-arm64", "2")] == "ubuntu-24.04-arm"))
    out.append(("runs-on: reads no output of a matrix job, which overwrite each other: nothing in the workflow does",
                "needs.node.outputs" not in text and "outputs" not in jobs["node"]
                and jobs["plan"]["outputs"].get("attempt") == "${{ github.run_attempt }}"))
    node = jobs["node"]
    matrix = node["strategy"]["matrix"]["leg"]
    out.append(("node: one job for each leg that compiles on a node, whichever fails the other goes on, so the run names every machine it could not have",
                matrix == theta_node.node_legs() and node["strategy"]["fail-fast"] is False and node["name"] == "node-${{ matrix.leg }}"))
    out.append(("no fallback: a node job that fails fails the run: nothing swallows it, and the build needs it, so the matrix does not run at all",
                "continue-on-error" not in node and "node" in build["needs"] and "if" not in build))
    out.append(("node: a rerun of failed jobs turns nothing on: it could not run on it",
                node["if"] == "needs.plan.outputs.attempt == github.run_attempt"))
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
                and "cache_rules.py worth" in steps.get("Compile cache - is this leg's upload worth it", {}).get("run", "")))
    binary = steps.get("Compile cache - the sccache binary, if built before", {})
    out.append(("cache: the node's sccache binary is its own entry: glibc 2.35 cannot run one built on 24.04",
                any(k.startswith("sccache-bin-") and "-node" in k for k in node_keys)
                and any(k.startswith("sccache-bin-") and "-node" not in k for k in arm_keys)))
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
    out.append(("cache: a toolchain entry only from toolchains that compiled the whole build", "steps.build.outputs.compiled == 'true'" in tool_save.get("if", "")))
    env = build.get("env", {})
    out.append(("cache key: CARGO_HOME declared by the job, which no runner's image does, the same on every leg", env.get("CARGO_HOME") == "/home/runner/.cargo"))
    out.append(("cache key: the comment no longer says GitHub's images state it", "as\n      # GitHub's images state it" not in text.replace("\r\n", "\n")))
    guard = steps.get("Compile cache - what its key depends on", {})
    out.append(("cache key: the guard in the step summary, after the build",
                "cache_rules.py guard" in guard.get("run", "") and "GITHUB_STEP_SUMMARY" in guard.get("run", "")
                and names.index("Build") < names.index("Compile cache - what its key depends on")))
    misses = steps.get("Compile cache - which objects missed, by name", {})
    events = steps.get("Upload this leg's events", {})
    out.append(("cache key: the node's first runs name their misses, out of the tree, only the names kept",
                "Cache miss in" in misses.get("run", "") and "matrix.node" in misses.get("if", "")
                and "sccache-misses.txt" in events.get("with", {}).get("path", "") and "sccache-debug" not in events.get("with", {}).get("path", "")))
    return out


def group_secrets_and_labels(doc, jobs, build, steps) -> list:
    """Finding 5, 6, 15, and fork pull requests on a public repository."""
    out = []
    secret_jobs = sorted(n for n, j in jobs.items() if "secrets." in json.dumps(j))
    out.append(("secrets: only on the jobs that run on GitHub's runners (%s)" % ", ".join(secret_jobs),
                secret_jobs == ["node", "stop-node", "watch-1", "watch-2"]
                and all(jobs[n]["runs-on"] == "ubuntu-24.04" for n in secret_jobs)))
    out.append(("secrets: each of those jobs declares the environment that limits them to its branches",
                all(jobs[n].get("environment") == ENVIRONMENT for n in secret_jobs)))
    out.append(("secrets: on the steps that use them, not on the job", all("secrets." not in json.dumps(jobs[n].get("env", {})) for n in secret_jobs)))
    sweeper = load((WORKFLOWS / "theta-sweeper.yml").read_text(encoding="utf-8"))
    out.append(("secrets: the sweeper's job too", sweeper["jobs"]["sweep"].get("environment") == ENVIRONMENT))
    out.append(("secrets: and no job hands them on to another workflow", "secrets: inherit" not in WORKFLOW.read_text(encoding="utf-8")))
    out.append(("token: the workflow's default reads, the build job's reads, no job on the node can do more",
                doc.get("permissions") == {"contents": "read"} and build.get("permissions") == {"contents": "read"}))
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
    provision = jobs["node"]["steps"][-1]
    out.append(("provision: its failure fails the step, with nothing behind a pipe to hide it", "|" not in provision.get("run", "")))
    out.append(("provision: its token can read a run's state, which is how it tells a live run's node from a leftover",
                jobs["node"]["permissions"].get("actions") == "read"))
    out.append(("no key travels: no job hands a node's access to another, no artifact of it, and the watch has nothing to open",
                "node-access" not in WORKFLOW.read_text(encoding="utf-8") and not any("artifact" in s.get("uses", "") for n in ("node",) + WATCHES for s in jobs[n]["steps"])))
    return out


def group_pull_requests(doc, jobs, build, steps) -> list:
    """What the branches' own workflows do on a pull request. Not what a fork can write in
    its copy of them: see the top of this file."""
    out = []
    live = live_workflows()
    out.append(("pull requests: the workflows read are those on the branches: series.yml, the only one with pull_request, and not the source tree's pr-handler.yml",
                "series.yml" in live and "pull_request" in triggers(live["series.yml"]) and "pr-handler.yml" not in live
                and [n for n, wf in live.items() if triggers(wf) & PR_EVENTS] == ["series.yml"]))
    reachable = jobs_of_pull_requests(live)
    out.append(("pull requests: every job those start names a hosted runner, literally (%s)" % reachable, not reachable))
    out.append(("pull requests: the check sees a job on a label of ours",
                jobs_of_pull_requests({"x.yml": {"on": ["pull_request"], "jobs": {"j": {"runs-on": "theta-5-1-a"}}}}) == ["x.yml:j:theta-5-1-a"]))
    out.append(("pull requests: release.yml, the sweeper and the workflows they call start by hand or by schedule only",
                triggers(doc) == {"workflow_dispatch"} and triggers(live["theta-sweeper.yml"]) == {"schedule", "workflow_dispatch"}
                and triggers(live["gate.yml"]) <= {"workflow_call", "workflow_dispatch"} and triggers(live["publish.yml"]) == {"workflow_call"}))
    with_label = []
    for name, wf in live.items():
        for job_name, job in wf["jobs"].items():
            runs_on = json.dumps(job.get("runs-on", ""))
            if "self-hosted" in runs_on or "theta-" in runs_on:
                with_label.append("%s:%s" % (name, job_name))
    out.append(("pull requests: no workflow on the branches names self-hosted or one of our labels (%s)" % with_label, not with_label))
    return out


def group_exposure(doc, jobs, build, steps) -> list:
    out = []
    everywhere = dict(tree_workflows(), **live_workflows())
    pasted = inputs_in_scripts(everywhere)
    out.append(("shell: no input, head ref or event field is pasted into a script, in any workflow (%s)" % pasted, not pasted))
    out.append(("shell: the check sees one", len(inputs_in_scripts({"x.yml": {"jobs": {"j": {"steps": [
        {"run": 'echo "${{ inputs.a }}"'}, {"run": 'echo "${{ github.event.inputs.a || 1 }}"'}, {"run": 'echo "${{ github.head_ref }}"'},
        {"run": 'echo "${{ github.event_name }}"'}, {"env": {"A": "${{ inputs.a }}"}, "run": 'echo "$A"'}]}}}})) == 3))
    printed = printed_secrets(everywhere)
    out.append(("logs: no step that holds a secret prints it, traces itself or dumps its environment, as far as its own text shows (%s)" % printed, not printed))
    secret = {"THETA_API_KEY": "${{ secrets.THETA_API_KEY }}"}
    bad = [{"env": secret, "run": 'echo "$THETA_API_KEY"'}, {"env": secret, "run": "set -ex\npython3 a.py"}, {"env": secret, "run": "env | sort"},
           {"run": 'curl -H "x: ${{ secrets.THETA_API_KEY }}" u'}, {"env": secret, "shell": "bash -x {0}", "run": "python3 a.py"}]
    out.append(("logs: the check sees each kind of it, and not a step that only uses one",
                all(printed_secrets({"x.yml": {"jobs": {"j": {"steps": [step]}}}}) for step in bad)
                and not printed_secrets({"x.yml": {"jobs": {"j": {"steps": [{"env": secret, "run": "python3 a.py --run 5 | tee -a out"}]}}}})))
    theta = (HERE / "theta_node.py").read_text(encoding="utf-8")
    named = printed_names(theta)
    out.append(("logs: theta_node.py gives say() and print() neither the address nor a secret (%s)" % named, not named))
    out.append(("logs: the check sees that", sorted(printed_names("say('at %s:%d' % (address[0], port))\nprint(access['key'])\nsay('fine %s' % exc)")) ==
                ["line 1: address", "line 1: port", "line 2: key"]))
    runners = (HERE / "node_runner.sh").read_text(encoding="utf-8")
    bootstrap = (HERE / "node_bootstrap.sh").read_text(encoding="utf-8")
    uses = [line.strip() for line in runners.splitlines() if "$config" in line and not line.lstrip().startswith("#")]
    out.append(("logs: on the node nothing traces, and the configuration only goes down the runner's pipe (%s)" % uses,
                not TRACED.search(runners) and not TRACED.search(bootstrap)
                and uses == ['[ -n "$config" ] || { echo "node_runner: ERROR: no runner configuration on stdin" >&2; exit 1; }', "printf '%s\\n' \"$config\" |"]))
    out.append(("logs: the node job has no output at all, so no deployment id reaches one", "outputs" not in jobs["node"]))
    gated = "runner.environment == 'github-hosted'"
    sudo = [s.get("name") for s in build["steps"] if gated not in str(s.get("if", "")) and re.search(r"\bsudo\b", s.get("run") or "")]
    out.append(("node: no step that can run on it uses sudo, which it does not have (%s)" % sudo, not sudo))
    scripts = [p for p in list(HERE.glob("*.py")) + list(HERE.glob("*.sh")) + list(HERE.parent.glob("*.py"))
               if not p.name.startswith("test_") and p.name not in ("node_bootstrap.sh", "theta_node.py")]
    calling = [p.name for p in scripts if re.search(r"\bsudo\b", p.read_text(encoding="utf-8"))]
    out.append(("node: nor does a script its steps call (%s)" % calling, not calling))
    block = bootstrap.split("PACKAGES=(", 1)[1].split("\n)", 1)[0]
    packages = {word for line in block.splitlines() for word in line.split("#", 1)[0].split()}
    wanted = NODE_JOB_PACKAGES | {p for leg in legs.LEGS if leg.node for p in leg.extra_pkgs.split()}
    out.append(("node: what those steps installed with it, the bootstrap installs (missing: %s)" % sorted(wanted - packages), wanted <= packages))
    return out


def group_watch_and_stop(doc, jobs, build, steps) -> list:
    out = []
    missing = [n for n in WATCHES if n not in jobs]
    out.append(("watch: two jobs in a row, no third", not missing and "watch-3" not in jobs))
    if missing:
        return out
    out.append(("watch: the same steps in each", jobs["watch-1"]["steps"] == jobs["watch-2"]["steps"]))
    per_job = theta_node.STAGE_LIMIT_S // 60
    timeouts = [jobs[n]["timeout-minutes"] for n in WATCHES]
    build_limit = build["timeout-minutes"]
    out.append(("watch (finding 11): each stops by itself inside its job's limit and GitHub's 360 minutes",
                all(per_job < limit <= 355 for limit in timeouts)))
    stuck = theta_node.STUCK_S // 60
    out.append(("watch (finding 11): together they outlast a leg at its limit that started as late as its runner may, and one alone would not",
                2 * per_job >= build_limit + stuck + 30 and per_job < build_limit))
    out.append(("watch: a leg's limit and a node job fit the age at which the sweeper deletes a node",
                build_limit + jobs["node"]["timeout-minutes"] + stuck <= theta_node.GUARD_HOURS * 60))
    out.append(("watch: each survives a cancel and a crash of the one before; none follows a verdict",
                all(jobs[n]["if"].startswith("always()") and "needs.node.result == 'success'" in jobs[n]["if"] for n in WATCHES)
                and "needs.watch-1.outputs.verdict" in jobs["watch-2"]["if"]))
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
    out.append(("stop: its step outlasts Theta's silence and one last slow call, and fits in its job, so the script speaks and not a timeout",
                delete["timeout-minutes"] * 60 >= theta_node.OFF_ANSWER_LIMIT_S + 4 * 60 + 30
                and delete["timeout-minutes"] < stop["timeout-minutes"]))
    out.append(("stop: a stop-node gone red holds nothing up: neither the gate nor the publish needs it",
                "stop-node" not in jobs["gate"]["needs"] and "stop-node" not in jobs["publish"]["needs"]))
    core_ref = doc[True]["workflow_dispatch"]["inputs"]["core_ref"]
    text = WORKFLOW.read_text(encoding="utf-8")
    out.append(("core_ref: no branch by default: empty is main, in each place that reads it",
                core_ref["default"] == "" and "firefox-157" not in core_ref["description"]
                and text.count("inputs.core_ref") == text.count("inputs.core_ref || 'main'") >= 4))
    guide = HERE.parent.parent / "CLAUDE.md"
    if guide.exists():
        guide_text = guide.read_text(encoding="utf-8")
        out.append(("guide: says what the plan and publish.yml do: releases use the cache, core_ref has no branch of its own",
                    "sempre a freddo" not in guide_text and "la rifiuta" not in guide_text and "engine/firefox-157" not in guide_text))
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
    import mozconfig
    out.append(("CPU: the node's measured limits give it all its 48 threads, and the report exists", mozconfig.node_parallel(48, 67177832448) == 48 and "cpu-report" in tests))
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
    for group in (group_no_fallback, group_cache_sharing, group_secrets_and_labels, group_pull_requests, group_exposure,
                  group_watch_and_stop, group_node_steps):
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
