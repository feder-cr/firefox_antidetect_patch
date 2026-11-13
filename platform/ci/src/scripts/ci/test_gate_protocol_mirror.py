"""The gate holds the protocol mirror of the core under test to the Protocol.js of the binary under test, before the core can be published.

Core 63.66.0 was published with two hand-edited fields in `protocol.py` (Page.getFrameOrigin.returns.ex/ey) and the wrapper's e2e, the only place that ran
`_gen_protocol --check`, ran it after the core was out and with the core from the index. The `gate` job of gate.yml has both things in hand - the binary
(FF_EXE) and the core under test (core_ref, from git) - and `publish` waits for it: the step stands there, once, on the Linux leg (Protocol.js is the same
file on every leg), after the core under test is installed and before the drive gate.

    python -m pytest scripts/ci/test_gate_protocol_mirror.py
"""
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = yaml.safe_load((ROOT / ".github" / "workflows" / "gate.yml").read_text(encoding="utf-8"))
CHECK = 'python -m invisible_core.juggler._gen_protocol --check --binary "$(dirname "$FF_EXE")"'


def _steps():
    return WORKFLOW["jobs"]["gate"]["steps"]


def _index(prefix):
    return next(i for i, s in enumerate(_steps()) if s.get("name", "").startswith(prefix))


def test_the_gate_checks_the_mirror_of_the_core_under_test_against_the_binary():
    steps = [s for s in _steps() if "_gen_protocol" in s.get("run", "")]
    assert len(steps) == 1, "the mirror check appears in %d steps of the gate" % len(steps)
    assert steps[0]["run"].strip() == CHECK and steps[0]["shell"] == "bash"


def test_it_runs_on_one_leg_the_linux_one():
    step = _steps()[_index("The core's protocol mirror")]
    assert step["if"] == "matrix.leg == 'linux-x86_64'"
    assert any(leg["leg"] == "linux-x86_64" for leg in WORKFLOW["jobs"]["gate"]["strategy"]["matrix"]["include"])


def test_it_comes_after_the_core_under_test_is_installed_and_before_the_binary_is_driven():
    installed = _index("The core under test (core_ref, from git)")
    checked = _index("The core's protocol mirror")
    driven = _index("DRIVE GATE")
    assert installed < checked < driven


def test_the_publication_waits_for_the_gate_that_holds_it():
    release = yaml.safe_load((ROOT / ".github" / "workflows" / "release.yml").read_text(encoding="utf-8"))
    gate = release["jobs"]["gate"]
    assert gate["uses"] == "./.github/workflows/gate.yml"
    assert "gate" in release["jobs"]["publish"]["needs"]


def cases() -> list:
    """The tests above as (name, ok) pairs, for scripts/ci/test_ci.py, which the plan job runs (a test that no job runs is deleted)."""
    found = []
    for name, function in sorted(globals().items()):
        if name.startswith("test_") and callable(function):
            try:
                function()
                ok = True
            except (AssertionError, StopIteration, KeyError):
                ok = False
            found.append(("gate protocol mirror: " + name[len("test_"):].replace("_", " "), ok))
    return found


if __name__ == "__main__":
    import sys

    results = cases()
    for label, ok in results:
        print(("ok    " if ok else "FAIL  ") + label)
    print("%d/%d cases as expected" % (sum(ok for _, ok in results), len(results)))
    sys.exit(0 if all(ok for _, ok in results) else 1)
