"""`engine.py apply` turns the series into a branch: the Mozilla tag, then one commit
per folder in SERIES order, titled with the folder, whose final tree is the engine.

Shown on a two-file repository instead of Firefox, so it runs anywhere (also in CI):
a folder that patches a Mozilla file and adds one of ours, a second that only adds.
Known-bad: apply onto an existing branch without --force must refuse, because the
branch may carry work that was never exported.

    python -m pytest tools/tests
"""
import json
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import engine  # noqa: E402


def run(cwd, *args):
    return subprocess.run(["git", "-C", str(cwd), *args], check=True, capture_output=True,
                          env={**engine.IDENTITY, "PATH": __import__("os").environ["PATH"]}).stdout.decode().strip()


@pytest.fixture
def world(tmp_path, monkeypatch):
    src = tmp_path / "firefox"
    src.mkdir()
    run(src, "init", "-q")
    run(src, "config", "core.autocrlf", "false")
    (src / "mozilla.txt").write_bytes(b"one\ntwo\n")
    run(src, "add", ".")
    run(src, "commit", "-qm", "upstream")
    run(src, "tag", "TAG")
    # the patch the series carries for mozilla.txt
    (src / "mozilla.txt").write_bytes(b"one\nhook();\ntwo\n")
    patch = subprocess.run(["git", "-C", str(src), "diff", "--binary", "--full-index"],
                           check=True, capture_output=True).stdout
    run(src, "checkout", "-q", "--", "mozilla.txt")

    series = tmp_path / "engine"
    a = series / "fingerprint" / "a"
    (a / "src" / "dom").mkdir(parents=True)
    (a / "hooks.patch").write_bytes(patch)
    (a / "src" / "dom" / "StealthA.cpp").write_bytes(b"// ours\n")
    (a / "MANIFEST.json").write_text(json.dumps({"modified": ["mozilla.txt"],
                                                 "added": {"dom/StealthA.cpp": "100644"}}))
    b = series / "platform" / "b"
    (b / "src").mkdir(parents=True)
    (b / "src" / "tool.py").write_bytes(b"print(1)\n")
    (b / "MANIFEST.json").write_text(json.dumps({"modified": [], "added": {"tool.py": "100755"}}))
    (series / "SERIES").write_text("platform/b\nfingerprint/a\n")
    monkeypatch.setattr(engine, "ROOT", series)
    return src


def test_apply_writes_one_commit_per_folder_on_top_of_the_tag(world):
    engine.apply(world, "TAG", "work", force=False)
    titles = run(world, "log", "--format=%s", "TAG..work").splitlines()
    assert titles == ["fingerprint/a", "platform/b"]
    assert run(world, "show", "work:mozilla.txt") == "one\nhook();\ntwo"
    assert run(world, "show", "work:dom/StealthA.cpp") == "// ours"
    assert "100755" in run(world, "ls-tree", "work", "tool.py")
    assert run(world, "log", "-1", "--format=%ae", "work") == engine.IDENTITY["GIT_AUTHOR_EMAIL"]


def test_apply_refuses_to_overwrite_a_branch_without_force(world):
    engine.apply(world, "TAG", "work", force=False)
    with pytest.raises(SystemExit):
        engine.apply(world, "TAG", "work", force=False)
    engine.apply(world, "TAG", "work", force=True)


def test_what_apply_builds_is_what_verify_accepts(world, capsys):
    engine.apply(world, "TAG", "work", force=False)
    assert engine.verify(world, "TAG", "work") == 0


def test_materialize_fetches_the_tag_applies_the_series_and_checks_the_tree(world, tmp_path, monkeypatch):
    engine.apply(world, "TAG", "probe", force=False)
    tree = run(world, "rev-parse", "probe^{tree}")
    up = tmp_path / "UPSTREAM.json"
    up.write_text(json.dumps({"mozilla_repository": str(world), "mozilla_tag": "TAG",
                              "source_branch": "work", "exported_tree": tree}))
    monkeypatch.setattr(engine, "UPSTREAM", up)
    into = tmp_path / "runner"
    head = engine.materialize(into)
    # the checkout follows the machine's line-ending setting, as actions/checkout does;
    # what git stores is the series' bytes exactly
    assert (into / "dom" / "StealthA.cpp").read_bytes().replace(b"\r\n", b"\n") == b"// ours\n"
    assert run(into, "show", "HEAD:dom/StealthA.cpp") == "// ours"
    assert head == run(world, "rev-parse", "probe"), "same series + same tag = same commit, anywhere"
    # a series that no longer gives the declared tree is refused, not built
    up.write_text(json.dumps({"mozilla_repository": str(world), "mozilla_tag": "TAG",
                              "source_branch": "work", "exported_tree": "0" * 40}))
    with pytest.raises(SystemExit):
        engine.materialize(tmp_path / "runner2")
