"""The first run is declared by the core, never decided by the engine.

A person's browser accepted the terms of use long ago: the core declares that
state on every host (invisible_core prefs.py: browser.preonboarding.enabled
false, termsofuse.acceptedVersion and termsofuse.acceptedDate, written to
user.js before the window exists). Until 2026-11-02 the engine also carried it
as compiled defaults (STEALTH-HOOK lines in firefox.js and all.js), a second
source that could disagree with the first. The engine now leaves Mozilla's
lines as Mozilla wrote them: no folder, hook or file of ours, sets these prefs.

    python -m pytest tools/tests
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

DECLARED_BY_THE_CORE = (
    "browser.preonboarding.enabled",
    "termsofuse.acceptedVersion",
    "termsofuse.acceptedDate",
    "datareporting.policy.dataSubmissionPolicyBypassNotification",
)


def _folders():
    return [p.parent for p in ROOT.rglob("hooks.patch") if ".git" not in p.parts]


def test_no_hook_changes_a_first_run_pref():
    found = []
    for folder in _folders():
        patch = folder / "hooks.patch"
        for n, line in enumerate(patch.read_text(encoding="utf-8").splitlines(), 1):
            if not line.startswith(("+", "-")) or line.startswith(("+++", "---")):
                continue
            for pref in DECLARED_BY_THE_CORE:
                if '"%s"' % pref in line:
                    found.append("%s:%d %s" % (patch.relative_to(ROOT), n, line.strip()))
    assert found == []


def test_no_file_of_ours_sets_a_first_run_pref():
    found = []
    for folder in _folders():
        src = folder / "src"
        if not src.is_dir():
            continue
        for path in src.rglob("*"):
            if not path.is_file():
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            for pref in DECLARED_BY_THE_CORE:
                if pref in text:
                    found.append("%s: %s" % (path.relative_to(ROOT), pref))
    assert found == []
