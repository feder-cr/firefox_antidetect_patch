"""The rules `engine.py check` enforces, each shown failing on the input it exists for.

The series is only useful while it stays a series: every folder listed and present,
each file of ours owned by one folder, and Mozilla's files carrying hooks rather than
our logic. These tests run the real check on the real repo (it must pass), then break
one rule at a time on a copy of the budget and prove the check notices.

    python -m pytest tools/tests
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import engine  # noqa: E402


def test_the_series_as_committed_breaks_no_rule():
    assert engine.problems() == []


def _with_budget(tmp_path, monkeypatch, change):
    budget = json.loads(engine.BUDGET.read_text(encoding="utf-8"))
    change(budget)
    path = tmp_path / "hook-budget.json"
    path.write_text(json.dumps(budget), encoding="utf-8")
    monkeypatch.setattr(engine, "BUDGET", path)
    return engine.problems()


def test_a_mozilla_file_that_grows_past_its_budget_fails(tmp_path, monkeypatch):
    def shrink(b):
        b["startupcache/StartupCache.h"] -= 1
    found = _with_budget(tmp_path, monkeypatch, shrink)
    assert any("startupcache/StartupCache.h" in p and "budget is" in p for p in found), found


def test_a_newly_patched_mozilla_file_needs_a_budget(tmp_path, monkeypatch):
    found = _with_budget(tmp_path, monkeypatch, lambda b: b.pop("startupcache/StartupCache.h"))
    assert any("no hook budget" in p for p in found), found


def test_a_budget_for_a_file_no_longer_patched_must_go(tmp_path, monkeypatch):
    found = _with_budget(tmp_path, monkeypatch, lambda b: b.__setitem__("dom/base/Gone.cpp", 3))
    assert any("dom/base/Gone.cpp" in p and "remove it" in p for p in found), found


def test_series_must_list_exactly_the_folders_on_disk(tmp_path, monkeypatch):
    listed = engine.series_folders()
    monkeypatch.setattr(engine, "series_folders", lambda: listed[1:])
    assert any("SERIES and the folders differ" in p for p in engine.problems())


def test_the_budget_only_goes_down(tmp_path, monkeypatch):
    budget = json.loads(engine.BUDGET.read_text(encoding="utf-8"))
    budget["startupcache/StartupCache.h"] += 40
    path = tmp_path / "hook-budget.json"
    path.write_text(json.dumps(budget), encoding="utf-8")
    monkeypatch.setattr(engine, "BUDGET", path)
    monkeypatch.setattr(engine, "REMOVED_BUDGET", _copy_removed_budget(tmp_path, lambda b: None))
    engine.shrink_budget()
    after = json.loads(path.read_text(encoding="utf-8"))
    assert after["startupcache/StartupCache.h"] == budget["startupcache/StartupCache.h"] - 40


# The second budget: Mozilla's lines we remove or rewrite. Such a line conflicts at a
# port like one we add, and a copy of it moved into a file of ours stops following
# Mozilla (InitializeFamily's cmap loader, 2026-10-30).
FILE = "gfx/thebes/gfxPlatformFontList.cpp"


def _copy_removed_budget(tmp_path, change):
    budget = json.loads(engine.REMOVED_BUDGET.read_text(encoding="utf-8"))
    change(budget)
    path = tmp_path / "removed-budget.json"
    path.write_text(json.dumps(budget), encoding="utf-8")
    return path


def test_a_mozilla_file_losing_more_lines_than_its_removed_budget_fails(tmp_path, monkeypatch):
    def shrink(b):
        b[FILE] -= 1
    monkeypatch.setattr(engine, "REMOVED_BUDGET", _copy_removed_budget(tmp_path, shrink))
    found = engine.problems()
    assert any(FILE in p and "removed budget is" in p for p in found), found


def test_a_file_without_a_removed_budget_may_remove_nothing(tmp_path, monkeypatch):
    monkeypatch.setattr(engine, "REMOVED_BUDGET", _copy_removed_budget(tmp_path, lambda b: b.pop(FILE)))
    found = engine.problems()
    assert any(FILE in p and "removed budget is 0" in p for p in found), found


def test_a_removed_budget_for_a_file_no_longer_patched_must_go(tmp_path, monkeypatch):
    path = _copy_removed_budget(tmp_path, lambda b: b.__setitem__("dom/base/Gone.cpp", 3))
    monkeypatch.setattr(engine, "REMOVED_BUDGET", path)
    found = engine.problems()
    assert any("removed-budget.json" in p and "dom/base/Gone.cpp" in p for p in found), found


def test_the_removed_budget_only_goes_down_and_drops_files_at_zero(tmp_path, monkeypatch):
    def loosen(b):
        b[FILE] += 40
        b["startupcache/StartupCache.h"] = 7  # patched, removes nothing
    path = _copy_removed_budget(tmp_path, loosen)
    monkeypatch.setattr(engine, "REMOVED_BUDGET", path)
    hook = tmp_path / "hook-budget.json"
    hook.write_bytes(engine.BUDGET.read_bytes())
    monkeypatch.setattr(engine, "BUDGET", hook)
    engine.shrink_budget()
    after = json.loads(path.read_text(encoding="utf-8"))
    real = json.loads(engine.REMOVED_BUDGET.read_text(encoding="utf-8"))
    assert after[FILE] == real[FILE]
    assert "startupcache/StartupCache.h" not in after


def test_lines_are_counted_inside_hunks_only(tmp_path):
    patch = tmp_path / "hooks.patch"
    patch.write_bytes(b"diff --git a/x.sql b/x.sql\n--- a/x.sql\n+++ b/x.sql\n"
                      b"@@ -1,2 +1,2 @@\n--- a comment\n+++ a line\n keep\n")
    assert engine.removed_lines(patch) == {"x.sql": 1}
    assert engine.added_lines(patch) == {"x.sql": 1}


def test_a_root_workflow_edited_by_hand_fails(tmp_path, monkeypatch):
    root = tmp_path / "wf"
    root.mkdir()
    for p in engine.WORKFLOWS_ROOT.glob("*.yml"):
        (root / p.name).write_bytes(p.read_bytes())
    (root / "release.yml").write_bytes((root / "release.yml").read_bytes() + b"# by hand\n")
    monkeypatch.setattr(engine, "WORKFLOWS_ROOT", root)
    assert any("release.yml differs" in p for p in engine.problems())
