"""The jobs that turn the node on and off run on GitHub's runners with this repository
checked out and no source tree: they take their scripts from the series' own copy, so
those paths must exist, and the scripts must start from there alone.

    python -m pytest tools/tests
"""
import json
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
    assert "platform/ci/src/scripts/ci/latitude_node.py" in referenced()
    assert "platform/ci/src/scripts/ci/cache_rules.py" in referenced()


def test_the_node_scripts_travel_with_the_series():
    scripts = ROOT / "platform/ci/src/scripts/ci"
    assert (scripts / "node_bootstrap.sh").is_file() and (scripts / "node_runner.sh").is_file()


def test_no_pull_request_reaches_a_runner_of_ours():
    """The repository is public: every job a pull request or a comment can start names a
    GitHub runner by its literal name, series.yml included, which the source tree's own
    test cannot see. And no workflow here names self-hosted or one of our labels."""
    import yaml

    pr_events = {"pull_request", "pull_request_target", "pull_request_review", "pull_request_review_comment",
                 "issue_comment", "issues", "workflow_run", "merge_group", "discussion", "discussion_comment"}
    hosted = re.compile(r"^(ubuntu|windows|macos)-[\w.-]+$")
    for wf in WORKFLOWS.glob("*.yml"):
        doc = yaml.safe_load(wf.read_text(encoding="utf-8"))
        on = doc.get(True, doc.get("on"))
        events = {on} if isinstance(on, str) else set(on)
        for name, job in doc["jobs"].items():
            where = json.dumps(job.get("runs-on", ""))
            assert "self-hosted" not in where and "latitude-" not in where, "%s: %s" % (wf.name, name)
            if events & pr_events:
                assert "uses" not in job and hosted.match(str(job.get("runs-on", ""))), "%s: %s" % (wf.name, name)


def test_every_script_exists_and_starts_without_the_tree():
    for rel in referenced():
        # Nothing written beside the script: a .pyc in the series is not in its MANIFEST.
        done = subprocess.run([sys.executable, "-B", str(ROOT / rel), "--help"], capture_output=True, text=True)
        assert done.returncode == 0, "%s: %s" % (rel, done.stderr)
