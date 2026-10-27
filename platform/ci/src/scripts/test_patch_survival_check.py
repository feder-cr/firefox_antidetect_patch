#!/usr/bin/env python3
"""patch_survival_check.sh, driven against a line that starts with a dash.

The 151 -> 157 port ran the gate and got 433 "lost lines". Most were not lost:
every added line that starts with `-` (a YAML list item, a docstring bullet, an
option in a usage text) reached `grep -qxF "$line"` as an OPTION, grep refused
it, and the refusal counted as "not found". A gate that reports survivors as
lost is as useless as one that reports nothing, because the real losses drown.

  known-bad   the dash line is gone from the new branch -> 1 lost line
  known-good  the dash line is still there              -> nothing lost, exit 0

Run it directly, like test_make_seal.py:
`python3 scripts/test_patch_survival_check.py`.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "patch_survival_check.sh"
# On Windows a bare `bash` is WSL's, which cannot see this temp repo: the gate
# runs under Git's bash there, as it does by hand.
GIT_BASH = Path(r"C:\Program Files\Git\bin\bash.exe")
BASH = str(GIT_BASH) if sys.platform == "win32" and GIT_BASH.exists() else "bash"
DASH_LINE = "- name: zoom.stealth.example"


def git(repo: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(repo), *args], check=True,
                   capture_output=True, text=True)


def run_gate(keep_line: bool) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as tmp:
        repo = Path(tmp)
        git(repo, "init", "-q", "-b", "main")
        git(repo, "config", "user.name", "test")
        git(repo, "config", "user.email", "test@example.invalid")
        (repo / "scripts").mkdir()
        shutil.copy(SCRIPT, repo / "scripts" / SCRIPT.name)
        prefs = repo / "prefs.yaml"
        prefs.write_text("- name: upstream.pref\n", encoding="utf-8")
        git(repo, "add", ".")
        git(repo, "commit", "-q", "-m", "base")
        git(repo, "tag", "base")
        prefs.write_text(f"- name: upstream.pref\n{DASH_LINE}\n", encoding="utf-8")
        git(repo, "commit", "-q", "-am", "ours")
        git(repo, "tag", "ours")
        # The "new branch" is the working tree the gate reads.
        prefs.write_text("- name: upstream.pref\n" + (f"{DASH_LINE}\n" if keep_line else ""),
                         encoding="utf-8")
        return subprocess.run([BASH, str(repo / "scripts" / SCRIPT.name), "base", "ours"],
                              cwd=repo, capture_output=True, text=True)


def main() -> int:
    failures = []
    kept = run_gate(keep_line=True)
    if kept.returncode != 0 or "[LOST LINE]" in kept.stdout:
        failures.append(f"a surviving '-' line was reported lost:\n{kept.stdout}{kept.stderr}")
    gone = run_gate(keep_line=False)
    if gone.returncode == 0 or "lost lines: 1" not in gone.stdout:
        failures.append(f"a dropped '-' line was not reported:\n{gone.stdout}{gone.stderr}")
    for f in failures:
        print("FAIL:", f)
    if not failures:
        print("PASS: a line starting with '-' is found when kept and reported when dropped")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
