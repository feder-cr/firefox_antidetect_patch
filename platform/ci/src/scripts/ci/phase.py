"""One phase of a leg, run as a command that leaves events behind.

    python3 scripts/ci phase build -- ./mach build --keep-going

runs the command with its output on the console AND in logs/<phase>.log, and
appends to events.jsonl one "start" and one "end" event (seconds, exit code).
When the command fails, the first error lines of its output go into the step
summary, and the memory watch's kills with them: a failed leg says why on
its run page, without downloading a log. The Windows legs of runs 40446702889
and 40466778026 failed with nothing readable at all, and the Linux leg of run
40500378511 needed a 500 KB log searched by hand to find "ld.lld killed".

The exit code is the command's, so the step fails exactly when it did.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

EVENTS = Path(os.environ.get("CI_EVENTS", "events.jsonl"))
LOGS = Path(os.environ.get("CI_LOGS", "logs"))

# What a failing Firefox build prints when it stops: mach's "E" lines, the
# compilers' and make's errors, Rust's error[...], and the CI's own ERROR lines.
ERROR = re.compile(r"(^|\s)(E\s|error(\[|:)|ERROR|\*\*\* |Error \d+)|KILLED")
WARNING = re.compile(r"warning", re.I)
MAX_ERROR_LINES = 25


def event(phase: str, what: str, **detail) -> dict:
    record = {"t": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              "leg": os.environ.get("CI_LEG", ""), "phase": phase, "event": what}
    record.update(detail)
    with EVENTS.open("a", encoding="utf-8") as f:
        f.write(json.dumps(record, sort_keys=True) + "\n")
    return record


def first_errors(lines, limit: int = MAX_ERROR_LINES) -> list:
    out = []
    for line in lines:
        if ERROR.search(line) and not WARNING.search(line):
            out.append(line.rstrip())
            if len(out) == limit:
                break
    return out


def summary(text: str) -> None:
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        with open(path, "a", encoding="utf-8") as f:
            f.write(text + "\n")


def run(phase: str, argv: list) -> int:
    LOGS.mkdir(parents=True, exist_ok=True)
    log_path = LOGS / ("%s.log" % phase)
    event(phase, "start", argv=" ".join(argv))
    started = time.monotonic()
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, encoding="utf-8", errors="replace", bufsize=1)
        for line in proc.stdout:
            sys.stdout.write(line)
            log.write(line)
        code = proc.wait()
    seconds = round(time.monotonic() - started)
    if code == 0:
        event(phase, "end", exit=0, seconds=seconds)
        summary("✅ **%s** %s in %s" % (os.environ.get("CI_LEG", ""), phase, clock(seconds)))
        return 0
    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    errors = first_errors(lines)
    killed = [l for l in _memwatch_lines() if "KILLED" in l]
    event(phase, "end", exit=code, seconds=seconds, errors=errors[:10], killed=killed)
    body = "\n".join(killed + errors) or "(no error line recognised; last lines)\n" + "\n".join(lines[-15:])
    summary("❌ **%s** %s failed after %s (exit %d)\n\n```\n%s\n```"
            % (os.environ.get("CI_LEG", ""), phase, clock(seconds), code, body))
    return code


def _memwatch_lines() -> list:
    path = Path(os.environ.get("CI_MEMWATCH", "memwatch.log"))
    try:
        return path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []


def clock(seconds: int) -> str:
    h, rest = divmod(int(seconds), 3600)
    return "%dh%02dm" % (h, rest // 60) if h else "%dm%02ds" % divmod(rest, 60)
