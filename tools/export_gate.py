"""The tests that have to be green before the series is exported.

Run 43156862361 (the third candidate of the UDP line) died one minute after it started: the Juggler's node tests, which the build job
runs before it compiles, found six reds that nobody had run, because the local tree they were tried on is a sparse one and four of
them read files it does not have. A red that can be found here costs a node of Latitude there. So `engine.py export` runs, on the
WHOLE tree of the head it is about to export, the same tests the build job runs, and refuses to write anything if one is red:

  * the Juggler's node tests (juggler/tests/*.test.mjs), with Node;
  * the python tests of ours in the engine tree that are plain scripts (modules/libpref/test/test_stealth_*.py);
  * the tests of the series itself (tools/tests), with pytest.

The tree is a git worktree of the source repository, at ENGINE_NODE_GATE_TREE (default C:/ff/wt/node-gate), moved to the head by a
checkout (the first time it is made it writes the whole tree, a quarter of an hour; after that only what differs). Node is the one
in ENGINE_NODE, else the portable one of this PC, else the one on the PATH; with none the export refuses (a gate that cannot run is
not a pass). There is no switch to skip it.
"""
from __future__ import annotations

import glob
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TREE = Path(os.environ.get("ENGINE_NODE_GATE_TREE", "C:/ff/wt/node-gate"))
PORTABLE_NODE = "C:/Users/feder/ext/tetra-run/node/node-v26.13.0-win-x64/node.exe"


def node_binary() -> str:
    for candidate in (os.environ.get("ENGINE_NODE"), PORTABLE_NODE, shutil.which("node")):
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise SystemExit("export refused: there is no Node on this machine (ENGINE_NODE, %s, or node on the PATH), so the Juggler's "
                     "node tests cannot run, and the build job would be the first to run them" % PORTABLE_NODE)


def git(*args: str, cwd: Path | None = None) -> str:
    done = subprocess.run(["git", "-c", "core.longpaths=true", *args], cwd=cwd, capture_output=True, text=True)
    if done.returncode:
        raise SystemExit("export refused: git %s failed: %s" % (" ".join(args[:3]), done.stderr.strip()[-400:]))
    return done.stdout.strip()


def prepare_tree(source: Path, head: str, tree: Path = TREE) -> Path:
    """The whole tree of `head`, in a worktree of the source repository."""
    sha = git("-C", str(source), "rev-parse", head)
    if not (tree / ".git").exists():
        print("export gate: making the whole tree of the node tests in %s (the first time takes a quarter of an hour)" % tree)
        git("-C", str(source), "worktree", "add", "--detach", str(tree), sha)
    elif git("-C", str(tree), "rev-parse", "HEAD") != sha:
        git("-C", str(tree), "checkout", "--detach", "--force", sha)
    if git("-C", str(tree), "rev-parse", "HEAD") != sha:
        raise SystemExit("export refused: %s is not at %s" % (tree, sha))
    if not (tree / "juggler" / "tests").is_dir() or not (tree / "netwerk" / "base").is_dir():
        raise SystemExit("export refused: %s is not a whole tree (no juggler/tests or netwerk/base): remove it and run again" % tree)
    return tree


def _failures(output: str) -> list:
    return [line.split(" - ", 1)[-1].strip()[:150] for line in output.splitlines() if line.startswith("not ok ")]


def run_node_tests(tree: Path, node: str) -> list:
    files = sorted(glob.glob(str(tree / "juggler" / "tests" / "*.test.mjs")))
    if not files:
        return ["no juggler/tests/*.test.mjs in %s" % tree]
    done = subprocess.run([node, "--test", "--test-reporter=tap", *files], cwd=tree, capture_output=True, text=True, encoding="utf-8", errors="replace")
    summary = [line for line in done.stdout.splitlines() if line.startswith(("# tests", "# pass", "# fail"))]
    print("export gate: node tests: " + ", ".join(s[2:] for s in summary))
    if done.returncode:
        return _failures(done.stdout) or ["node exited %d: %s" % (done.returncode, done.stderr.strip()[-300:])]
    return []


def run_tree_python_tests(tree: Path) -> list:
    problems = []
    for script in sorted(glob.glob(str(tree / "modules" / "libpref" / "test" / "test_stealth_*.py"))):
        done = subprocess.run([sys.executable, script], cwd=tree, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if done.returncode:
            problems.append("%s: %s" % (Path(script).name, (done.stdout + done.stderr).strip()[-300:]))
    print("export gate: %d python tests of the tree" % len(glob.glob(str(tree / "modules" / "libpref" / "test" / "test_stealth_*.py"))))
    return problems


def run_series_tests() -> list:
    done = subprocess.run([sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider", "tools/tests"], cwd=ROOT,
                          capture_output=True, text=True, encoding="utf-8", errors="replace")
    print("export gate: tools/tests: " + (done.stdout.strip().splitlines() or ["no output"])[-1])
    if done.returncode:
        return ["tools/tests: " + (done.stdout + done.stderr).strip()[-500:]]
    return []


def gate(source: Path, head: str) -> None:
    """Nothing is returned: the export goes on, or this exits saying which test is red."""
    node = node_binary()
    tree = prepare_tree(source, head)
    problems = run_node_tests(tree, node) + run_tree_python_tests(tree) + run_series_tests()
    if problems:
        raise SystemExit("export refused: red before the build job would have found it:\n  " + "\n  ".join(problems))
