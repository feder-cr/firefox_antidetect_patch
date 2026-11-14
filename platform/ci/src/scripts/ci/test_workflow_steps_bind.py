#!/usr/bin/env python3
"""The steps that are there to STOP a run bind: they are read as YAML, by their keys, not as text.

The tests of the checks, the guard, the reuse, the GeoIP database and the protocol mirror compared the text of a step (its `run`, its `name`). A step
whose text is right and whose KEYS make it harmless passed them all: `continue-on-error: true` on the mirror check, `if: matrix.kind == 'linux'` on the
database step (the Windows legs launch without it), `|| true` on the verify line, `exit 1` turned into `exit 0` after the guard's sleep. Each of those was
tried on a copy of the workflows and the old tests stayed green; these are red under each of them.

For every step named here: no `continue-on-error`; the `if` it has is the one written below (none, when none is written); no `|| true` and no `set +e`
unless the step says it needs one; and, for the ones that carry a rule in their script, the rule.

    python3 scripts/ci/test_workflow_steps_bind.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORKFLOWS = HERE.parent.parent / ".github" / "workflows"
GEOIP_BUILD = "python scripts/ci_lab_geoip.py build --pin ci/geoip.json"
GEOIP_STEP = "The GeoIP database (made by the repository, same bytes every run)"
VERIFY_STEP = "The leg ran with the pinned GeoIP database"
VERDICT_STEP = "Verdict - required suite"
MIRROR_STEP = "The core's protocol mirror"
GUARD_STEP = "Still the head of the branch"
REUSE_STEP = "A green run built this tree already (a push to main only)"
CHECK_STEPS = ("Check asset names match the version this tree builds",
               "Check the declared upstream source matches the version this tree builds",
               "Juggler's tests without a browser",
               "The release scripts refuse what they must")


def load(name):
    import yaml

    return yaml.safe_load((WORKFLOWS / name).read_text(encoding="utf-8"))


def step_of(doc, job, name):
    """The one step of `job` whose name starts with `name` (KeyError / StopIteration are a FAIL that names it, see `cases`)."""
    found = [s for s in doc["jobs"][job]["steps"] if str(s.get("name", "")).startswith(name)]
    if len(found) != 1:
        raise KeyError("%s/%s: %d steps start with %r" % (doc.get("name"), job, len(found), name))
    return found[0]


def binding(step, expected_if=None):
    """Why a step does NOT bind, or '' when it does: no continue-on-error, the `if` it is written with, no `|| true`, no `set +e`."""
    if "continue-on-error" in step:
        return "continue-on-error: %r" % (step["continue-on-error"],)
    if step.get("if") != expected_if:
        return "if: %r instead of %r" % (step.get("if"), expected_if)
    run = step.get("run", "")
    if re.search(r"\|\|\s*true\b", run):
        return "`|| true` in its script"
    if re.search(r"^\s*set \+e\b", run, re.M):
        return "`set +e` in its script"
    return ""


def _cases() -> list:
    out = []
    release, gate = load("release.yml"), load("gate.yml")

    def add(name, reason_or_bool):
        ok = reason_or_bool is True or reason_or_bool == ""
        out.append((name + ("" if ok else " [%s]" % reason_or_bool), ok))

    # --- release.yml: the job `checks`, the guard, the reuse ---------------------------------------------------------------------------------------
    for name in CHECK_STEPS:
        add("checks: the step %r binds (no continue-on-error, no `if`, no `|| true`)" % name, binding(step_of(release, "checks", name)))
    guard = step_of(release, "checks", GUARD_STEP)
    # the guard's script reads the exit code of still_head.py after `set +e`, so `binding` is not asked about that line: the rest of it is
    run = guard["run"]
    add("guard: the step binds (no `if`, no continue-on-error, no `|| true`)",
        "continue-on-error" not in guard and "if" not in guard and re.search(r"\|\|\s*true", run) is None)
    add("guard: a superseded run (exit code 1) cancels itself, waits for the cancellation, and then FAILS: `checks` is never green for it",
        re.search(r'if \[ "\$CODE" = 1 \]; then\s+gh run cancel "\$GITHUB_RUN_ID".*\n\s+sleep \d+\s+exit 1\s+fi', run) is not None)
    add("guard: any other code (go on, could not read) ends in exit 0", run.rstrip().endswith("exit 0"))
    add("guard: it is the LAST step of `checks`", release["jobs"]["checks"]["steps"][-1] is guard)
    add("guard: the node still needs `checks`", "checks" in release["jobs"]["node"]["needs"])
    reuse = step_of(release, "plan", REUSE_STEP)
    add("reuse: the step binds", binding(reuse, None))

    # --- gate.yml: the GeoIP database and the protocol mirror ---------------------------------------------------------------------------------------
    for job in ("gate", "wrapper-e2e"):
        step = step_of(gate, job, GEOIP_STEP)
        add("geoip: %s makes the database on EVERY leg (no `if`, no continue-on-error) with the pin of the checkout" % job,
            binding(step, None) or ("" if step["run"].strip() == GEOIP_BUILD else "script: %r" % step["run"]))
        verify = step_of(gate, job, VERIFY_STEP) if job == "wrapper-e2e" else None
    verify = step_of(gate, "wrapper-e2e", VERIFY_STEP)
    add("geoip: the verification of the evidence binds, and runs whatever pytest said (`if: always()`)", binding(verify, "always()"))
    add("geoip: the verification reads the pin of MAIN when main has one, and calls the script's `verify` with the evidence",
        'GEOIP_PIN=ci/geoip.json\n[ -f wrapper-main/ci/geoip.json ] && GEOIP_PIN=wrapper-main/ci/geoip.json' in "\n".join(l.strip() for l in verify["run"].splitlines())
        and 'ci_lab_geoip.py verify --pin "$GEOIP_PIN" --evidence geoip-evidence.json' in verify["run"])
    verdict = step_of(gate, "wrapper-e2e", VERDICT_STEP)
    add("geoip: the verdict step binds", binding(verdict, None))
    add("geoip: the judge is handed the evidence and the pin of MAIN's checkout when main has one",
        "[ -f wrapper-main/ci/geoip.json ] && GEOIP_PIN=wrapper-main/ci/geoip.json" in verdict["run"]
        and '--geoip geoip-evidence.json --geoip-pin "$GEOIP_PIN"' in verdict["run"])
    mirror = step_of(gate, "gate", MIRROR_STEP)
    add("protocol mirror: the step binds, and runs on the Linux leg only", binding(mirror, "matrix.leg == 'linux-x86_64'"))
    add("protocol mirror: it calls `--check` against the binary", "_gen_protocol --check --binary" in mirror["run"])
    return out


def cases() -> list:
    """The cases; a workflow that lacks what they read is a FAIL that names it, not a crash of the caller."""
    try:
        return _cases()
    except (KeyError, IndexError, TypeError, AttributeError, StopIteration) as exc:
        return [("workflow steps: stopped on %s: %s (the workflow lacks what these cases read)" % (type(exc).__name__, exc), False)]


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
