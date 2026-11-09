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

And red if a gtest of ours that the filter selects did not run, whatever way the filter
names it (a prefix names no suite, so a suite of ours that left the build would pass): the
list of ours is read from the series by scripts/our_gtests.py, the same function that
scripts/test_release_plan.py checks the filter with. This job has no source tree, so the
build job writes the list beside the binaries (ROOT/our-gtests.txt, in gtest-bin.tar);
with ENGINE_SERIES_ROOT set to a series it is read from that series instead. With neither the
run is refused before any suite starts: a check that could not run is not a pass.

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

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import our_gtests  # noqa: E402

SUITE = re.compile(r"(\S+)\.(  # .*)?")
#: Where the build job writes the list of our gtests, next to bin/ (see scripts/our_gtests.py).
OURS_FILE = "our-gtests.txt"
# Suites of ours compiled for one platform only: the suites NOT_ON[platform] are not there. Read from
# the moz.build conditions and #ifdefs of their files (the WebGPU ones were first seen in
# run 41402916724's logs); scripts/ci/test_release_path.py checks that each is one of ours.
NOT_ON = {
    "linux": {"StealthWebGPUAdapter", "StealthWebGPUInstanceEnv",  # XP_WIN in TestStealthWebGPUDeclaration.cpp
              "StealthBundledCollection", "StealthFontList", "StealthFallbackNames", "StealthTtcFace",
              "StealthBundledFamilyFilter", "StealthTextQuality",  # OS_TARGET == WINNT in gfx/tests/gtest/moz.build
              "StealthOwnBinaries"},  # toolkit/xre/dllservices is built on WINNT only
    "win": {"StealthWebGPUBundledVulkan",  # XP_LINUX in TestStealthWebGPUDeclaration.cpp
            "StealthFcEmptyConfig", "StealthFcFontList", "StealthFontDescriptorPath",
            "StealthFontMetrics"},  # MOZ_WIDGET_TOOLKIT == gtk in gfx/tests/gtest/moz.build
}


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


def read_ours(root: Path, environ=None) -> list:
    """`Suite.Test` of every gtest of ours: from the series ENGINE_SERIES_ROOT names, else from
    ROOT/our-gtests.txt. Raises OSError, saying what is missing, when it can be had from neither."""
    environ = os.environ if environ is None else environ
    if environ.get(our_gtests.SERIES_ROOT_ENV):
        return our_gtests.our_test_names(Path(environ[our_gtests.SERIES_ROOT_ENV]))
    listed = root / OURS_FILE
    if not listed.is_file():
        raise OSError("no list of our gtests: %s is not there and %s names no series. The build job writes it "
                      "(python3 scripts/our_gtests.py --series .series --out <dir>/%s, into gtest-bin.tar)"
                      % (listed, our_gtests.SERIES_ROOT_ENV, OURS_FILE))
    names = [l.strip() for l in listed.read_text(encoding="utf-8").splitlines() if l.strip()]
    if not names:
        raise OSError("%s lists no gtest of ours" % listed)
    return names


def expected_suites(gtest_filter: str, ours: list, platform: str) -> list:
    """The suites of ours that the filter selects (one of their tests) and that this platform builds."""
    selected = {our_gtests.suite_of(n) for n in ours if our_gtests.gtest_matches(gtest_filter, n)}
    return sorted(selected - NOT_ON.get(platform, set()))


def ran_suites(report: dict) -> set:
    """The suites a report says ran a test (`X/Suite` of a TEST_P counts as Suite)."""
    return {our_gtests.suite_of(s["name"]) for s in report.get("testsuites", [])
            if s.get("name") and s.get("tests", 1) > s.get("disabled", 0)}


def environment(root: Path, gtest_filter: str, base=None) -> dict:
    bin_dir = root / "bin"
    env = dict(os.environ if base is None else base)
    env.update(GTEST_FILTER=gtest_filter, MOZ_XRE_DIR=str(bin_dir), MOZ_RUN_GTEST="True", PYTHON=sys.executable,
               MOZ_GMP_PATH=os.pathsep.join(str(bin_dir / p / "1.0") for p in ("gmp-fake", "gmp-fakeopenh264")),
               MOZ_WEBRENDER="0", MOZ_ENABLE_INC_ORIGIN_INIT="0")
    return env


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


def run(root: Path, gtest_filter: str, reports=None, out=print, cores=None, debugger="gdb", ours=None) -> int:
    """`ours`: the `Suite.Test` of our gtests, as read_ours() gives them (read here when None)."""
    if not gtest_filter:
        out("ERROR: an empty filter: name the gtests, or * for all")
        return 2
    if ours is None:
        try:
            ours = read_ours(root)
        except OSError as exc:
            out("ERROR: our gtests were NOT read, so whether they ran cannot be checked: %s" % exc)
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
    ran = set()
    args = [app, "-unittest"] + ([death] + launcher if windows else [])
    with tempfile.TemporaryDirectory() as temporary:
        where = Path(reports).resolve() if reports else Path(temporary)
        where.mkdir(parents=True, exist_ok=True)
        for suite in suites:
            one = dict(env, GTEST_FILTER=only_suite(gtest_filter, suites, suite),
                       GTEST_OUTPUT="json:%s" % (where / ("%s.json" % suite.replace("/", "_"))))
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
            ran |= ran_suites(report)
    out("Ran %d test(s) from %d test suite(s) (%d disabled), with %d failure(s)." % (tests - disabled, len(suites), disabled, failures))
    if tests - disabled <= 0:
        out("ERROR: no gtest ran under the filter %s" % gtest_filter)
        return exit_code or 1
    missing = missing_suites(gtest_filter, suites, "win" if windows else "linux")
    if missing:
        out("ERROR: the filter names suites that are not there: %s" % ", ".join(missing))
        return exit_code or 1
    silent = [s for s in expected_suites(gtest_filter, ours, "win" if windows else "linux") if s not in ran]
    if silent:
        out("ERROR: suites of ours that the filter selects did not run (not in the reports): %s" % ", ".join(silent))
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
