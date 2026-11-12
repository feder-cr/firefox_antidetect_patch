"""The export runs the tests of the build job first, on the whole tree, and refuses when one is red.

Run 43156862361 died a minute after it started on six node tests of the Juggler that nobody had run: the tree they were tried on
was a sparse one, and four of them read files it does not have (tools/export_gate.py says the rest). These tests hold the gate to
what it says: it is on the path of `engine.py export`, it needs Node, it needs the whole tree, and a red test stops the export.

    python -m pytest tools/tests
"""
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import engine  # noqa: E402
import export_gate  # noqa: E402


def test_the_export_of_the_command_line_goes_through_the_gate_before_it_writes():
    text = (Path(engine.__file__)).read_text(encoding="utf-8")
    branch = text[text.index('if a.command == "export":'):]
    assert branch.index("export_gate.gate(") < branch.index("export(a.source, base, head)"), \
        "engine.py export must run the gate before it writes anything"


def test_a_machine_without_node_refuses_the_export(monkeypatch):
    monkeypatch.delenv("ENGINE_NODE", raising=False)
    monkeypatch.setattr(export_gate, "PORTABLE_NODE", "C:/no/such/node.exe")
    monkeypatch.setattr(export_gate.shutil, "which", lambda name: None)
    with pytest.raises(SystemExit, match="no Node"):
        export_gate.node_binary()


def test_a_failing_node_test_is_named(monkeypatch, tmp_path):
    (tmp_path / "juggler" / "tests").mkdir(parents=True)
    (tmp_path / "juggler" / "tests" / "a.test.mjs").write_text("", encoding="utf-8")

    class Done:
        returncode = 1
        stdout = "ok 1 - fine\nnot ok 2 - the flag has a bit of its own\n# tests 2\n# pass 1\n# fail 1\n"
        stderr = ""

    monkeypatch.setattr(export_gate.subprocess, "run", lambda *a, **k: Done())
    assert export_gate.run_node_tests(tmp_path, "node") == ["the flag has a bit of its own"]


def test_a_tree_that_is_not_whole_is_refused(monkeypatch, tmp_path):
    calls = []

    def fake_git(*args, cwd=None):
        calls.append(args)
        return "abc"

    monkeypatch.setattr(export_gate, "git", fake_git)
    (tmp_path / ".git").write_text("gitdir: x", encoding="utf-8")        # a worktree, but sparse: no netwerk/base
    (tmp_path / "juggler" / "tests").mkdir(parents=True)
    with pytest.raises(SystemExit, match="not a whole tree"):
        export_gate.prepare_tree(Path("src"), "head", tmp_path)


def test_a_red_gate_stops_the_export(monkeypatch):
    monkeypatch.setattr(export_gate, "node_binary", lambda: "node")
    monkeypatch.setattr(export_gate, "prepare_tree", lambda source, head: Path("tree"))
    monkeypatch.setattr(export_gate, "run_node_tests", lambda tree, node: ["a node test is red"])
    monkeypatch.setattr(export_gate, "run_tree_python_tests", lambda tree: [])
    monkeypatch.setattr(export_gate, "run_series_tests", lambda: [])
    with pytest.raises(SystemExit, match="a node test is red"):
        export_gate.gate(Path("src"), "head")


def test_the_series_tests_it_runs_are_these_ones():
    assert subprocess.run([sys.executable, "-c", "import pytest"], capture_output=True).returncode == 0
    assert (Path(export_gate.ROOT) / "tools" / "tests").is_dir()
