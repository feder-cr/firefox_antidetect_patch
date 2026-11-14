"""Every leg of the gate that runs the wrapper's tests launches with the GeoIP database that the wrapper's scripts/ci_lab_geoip.py makes (ci/geoip.json), and says so to the judge.

Run 43211837926: without STEALTHFOX_GEOIP_MMDB the core asked GitHub which database is the latest on every call, from the python of the test, and the
resolver trap of the realness suite counted github.com and api.github.com as the browser's. The two jobs that launch the wrapper (`gate`, the legs
of the engine, and `wrapper-e2e`, the suites of the list) make the database (scripts/ci_lab_geoip.py of the wrapper, no download) before their first step that runs the
wrapper; the suite step writes what the environment of its launch says (through the same sudo as pytest, on a root leg) and the verdict hands it to
the judge, which turns a leg that ran without the database red.

    python -m pytest scripts/ci/test_gate_geoip.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GATE = (ROOT / ".github" / "workflows" / "gate.yml").read_text(encoding="utf-8")
FETCH = "python scripts/ci_lab_geoip.py build --pin ci/geoip.json"
STEP = "      - name: The GeoIP database (made by the repository, same bytes every run)\n"


def _job(name):
    start = GATE.index("\n  %s:\n" % name)
    ends = [GATE.find("\n  %s:\n" % other, start + 1) for other in ("wrapper-e2e", "wrapper-e2e-complete", "host-gate")]
    ends = [e for e in ends if e > start]
    return GATE[start:min(ends)] if ends else GATE[start:]


def test_the_legs_that_run_the_wrapper_make_the_database_before_they_run_it():
    for name, first_use in (("gate", "      - name: Install Playwright driver"), ("wrapper-e2e", "      - name: Linux system deps")):
        job = _job(name)
        assert job.count(FETCH) == 1, "%s makes the GeoIP database %d times" % (name, job.count(FETCH))
        assert job.index(STEP) < job.index(first_use), "%s makes the database after the wrapper is installed or run" % name
        assert job.index("Set up Python") < job.index(STEP), "%s makes the database before Python is set up" % name


def test_the_jobs_that_only_judge_do_not_make_it():
    assert STEP not in _job("host-gate") and STEP not in _job("wrapper-e2e-complete")
    assert GATE.count(STEP) == 2


def test_the_suite_writes_the_evidence_in_the_environment_pytest_is_launched_in():
    root_launch = 'sudo -H -E env -u PYTEST_ADDOPTS -u XDG_RUNTIME_DIR "PATH=$PATH" python scripts/ci_lab_geoip.py evidence --out geoip-evidence.json'
    other = "env -u PYTEST_ADDOPTS python scripts/ci_lab_geoip.py evidence --out geoip-evidence.json"
    assert GATE.count(root_launch) == 1
    assert GATE.count(other) == 1
    # both before the line that starts pytest in the same branch
    assert GATE.index(root_launch) < GATE.index('INVPW_REQUIRE_TRAP=1 "PATH=$PATH" "${PYTEST[@]}"')
    assert GATE.index(other) < GATE.index('env -u PYTEST_ADDOPTS "${PYTEST[@]}"')


def test_the_verdict_hands_the_evidence_and_the_pin_to_the_judge():
    assert '"${MAIN[@]}" "${GEOIP[@]}" --out' in GATE
    assert "GEOIP=(--geoip geoip-evidence.json --geoip-pin " in GATE
    assert "wrapper-main/ci/geoip.json" in GATE, "the pin of main binds the candidate, as the judge and the list do"
    assert re.search(r"if grep -q -- '--geoip-pin' \"\$JUDGE\"", GATE), "a judge that does not read the evidence yet is said, loudly"


def test_the_leg_verifies_that_it_ran_with_the_pinned_database_before_the_verdict_and_whatever_the_judge_knows():
    step = "      - name: The leg ran with the pinned GeoIP database\n        if: always()\n"
    assert GATE.count(step) == 1
    assert 'python scripts/ci_lab_geoip.py verify --pin "$GEOIP_PIN" --evidence geoip-evidence.json --used "$GEOIP_USED_OUT"' in GATE
    assert GATE.index("      - name: The suite - every listed test runs and passes\n") < GATE.index(step) < GATE.index("      - name: Verdict - required suite\n")



def test_the_evidence_is_not_in_the_uploaded_result_folder():
    """The aggregate judge reads every JSON file of the uploaded result folders as a result: the GeoIP evidence written
    into result/ was refused as "not a result of judge" on every leg (candidate 43505101105), so it lives beside it."""
    assert "result/geoip.json" not in GATE
    assert "evidence --out geoip-evidence.json" in GATE

def cases() -> list:
    """The tests above as (name, ok) pairs, for scripts/ci/test_ci.py, which the plan job runs (a test that no job runs is deleted)."""
    found = []
    for name, function in sorted(globals().items()):
        if name.startswith("test_") and callable(function):
            try:
                function()
                ok = True
            except AssertionError:
                ok = False
            found.append(("gate: " + name[len("test_"):].replace("_", " "), ok))
    return found


if __name__ == "__main__":
    import sys

    results = cases()
    for label, ok in results:
        print(("ok    " if ok else "FAIL  ") + label)
    print("%d/%d cases as expected" % (sum(ok for _, ok in results), len(results)))
    sys.exit(0 if all(ok for _, ok in results) else 1)
