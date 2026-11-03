#!/usr/bin/env python3
"""Long work beside other work, on the node, across a job's steps.

A job's steps run one after the other, and a machine paid by the hour sits at one core
through a 39-minute link. So the build, lavapipe and materialize go on in a session of
their own while later steps do what does not need their result, and a later step waits
and takes their exit code. Their output goes to the log of the step that follows them,
and what they write for the step summary to the summary of the step that waits for them:
the file of the step that started them is read only when that step ends.

    background.py start NAME [--nice N] -- CMD...
    background.py wait NAME             its output, then its exit code
    background.py until-compiled NAME   its output, until libxul's link runs with no
                                        compiler left, or it has ended; compiled=true|false
                                        for $GITHUB_OUTPUT; it exits 0
    background.py stop NAME...          ends them and all they started, for a cancel

The runner's signals on a cancel reach a step's shell and no further, so what stops the
work is `stop`, in a step of its own. Going by the process group would miss most of it:
mach runs make through mozprocess, which gives it a group of its own. So it walks the
children, from the process that `start` left, and nothing above that.

Its cases: scripts/ci/test_release_path.py.
"""
from __future__ import annotations

import argparse
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

POLL_S = 1.0
STOP_GRACE_S = 5
# The compiles beside the link's start end by themselves, in 2 to 4 minutes (cold Linux on
# the node, Windows in run 41402916724). A save needs them over: tar exits 1 when a file
# changes under it, and the cache action then only warns, with the entry lost. No compiler
# for twice the longest silence seen between two of them, 50 s on Windows.
QUIET_S = 120
# Past this the link is reason enough: sccache's objects appear whole, by a rename.
QUIET_LIMIT_S = 1800


def home() -> Path:
    return Path(os.environ.get("BG_DIR") or Path(os.environ.get("RUNNER_TEMP", ".")) / "background")


def _run(name: str, nice: int, argv: list) -> int:
    done = home() / ("%s.rc" % name)
    code = 1
    try:
        code = subprocess.run(argv, preexec_fn=(lambda: os.nice(nice)) if nice else None, check=False).returncode
        return code
    finally:
        temporary = done.with_suffix(".tmp")
        temporary.write_text(str(code if code >= 0 else 128 - code), encoding="utf-8")
        temporary.replace(done)


def start(name: str, nice: int, argv: list) -> int:
    home().mkdir(parents=True, exist_ok=True)
    with (home() / ("%s.out" % name)).open("wb") as out:
        proc = subprocess.Popen([sys.executable, "-u", str(Path(__file__).resolve()), "_run", name, "--nice", str(nice), "--", *argv],
                                stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, start_new_session=True,
                                env=dict(os.environ, GITHUB_STEP_SUMMARY=str(home() / ("%s.summary" % name))))
    (home() / ("%s.pid" % name)).write_text(str(proc.pid), encoding="utf-8")
    print("background: %s started, its output comes with the step that waits for it" % name)
    return 0


def _parents(proc: Path) -> dict:
    """pid -> parent pid of what is running; a zombie is not."""
    out = {}
    for entry in proc.iterdir():
        if entry.name.isdigit():
            try:
                fields = (entry / "stat").read_text().rsplit(")", 1)[1].split()
                if fields[0] != "Z":
                    out[int(entry.name)] = int(fields[1])
            except (OSError, ValueError, IndexError):
                continue
    return out


def _descendants(roots: set, proc: Path) -> set:
    parents = _parents(proc)
    found, grew = {p for p in roots if p in parents}, True
    while grew:
        more = {p for p, parent in parents.items() if parent in found} - found
        found |= more
        grew = bool(more)
    return found


def _root(name: str, proc: Path):
    """The process `start` left, if it is still that one: a pid file can outlive it."""
    try:
        pid = int((home() / ("%s.pid" % name)).read_text(encoding="utf-8"))
        words = (proc / str(pid) / "cmdline").read_bytes().split(b"\0")
    except (OSError, ValueError):
        return None
    return pid if b"_run" in words and name.encode() in words else None


def stop(names: list, proc: Path = Path("/proc"), grace: float = STOP_GRACE_S) -> int:
    roots = {r for r in (_root(n, proc) for n in names) if r is not None}
    # What dies leaves its children to init, so everything seen stays in the set.
    seen = _descendants(roots, proc)
    limit = time.monotonic() + grace
    for how in (signal.SIGTERM, signal.SIGKILL):
        for pid in sorted(seen):
            try:
                os.kill(pid, how)
            except OSError:
                pass
        while how == signal.SIGTERM and time.monotonic() < limit:
            seen |= _descendants(seen, proc)
            if not (seen & set(_parents(proc))):
                return 0
            time.sleep(0.2)
        seen |= _descendants(seen, proc)
    return 0


def _alive(name: str) -> bool:
    try:
        pid = int((home() / ("%s.pid" % name)).read_text(encoding="utf-8"))
        os.kill(pid, 0)
        # A zombie still answers: a start in this process's tree has not been reaped.
        return Path("/proc/%d/stat" % pid).read_text().rsplit(")", 1)[1].split()[0] != "Z"
    except (OSError, ValueError, IndexError):
        return False


class Follow:
    """The work's output from where the last step stopped reading."""

    def __init__(self, name: str):
        self.name, self.out, self.pos = name, home() / ("%s.out" % name), home() / ("%s.pos" % name)
        try:
            self.at = int(self.pos.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            self.at = 0

    def drain(self):
        try:
            with self.out.open("rb") as f:
                f.seek(self.at)
                while True:
                    chunk = f.read(1 << 16)
                    if not chunk:
                        break
                    sys.stdout.buffer.write(chunk)
                    self.at += len(chunk)
            sys.stdout.buffer.flush()
            self.pos.write_text(str(self.at), encoding="utf-8")
        except OSError:
            pass

    def exit_code(self):
        """Its exit code once it has ended, or None."""
        try:
            return int((home() / ("%s.rc" % self.name)).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            pass
        if not _alive(self.name):
            try:
                return int((home() / ("%s.rc" % self.name)).read_text(encoding="utf-8"))
            except (OSError, ValueError):
                print("background: %s is gone, with no exit code" % self.name, flush=True)
                return 1
        return None


def _hand_on_summary(name: str):
    target = os.environ.get("GITHUB_STEP_SUMMARY")
    written = home() / ("%s.summary" % name)
    if target and written.is_file():
        with open(target, "a", encoding="utf-8") as f:
            f.write(written.read_text(encoding="utf-8", errors="replace"))


def wait(name: str, poll: float = POLL_S) -> int:
    follow = Follow(name)
    while True:
        code = follow.exit_code()
        follow.drain()
        if code is not None:
            print("background: %s exited %d" % (name, code), flush=True)
            _hand_on_summary(name)
            return code
        time.sleep(poll)


def _nice(proc: Path, pid: int) -> int:
    try:
        return int((proc / str(pid) / "stat").read_text().rsplit(")", 1)[1].split()[16])
    except (OSError, ValueError, IndexError):
        return 0


def compile_state(proc: Path = Path("/proc")) -> tuple:
    """(libxul's link is running, a compiler is running). A compile has -c, the driver
    that waits for a link has not; rustc always compiles. What runs at nice 19 is not the
    build's: lavapipe writes nothing of the compile cache."""
    import cpu_watch
    linking = compiling = False
    for pid, row in cpu_watch.process_table(proc).items():
        comm = row[0]
        if comm in cpu_watch.LINKERS:
            linking = linking or cpu_watch.link_output(proc, pid) in cpu_watch.MAIN_LIBRARY
        elif _nice(proc, pid) >= 19:
            continue
        elif comm == "rustc":
            compiling = True
        elif comm.startswith("clang"):
            try:
                args = (proc / str(pid) / "cmdline").read_bytes().split(b"\0")
            except OSError:
                continue
            compiling = compiling or b"-c" in args or b"-cc1" in args
    return linking, compiling


def until_compiled(name: str, poll: float = POLL_S, quiet_s: float = QUIET_S, limit_s: float = QUIET_LIMIT_S,
                   state=compile_state, clock=time.monotonic) -> bool:
    follow = Follow(name)
    linked_at = quiet_since = None
    while True:
        code = follow.exit_code()
        follow.drain()
        if code is not None:
            return code == 0
        now = clock()
        try:
            linking, compiling = state()
        except OSError:
            linking, compiling = False, True
        if linking:
            linked_at = now if linked_at is None else linked_at
            quiet_since = None if compiling else (now if quiet_since is None else quiet_since)
            if (quiet_since is not None and now - quiet_since >= quiet_s) or now - linked_at >= limit_s:
                return True
        else:
            quiet_since = None
        time.sleep(poll)


def main(argv=None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    command = argv[:argv.index("--")] if "--" in argv else argv
    rest = argv[argv.index("--") + 1:] if "--" in argv else []
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("start", "_run"):
        s = sub.add_parser(name)
        s.add_argument("name")
        s.add_argument("--nice", type=int, default=0)
    sub.add_parser("wait").add_argument("name")
    sub.add_parser("until-compiled").add_argument("name")
    sub.add_parser("stop").add_argument("names", nargs="+")
    a = ap.parse_args(command)
    if a.cmd == "start":
        return start(a.name, a.nice, rest)
    if a.cmd == "_run":
        return _run(a.name, a.nice, rest)
    if a.cmd == "stop":
        return stop(a.names)
    if a.cmd == "wait":
        return wait(a.name)
    compiled = until_compiled(a.name)
    print("background: %s: %s" % (a.name, "the objects are complete" if compiled else "stopped before its objects were complete"), flush=True)
    output = os.environ.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as f:
            f.write("compiled=%s\n" % str(compiled).lower())
    return 0


if __name__ == "__main__":
    sys.exit(main())
