"""The jobs that turn the node on and off run on GitHub's runners with this repository
checked out and no source tree: they take their scripts from the series' own copy, so
those paths must exist, and the scripts must start from there alone.

    python -m pytest tools/tests
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
PATH = re.compile(r"\.series/(platform/ci/src/\S+\.py)")


def referenced():
    return sorted({m for wf in WORKFLOWS.glob("*.yml") for m in PATH.findall(wf.read_text(encoding="utf-8"))})


def test_the_workflows_name_the_node_scripts():
    assert "platform/ci/src/scripts/ci/theta_node.py" in referenced()
    assert "platform/ci/src/scripts/ci/cache_rules.py" in referenced()


def test_every_script_exists_and_starts_without_the_tree():
    for rel in referenced():
        # Nothing written beside the script: a .pyc in the series is not in its MANIFEST.
        done = subprocess.run([sys.executable, "-B", str(ROOT / rel), "--help"], capture_output=True, text=True)
        assert done.returncode == 0, "%s: %s" % (rel, done.stderr)
