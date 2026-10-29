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
    engine.shrink_budget()
    after = json.loads(path.read_text(encoding="utf-8"))
    assert after["startupcache/StartupCache.h"] == budget["startupcache/StartupCache.h"] - 40
