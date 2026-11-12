#!/usr/bin/env python3
"""A leg's gtests where `mach gtest` cannot run: on a runner with its binaries and no
source tree. What `mach gtest <filter>` does after its build, for one job on Linux
(python/mozbuild/mozbuild/mach_commands.py, testing/gtest/suites.py):

  - the suites from --gtest_list_tests under the filter, from _tests/gtest;
  - one process per suite, one after the other, each with the filter narrowed to it;
  - MOZ_XRE_DIR, MOZ_GMP_PATH, MOZ_RUN_GTEST, PYTHON, MOZ_WEBRENDER=0,
    MOZ_ENABLE_INC_ORIGIN_INIT=0, GTEST_OUTPUT to count what ran;
  - red if any process is.

Stricter than mach in what it cannot tell: red if nothing ran, and red if a suite the
filter names is not there, unless it does not exist on this platform (NOT_ON). The report
of each suite stays in --reports, for comparing runs.

On Windows firefox.exe is the launcher: --wait-for-browser, as the green candidate ran it.
A suite whose process dies on a signal says which, and with --cores (Linux, where the job
points the kernel's core_pattern there) its core is read with gdb: every thread's
backtrace goes into the log and into --reports as <suite>.backtrace.txt, and the core is
dropped. Candidate 42473169415 had a suite exit -11 after all its tests passed, and the
log could say no more than that.

    GTEST_FILTER_INPUT=<filter> python3 gtest_run.py --root DIR [--reports DIR] [--cores DIR]    (DIR/bin, DIR/gtest)

Its cases: scripts/ci/test_release_path.py.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

SUITE = re.compile(r"(\S+)\.(  # .*)?")
# Suites that run with the log of the modules whose failures the reports must explain, one file per process
# (MOZ_LOG_FILE with %PID), written synchronously so that a process that dies leaves what it said. The files stay in
# --reports next to the suite's json. StealthSessionUdpProxy: the SOCKS UDP layer and the host resolver, for a test red
# on one platform whose cause the sources do not say.
LOGGED = {"StealthSessionUdpProxy": "sync,SOCKS-UDP:5,nsHostResolver:5"}
# Named by the default filter and compiled for one platform only, from run 41402916724's logs.
NOT_ON = {"linux": {"StealthWebGPUAdapter", "StealthWebGPUInstanceEnv"}, "win": {"StealthWebGPUBundledVulkan"}}


def suite_names(listing: str) -> list:
    return [m.group(1) for m in map(SUITE.fullmatch, listing.splitlines()) if m]


def only_suite(existing: str, suites: list, suite: str) -> str:
    """The filter that runs one suite of `suites`, as testing/gtest/suites.py makes it."""
    if existing == "*":
        return "%s.*" % suite
    others = ":".join("%s.*" % s for s in suites if s != suite)
    return existing + (":" if "-" in existing else "-") + others


def missing_suites(gtest_filter: str, suites: list, platform: str) -> list:
    """The suites the filter names outright (Name.*) that did not list."""
    positive = gtest_filter.split("-", 1)[0].split(":")
    named = [p[:-2] for p in positive if p.endswith(".*") and "*" not in p[:-2] and "?" not in p[:-2]]
    return [n for n in named if n not in suites and n not in NOT_ON.get(platform, ())]


def environment(root: Path, gtest_filter: str, base=None) -> dict:
    bin_dir = root / "bin"
    env = dict(os.environ if base is None else base)
    env.update(GTEST_FILTER=gtest_filter, MOZ_XRE_DIR=str(bin_dir), MOZ_RUN_GTEST="True", PYTHON=sys.executable,
               MOZ_GMP_PATH=os.pathsep.join(str(bin_dir / p / "1.0") for p in ("gmp-fake", "gmp-fakeopenh264")),
               MOZ_WEBRENDER="0", MOZ_ENABLE_INC_ORIGIN_INIT="0")
    return env


def logging_for(suite: str, where: Path) -> dict:
    """MOZ_LOG and MOZ_LOG_FILE of a suite that is logged, nothing for the others."""
    modules = LOGGED.get(suite)
    if not modules:
        return {}
    folder = where / ("%s.moz-log" % suite.replace("/", "_"))
    folder.mkdir(parents=True, exist_ok=True)
    return {"MOZ_LOG": modules, "MOZ_LOG_FILE": str(folder / "moz-%PID.log")}


def signal_name(status: int) -> str:
    """SIGSEGV for -11; the number when Python has no name for it."""
    try:
        return signal.Signals(-status).name
    except ValueError:
        return "signal %d" % -status


def find_core(cores: Path, pid: int, since: float):
    """The core the suite's process left (core_pattern core.%p), else any core
    written since the suite started (one of its children)."""
    exact = sorted(cores.glob("core.%d*" % pid))
    if exact:
        return exact[0]
    fresh = sorted((p for p in cores.glob("core*") if p.stat().st_mtime >= since), key=lambda p: p.stat().st_mtime)
    return fresh[-1] if fresh else None


def backtrace(app: str, core: Path, debugger: str = "gdb", run=subprocess.run) -> str:
    """Every thread's backtrace from the core, as gdb prints it."""
    if not shutil.which(debugger):
        return "no %s on this machine: the core is %s" % (debugger, core)
    done = run([debugger, "-batch", "-nx", "-ex", "info sharedlibrary", "-ex", "thread apply all bt 40", app, str(core)],
               capture_output=True, text=True, check=False, timeout=900)
    return (done.stdout + done.stderr).strip()


def unlimited_core():
    import resource
    resource.setrlimit(resource.RLIMIT_CORE, (resource.RLIM_INFINITY, resource.RLIM_INFINITY))


def run(root: Path, gtest_filter: str, reports=None, out=print, cores=None, debugger="gdb") -> int:
    if not gtest_filter:
        out("ERROR: an empty filter: name the gtests, or * for all")
        return 2
    windows = (root / "bin" / "firefox.exe").is_file()
    app = str(root / "bin" / ("firefox.exe" if windows else "firefox"))
    launcher = ["--wait-for-browser"] if windows else []
    death = "--gtest_death_test_style=threadsafe"
    cwd = root / "gtest"
    cwd.mkdir(parents=True, exist_ok=True)
    env = environment(root, gtest_filter)
    listed = subprocess.run([app, "-unittest", death, "--gtest_list_tests"] + launcher,
                            cwd=cwd, env=env, capture_output=True, text=True, check=False)
    if listed.returncode:
        out("ERROR: listing the gtests exited %d:\n%s" % (listed.returncode, listed.stderr[-2000:]))
        return min(listed.returncode, 255) if listed.returncode > 0 else 1
    suites = suite_names(listed.stdout)
    exit_code, tests, failures, disabled = 0, 0, 0, 0
    args = [app, "-unittest"] + ([death] + launcher if windows else [])
    with tempfile.TemporaryDirectory() as temporary:
        where = Path(reports).resolve() if reports else Path(temporary)
        where.mkdir(parents=True, exist_ok=True)
        for suite in suites:
            one = dict(env, GTEST_FILTER=only_suite(gtest_filter, suites, suite),
                       GTEST_OUTPUT="json:%s" % (where / ("%s.json" % suite.replace("/", "_"))))
            one.update(logging_for(suite, where))
            out("%s: running" % suite)
            since = time.time()
            child = subprocess.Popen(args, cwd=cwd, env=one,
                                     preexec_fn=unlimited_core if cores and not windows else None)
            status = child.wait()
            if status > 0:
                out("%s: exited %d" % (suite, status))
                exit_code = status
            elif status < 0:
                out("%s: exited %d (%s)" % (suite, status, signal_name(status)))
                exit_code = 1
                core = find_core(Path(cores), child.pid, since) if cores and not windows else None
                if core:
                    trace = backtrace(app, core, debugger)
                    (where / ("%s.backtrace.txt" % suite.replace("/", "_"))).write_text(trace + "\n", encoding="utf-8")
                    for line in trace.splitlines():
                        out("%s: %s" % (suite, line))
                    core.unlink(missing_ok=True)
                elif cores and not windows:
                    out("%s: no core in %s" % (suite, cores))
            try:
                report = json.loads(Path(one["GTEST_OUTPUT"][5:]).read_text(encoding="utf-8"))
            except (OSError, ValueError):
                out("%s: left no report" % suite)
                continue
            tests += report.get("tests", 0)
            failures += report.get("failures", 0)
            disabled += report.get("disabled", 0)
    out("Ran %d test(s) from %d test suite(s) (%d disabled), with %d failure(s)." % (tests - disabled, len(suites), disabled, failures))
    if tests - disabled <= 0:
        out("ERROR: no gtest ran under the filter %s" % gtest_filter)
        return exit_code or 1
    missing = missing_suites(gtest_filter, suites, "win" if windows else "linux")
    if missing:
        out("ERROR: the filter names suites that are not there: %s" % ", ".join(missing))
        return exit_code or 1
    return min(exit_code, 255) or (1 if failures else 0)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, required=True)
    ap.add_argument("--reports", type=Path)
    ap.add_argument("--cores", type=Path, help="where core_pattern writes cores (Linux)")
    args = ap.parse_args(argv)
    return run(args.root.resolve(), os.environ.get("GTEST_FILTER_INPUT", ""), args.reports,
               cores=args.cores.resolve() if args.cores else None)


if __name__ == "__main__":
    sys.exit(main())
