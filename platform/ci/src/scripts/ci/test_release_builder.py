#!/usr/bin/env python3
"""builder=github leaves a run of release.yml exactly as it is on GitHub's runners,
and builder=theta sends only the x86_64 legs' compilation to the node.

Everything the node added to the workflow is taken back out by hosted_view(), by
exact text, and the fingerprint of what remains is fixed here. A change to the
hosted path changes the fingerprint and fails this test: that is a decision, and it
changes HOSTED_VIEW_SHA256 with it, as a flag does in test_ci.py.

    python3 scripts/ci/test_release_builder.py
    python3 scripts/ci/test_release_builder.py --print   # the fingerprint and the view
"""
from __future__ import annotations

import copy
import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import legs  # noqa: E402

WORKFLOW = HERE.parent.parent / ".github" / "workflows" / "release.yml"
HOSTED_VIEW_SHA256 = "fc579c42cc9d691802709eea8051e5c71dffb7dfc65a0593dc3b41e147d8301e"

ON_HOSTED = " && runner.environment == 'github-hosted'"
NOT_ON_HOSTED = "runner.environment != 'github-hosted'"
ONLY_THETA = "needs.plan.outputs.builder == 'theta'"
RUNS_ON = ("${{ (matrix.node && needs.node.outputs.attempt == github.run_attempt && "
           "needs.node.outputs.runner) || matrix.runner }}")
SAVE_GUARD = " && steps.cache-worth.outputs.save != 'false'"
SCCACHE_BIN_NODE = "${{ runner.environment == 'github-hosted' && '' || '-node' }}"
NEW_JOBS = ("node", "stop-node", "cache-trim", "gtest-linux")


def load(text: str) -> dict:
    import yaml
    return yaml.safe_load(text)


def hosted_view(doc: dict) -> dict:
    """The workflow as a run with builder=github on GitHub's runners sees it."""
    view = copy.deepcopy(doc)
    view[True]["workflow_dispatch"]["inputs"].pop("builder", None)
    for job in NEW_JOBS:
        view["jobs"].pop(job, None)
    plan = view["jobs"]["plan"]
    plan["outputs"].pop("builder", None)
    for step in plan["steps"]:
        if step.get("id") == "plan":
            step["env"].pop("BUILDER", None)
            step["run"] = step["run"].replace(' --builder "$BUILDER"', "")
    build = view["jobs"]["build"]
    build["needs"] = "plan" if build["needs"] == ["plan", "node"] else build["needs"]
    if build["runs-on"] == RUNS_ON:
        build["runs-on"] = "${{ matrix.runner }}"
    build.pop("permissions", None)
    kept = []
    for step in build["steps"]:
        cond = step.get("if", "")
        if NOT_ON_HOSTED in cond or ONLY_THETA in cond:
            continue
        step = dict(step)
        if "if" in step:
            if cond == "runner.environment == 'github-hosted'":
                del step["if"]
            else:
                step["if"] = cond.replace(ON_HOSTED, "").replace(SAVE_GUARD, "")
        key = (step.get("with") or {}).get("key")
        if key and SCCACHE_BIN_NODE in key:
            step["with"] = dict(step["with"], key=key.replace(SCCACHE_BIN_NODE, ""))
        kept.append(step)
    build["steps"] = kept
    return view


def as_json(doc: dict, **kwargs) -> str:
    # YAML reads the key `on` as true.
    return json.dumps({str(k): v for k, v in doc.items()}, sort_keys=True, default=str, ensure_ascii=False, **kwargs)


def fingerprint(doc: dict) -> str:
    text = as_json(hosted_view(doc))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def cases() -> list:
    try:
        doc = load(WORKFLOW.read_text(encoding="utf-8"))
    except ImportError:
        return [("builder: the workflow can be read (PyYAML is missing)", False)]
    out = []
    inputs = doc[True]["workflow_dispatch"]["inputs"]
    out.append(("builder: an input, theta by default, github to go back",
                inputs.get("builder", {}).get("default") == "theta"
                and inputs.get("builder", {}).get("options") == ["theta", "github"]))
    out.append(("builder=github: what runs on GitHub's runners is what was fixed, step by step (%s)" % fingerprint(doc)[:12],
                fingerprint(doc) == HOSTED_VIEW_SHA256))
    jobs = doc["jobs"]
    build = jobs.get("build", {})
    out.append(("builder=github: the legs' runner is the matrix's when the node job said nothing",
                build.get("runs-on") == RUNS_ON))
    node_steps = jobs.get("node", {}).get("steps", [])
    out.append(("builder=github: the node job runs and does nothing (a skipped ancestor would skip gate and publish)",
                "if" not in jobs.get("node", {}) and bool(node_steps)
                and all(s.get("if") == ONLY_THETA for s in node_steps)))
    for name in ("stop-node", "cache-trim"):
        out.append(("builder=github: %s is skipped, and never an ancestor of gate or publish" % name,
                    ONLY_THETA in jobs.get(name, {}).get("if", "")
                    and not any(name in str(j.get("needs", "")) for j in jobs.values())))
    out.append(("builder=github: no node, so no Linux gtest job for one",
                "needs.node.outputs.runner != ''" in jobs.get("gtest-linux", {}).get("if", "")))
    out.append(("builder=github: no secret on any job of the hosted path",
                "secrets." not in json.dumps({k: v for k, v in hosted_view(doc)["jobs"].items()})))

    mutated = copy.deepcopy(doc)
    mutated["jobs"]["build"]["steps"][0]["run"] += "\necho changed\n"
    out.append(("the fingerprint sees a changed step", fingerprint(mutated) != fingerprint(doc)))
    mutated = copy.deepcopy(doc)
    mutated["jobs"]["build"]["steps"].pop(1)
    out.append(("the fingerprint sees a step gone", fingerprint(mutated) != fingerprint(doc)))
    mutated = copy.deepcopy(doc)
    mutated["jobs"]["gate"]["needs"] = ["plan"]
    out.append(("the fingerprint sees another job change", fingerprint(mutated) != fingerprint(doc)))

    out.append(("theta: arm64 stays on GitHub, the x86_64 legs go to the node",
                {l.leg: l.node for l in legs.LEGS} == {"linux-x86_64": True, "linux-arm64": False, "win-x86_64": True}))
    out.append(("theta: the matrix tells the job", all("node" in e for e in json.loads(legs.matrix("1.0"))["include"])))
    steps = {s.get("name", ""): s for s in build.get("steps", [])}
    swap = steps.get("Free disk + 32G swap (Linux runners)", {})
    out.append(("theta: no disk cleaning and no swap on the node", swap.get("if") == "runner.environment == 'github-hosted'"))
    restore = steps.get("Compile cache - restore this leg's last one", {})
    save = steps.get("Compile cache - save this leg's", {})
    out.append(("theta: the compile cache on the node, with the same key as on GitHub's runners",
                "runner.environment" not in restore.get("if", "")
                and restore.get("with", {}).get("key") == "sccache-${{ matrix.leg }}-${{ github.run_id }}-${{ github.run_attempt }}"
                and restore.get("with", {}).get("restore-keys") == "sccache-${{ matrix.leg }}-"
                and "runner.environment" not in save.get("if", "")
                and save.get("with", {}).get("key") == restore.get("with", {}).get("key")))
    out.append(("theta: a save only if worth the upload, and never of a build that did not start",
                SAVE_GUARD in save.get("if", "") and "steps.build.outcome" in save.get("if", "")
                and "cache_rules.py worth" in steps.get("Compile cache - is the node's upload worth it", {}).get("run", "")))
    binary = steps.get("Compile cache - the sccache binary, if built before", {})
    out.append(("theta: the node's sccache binary is its own entry: glibc 2.35 cannot run one built on 24.04",
                SCCACHE_BIN_NODE in binary.get("with", {}).get("key", "")))
    tool_restore, tool_save = steps.get("Toolchains - restore", {}), steps.get("Toolchains - save", {})
    names = list(steps)
    out.append(("theta: the toolchains have their own entry, by their versions, restored before Rust and the build",
                "~/.mozbuild" in tool_restore.get("with", {}).get("path", "")
                and tool_restore.get("with", {}).get("key") == tool_save.get("with", {}).get("key")
                and "rust$RUST" in steps.get("Toolchains - their versions", {}).get("run", "")
                and names.index("Toolchains - restore") < names.index("Rust, the retail's release for this tree") < names.index("Build")))
    out.append(("theta: a toolchain entry only from a build that worked", "steps.build.outcome == 'success'" in tool_save.get("if", "")))
    out.append(("theta: the node's parallelism only on the node",
                steps.get("The node - compile jobs from its limits", {}).get("if") == NOT_ON_HOSTED))
    out.append(("theta: the build job's token reads, no more", build.get("permissions") == {"contents": "read"}))
    gtests = steps.get("The node - its gtest binaries, for a job on GitHub's runners", {})
    out.append(("theta: the node only compiles its gtests, and a job on GitHub's runners runs Linux's",
                "recurse_gtest" in gtests.get("run", "") and "mach gtest" not in gtests.get("run", "")
                and jobs.get("gtest-linux", {}).get("runs-on") == "ubuntu-24.04"
                and any(s.get("with", {}).get("name") == "gtest-bin-linux-x86_64" for s in jobs.get("gtest-linux", {}).get("steps", []))))
    stop = jobs.get("stop-node", {})
    out.append(("theta: stop-node follows a cancel and a failed node job, and does not wait for arm64",
                stop.get("if", "").startswith("always()") and stop.get("needs") == ["plan", "node"]
                and all(s.get("if", "").startswith("always()") for s in stop.get("steps", []))))
    out.append(("theta: the delete is unconditional and the watch has a limit below the job's",
                any(s.get("if") == "always()" and " off " in s.get("run", "") for s in stop.get("steps", []))
                and any(s.get("timeout-minutes", 999) < stop.get("timeout-minutes", 0) for s in stop.get("steps", []) if "watch" in s.get("run", ""))))
    secret_jobs = sorted(n for n, j in jobs.items() if "secrets." in json.dumps(j))
    out.append(("theta: secrets only on the jobs that run on GitHub's runners (%s)" % ", ".join(secret_jobs),
                secret_jobs == ["node", "stop-node"]
                and all(jobs[n]["runs-on"] == "ubuntu-24.04" for n in secret_jobs)))
    trim = jobs.get("cache-trim", {})
    out.append(("cache-trim: on GitHub's runners with actions: write; the build job never has it",
                trim.get("runs-on") == "ubuntu-24.04" and trim.get("permissions", {}).get("actions") == "write"
                and "actions" not in build.get("permissions", {})))
    sweeper = load((WORKFLOW.parent / "theta-sweeper.yml").read_text(encoding="utf-8"))
    out.append(("sweeper: scheduled, and only the sweep", "schedule" in sweeper[True]
                and "theta_node.py sweep" in sweeper["jobs"]["sweep"]["steps"][-1]["run"]))
    publish = (WORKFLOW.parent / "publish.yml").read_text(encoding="utf-8")
    out.append(("publish: no refusal of a cached build, and nothing reads a builder record",
                "compile-cache" not in publish and "builder.txt" not in publish))
    return out


def main() -> int:
    if sys.argv[1:] == ["--print"]:
        doc = load(WORKFLOW.read_text(encoding="utf-8"))
        print(fingerprint(doc))
        print(as_json(hosted_view(doc), indent=1))
        return 0
    got = cases()
    failures = 0
    for name, ok in got:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(got) - failures, len(got)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
