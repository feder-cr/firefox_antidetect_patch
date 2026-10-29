"""`engine.py port --to <tag>`: every folder replayed three-way onto the new Mozilla
tag in one run, every conflict reported, none stopping the run; `drift` lists the
patched Mozilla files Mozilla changed; export and verify refuse a branch not built on
the tag.

Shown on the two-file repository of test_apply_builds_the_layer_stack.py, with a new
tag that moves the line our hook sits next to (clean) and one that rewrites it
(conflict).

    python -m pytest tools/tests
"""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import engine  # noqa: E402
from test_apply_builds_the_layer_stack import run, world  # noqa: E402,F401


def new_tag(src, name, content):
    run(src, "checkout", "-q", "-B", "up-" + name, "TAG")
    (src / "mozilla.txt").write_bytes(content)
    run(src, "commit", "-qam", "upstream " + name)
    run(src, "tag", name)


def test_a_port_onto_a_tag_that_moved_nearby_merges_cleanly(world):
    new_tag(world, "NEW", b"zero\none\ntwo\n")
    assert engine.port(world, "TAG", "NEW", "port/NEW", force=False) == 0
    assert run(world, "show", "port/NEW:mozilla.txt") == "zero\none\nhook();\ntwo"
    assert run(world, "log", "--format=%s", "NEW..port/NEW").splitlines() == ["fingerprint/a", "platform/b"]


def test_a_port_reports_a_conflict_and_still_writes_every_folder(world, capsys):
    new_tag(world, "NEW", b"one\nTWO CHANGED\n")
    assert engine.port(world, "TAG", "NEW", "port/NEW", force=False) == 1
    out = capsys.readouterr().out
    assert "CONFLICT fingerprint/a: mozilla.txt" in out
    titles = run(world, "log", "--format=%s", "NEW..port/NEW").splitlines()
    assert titles == ["fingerprint/a (CONFLICT: mozilla.txt)", "platform/b"]
    assert "<<<<<<<" in run(world, "show", "port/NEW:mozilla.txt")
    assert run(world, "show", "port/NEW:dom/StealthA.cpp") == "// ours", "our own files come through"


def test_a_port_needs_both_tags(world):
    with pytest.raises(SystemExit):
        engine.port(world, "TAG", "NOPE", "port/NOPE", force=False)


def test_drift_lists_the_patched_files_mozilla_changed(world, tmp_path, monkeypatch, capsys):
    new_tag(world, "NEW", b"zero\none\ntwo\n")
    inv = tmp_path / "inventory.json"
    inv.write_text('[{"file": "mozilla.txt"}, {"file": "untouched.txt"}]')
    monkeypatch.setattr(engine, "INVENTORY", inv)
    engine.drift(world, "TAG", "NEW")
    out = capsys.readouterr().out
    assert "changed mozilla.txt" in out and "1 of our 2 patched Mozilla files changed" in out


def test_export_and_verify_refuse_a_branch_not_built_on_the_tag(world):
    run(world, "checkout", "-q", "--orphan", "elsewhere")
    run(world, "commit", "-q", "--allow-empty", "-m", "unrelated")
    with pytest.raises(SystemExit):
        engine.verify(world, "TAG", "elsewhere")
    with pytest.raises(SystemExit):
        engine.export(world, "TAG", "elsewhere")
