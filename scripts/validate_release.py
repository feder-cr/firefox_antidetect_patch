#!/usr/bin/env python3
"""Pre-publish validation for a Firefox stealth release.

Validates BOTH the Linux tar.gz AND the Windows zip in a single run.
Each archive is extracted to a clean temp dir and:

  1. Symlink check - must have ZERO symlinks
  2. Path-leak check - must have ZERO `/home/feder` or `Users\\Feder` bytes
  3. Critical-files check - firefox + application.ini + dependentlibs.list
     must exist as real files
  4. Smoke-test - `firefox --version` (Linux: run under WSL if invoked from
     Windows; Windows: run the exe directly) must succeed and print
     "Mozilla Firefox <major>.x.y", where <major> is read from the tree
     (browser/config/version_display.txt) rather than hardcoded

Exits non-zero on the first failure so a release script can `&&` it
before uploading.

Exit codes:
    0   everything checked, everything passed
    1   a check failed - do not publish
    3   the scan could not run in full: the site-name patterns (step 2) were
        not applied. Nothing was proven about them, so this is not a pass.
        Point STEALTH_EXTRA_LEAK_PATTERNS at the token list, or state the gap
        on the record with --no-site-patterns (see below).

Usage:
    python scripts/validate_release.py <tag>

    # Validates both (<ver> comes from browser/config/version_display.txt):
    #   release/binary/<tag>/firefox-<ver>-stealth-linux-x86_64.tar.gz
    #   release/binary/<tag>/firefox-<ver>-stealth-win-x86_64.zip
"""
from __future__ import annotations

import argparse
import gzip
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# Build-machine paths that must never appear in a shipped archive.
LEAK_PATTERNS = [
    re.compile(rb"/home/feder", re.I),
    re.compile(rb"Users[\\/]Feder", re.I),
]

# Site names are deliberately NOT inline here. This file is public, and a
# scanner that carries the very token it is scanning for publishes exactly what
# it exists to keep out - the guard leaked the thing it guards. They live in a
# file outside this repo instead, one token per line, blank lines and # comments
# ignored, path given by STEALTH_EXTRA_LEAK_PATTERNS.
EXTRA_PATTERNS_ENV = "STEALTH_EXTRA_LEAK_PATTERNS"


def _extra_leak_patterns() -> tuple[list, str, bool]:
    """Returns (patterns, status_line, applied).

    `applied` is the whole point. This used to return the patterns and a warning
    string, and the warning was PRINTED - once, into the middle of a long run -
    and then had no effect on anything. A CI step keys on the exit code, not on
    the transcript, so an unset variable produced a green validation in which
    the site-name half of the scan never happened. The caller now has to decide
    what to do about `applied` and the exit code carries it.
    """
    path = os.environ.get(EXTRA_PATTERNS_ENV)
    if not path:
        return [], (
            f"WARNING: {EXTRA_PATTERNS_ENV} is unset, so site-name patterns were "
            f"NOT applied. Build-machine path checks still ran."
        ), False
    p = Path(path)
    if not p.is_file():
        return [], (
            f"WARNING: {EXTRA_PATTERNS_ENV}={path} does not exist, so site-name "
            f"patterns were NOT applied. Build-machine path checks still ran."
        ), False
    tokens = [
        ln.strip() for ln in p.read_text(encoding="utf-8").splitlines()
        if ln.strip() and not ln.strip().startswith("#")
    ]
    if not tokens:
        return [], (
            f"WARNING: {EXTRA_PATTERNS_ENV}={path} has no tokens in it, so site-name "
            f"patterns were NOT applied. Build-machine path checks still ran."
        ), False
    pats = [re.compile(re.escape(t).encode(), re.I) for t in tokens]
    return pats, f"site-name patterns loaded: {len(pats)} from {EXTRA_PATTERNS_ENV}", True

CRITICAL_LINUX = ["firefox", "application.ini", "dependentlibs.list"]
CRITICAL_WIN = ["firefox.exe", "application.ini", "dependentlibs.list"]

def _upstream_version() -> str:
    """Upstream Firefox version this branch is based on, read from the tree.

    Deliberately NOT hardcoded: it used to be pinned to 150, which silently made
    this gate unable to pass on an FF151 build - the smoke test prints
    "Mozilla Firefox 151.0" and the 150 regex rejected every archive. Reading it
    from the tree means the next base bump carries this along for free.
    """
    for rel in ("browser/config/version_display.txt", "browser/config/version.txt"):
        p = REPO_ROOT / rel
        if p.is_file():
            v = p.read_text(encoding="utf-8").strip()
            if v:
                return v
    raise SystemExit(
        "validate_release: cannot read the Firefox version from the tree "
        "(browser/config/version_display.txt missing)"
    )


UPSTREAM_VERSION = _upstream_version()          # e.g. "151.0"
UPSTREAM_MAJOR = UPSTREAM_VERSION.split(".")[0]  # e.g. "151"
VERSION_RE = re.compile(
    rf"^Mozilla Firefox {re.escape(UPSTREAM_MAJOR)}\.\d+(\.\d+)?", re.M
)


class ValidationError(Exception):
    pass


def check_no_symlinks_tar(archive: Path) -> None:
    """tar.gz must have no SYMlinks (they point to absolute build-host paths
    and break on user machines). Hard links inside the archive are fine -
    they reference other files that are also packed in.
    """
    with tarfile.open(archive, "r:gz") as tf:
        bad = []
        for m in tf.getmembers():
            if m.issym():
                # Reject any symlink pointing at an absolute path or escaping
                # the archive root. Relative symlinks inside the tree would
                # be OK in principle, but stealth-firefox doesn't ship any.
                bad.append((m.name, m.linkname))
    if bad:
        sample = [f"{n} -> {t}" for n, t in bad[:5]]
        raise ValidationError(
            f"{archive.name}: contains {len(bad)} symlinks "
            f"(broken on user machines). First 5: {sample}"
        )


_EXTRA_CACHE = None
_EXTRA_APPLIED = None


def _all_leak_patterns() -> list:
    """LEAK_PATTERNS plus the out-of-repo site tokens.

    Resolved once per run, and main() resolves it BEFORE any archive is opened,
    so the status is the first thing on screen rather than a line buried between
    two smoke tests - and so `_EXTRA_APPLIED` is decided even on a run where no
    archive was scanned at all.
    """
    global _EXTRA_CACHE, _EXTRA_APPLIED
    if _EXTRA_CACHE is None:
        pats, status, applied = _extra_leak_patterns()
        print(f"  [leak-scan] {status}")
        _EXTRA_CACHE, _EXTRA_APPLIED = pats, applied
    return LEAK_PATTERNS + _EXTRA_CACHE


def check_no_leaks_archive(archive: Path) -> None:
    """Scan the (decompressed) archive bytes for build-path and site-name leaks."""
    data = archive.read_bytes()
    if archive.suffix == ".gz":
        data = gzip.decompress(data)
    hits = []
    for pat in _all_leak_patterns():
        if pat.search(data):
            hits.append(pat.pattern.decode())
    if hits:
        raise ValidationError(
            f"{archive.name}: leak hits for patterns {hits}"
        )


def extract_tar(archive: Path, dst: Path) -> None:
    with tarfile.open(archive, "r:gz") as tf:
        tf.extractall(dst)


def extract_zip(archive: Path, dst: Path) -> None:
    with zipfile.ZipFile(archive) as zf:
        zf.extractall(dst)


def check_critical_files(root: Path, critical: list[str]) -> None:
    missing = []
    for name in critical:
        p = root / name
        if not p.exists():
            missing.append(name)
        elif p.is_symlink():
            missing.append(f"{name} (is a symlink)")
        elif p.stat().st_size == 0:
            missing.append(f"{name} (empty file)")
    if missing:
        raise ValidationError(f"critical files missing/broken: {missing}")


JUGGLER_ENTRIES = (
    "chrome/juggler/content/protocol/PageHandler.js",
    "chrome/juggler/content/TargetRegistry.js",
    "chrome/juggler/content/content/main.js",
    "chrome/juggler/content/content/Runtime.js",
)
JUGGLER_DIR_REL = "chrome/juggler"
JUGGLER_MARKERS = (b"stealthfox", b"zoom.stealth")


def _juggler_root(tree: Path) -> Path:
    """Where application.ini / omni.ja / chrome live for this extracted tree."""
    mac = tree / "Firefox.app" / "Contents" / "Resources"
    return mac if mac.is_dir() else tree


def assert_juggler_provenance(tree: Path) -> None:
    """The launch-time guard in invisible_core keys on these markers. If a
    rebase drops them, every user's launch breaks at once, so a build without
    them must never be published. Measured 2026-07-25: firefox-14 and
    firefox-18 mark 4/4; stock builds mark 0/4.

    TWO LAYOUTS. The Windows zip and the macOS tarballs pack the juggler into
    omni.ja. The Linux tarballs carry no omni.ja at all - linux_release.sh tars
    the pre-package dist/bin layout, where chrome/juggler/ is loose in the tree
    (measured on firefox-18: 0 *.ja members, 21 loose juggler members). Reading
    only omni.ja made this gate raise on the Linux arm, and since Hard rule 4
    makes this script a publishing precondition, that would have blocked every
    future release. The marker semantics are identical in both layouts.
    """
    import zipfile
    root = _juggler_root(tree)
    omni = root / "omni.ja"

    if omni.exists():
        layout = "omni.ja"
        with zipfile.ZipFile(omni) as zf:
            names = set(zf.namelist())
            if not any(n.startswith(JUGGLER_DIR_REL + "/") for n in names):
                raise SystemExit("validate_release: omni.ja has no chrome/juggler/ - "
                                 "Playwright cannot drive this build")
            marked = 0
            for entry in JUGGLER_ENTRIES:
                if entry not in names:
                    raise SystemExit(f"validate_release: juggler entry missing from omni.ja: "
                                     f"{entry}")
                if any(m in zf.read(entry) for m in JUGGLER_MARKERS):
                    marked += 1
    elif (root / "chrome" / "juggler").is_dir():
        layout = "unpacked tree"
        marked = 0
        for entry in JUGGLER_ENTRIES:
            p = root / entry
            if not p.is_file():
                raise SystemExit(f"validate_release: juggler entry missing from the tree: "
                                 f"{entry}")
            if any(m in p.read_bytes() for m in JUGGLER_MARKERS):
                marked += 1
    else:
        raise SystemExit(
            f"validate_release: no omni.ja and no {JUGGLER_DIR_REL}/ in {root} - "
            f"Playwright cannot drive this build, and invisible_core refuses to launch it")

    if marked < len(JUGGLER_ENTRIES):
        raise SystemExit(
            f"validate_release: only {marked}/{len(JUGGLER_ENTRIES)} juggler entries carry a "
            f"stealth marker ({b'/'.join(JUGGLER_MARKERS).decode()}) in the {layout} layout. "
            f"The launch-time provenance guard keys on these; publishing this build would "
            f"erode it silently.")
    print(f"    juggler provenance OK ({marked}/{len(JUGGLER_ENTRIES)} entries marked, "
          f"{layout} layout)")


UPDATER_FILES = (
    "updater.exe", "updater", "updater.ini", "update-settings.ini",
    "maintenanceservice.exe", "maintenanceservice_installer.exe", "updateagent",
)
UPDATER_MODULES = (
    "UpdateService.sys.mjs", "UpdateListener.sys.mjs", "UpdateServiceStub.sys.mjs",
    "AppUpdater.sys.mjs", "BackgroundUpdate.sys.mjs",
)


def assert_no_updater(tree: Path) -> None:
    """No update machinery may be inside a shipped archive.

    It is not switched off by a pref or by a policy: the build OPTIONS were
    removed from build/moz.configure/update-programs.configure on 2026-08-31, so
    MOZ_UPDATER is never set and toolkit/mozapps/update never enters the build.

    That makes it a BUILD-SYSTEM fact, which is exactly the kind a rebase can
    undo in silence - one conflict on that file resolved towards upstream and
    the updater is back, the browser still starts, every JavaScript test still
    passes, and the only symptom is the badge the owner asked to remove, plus a
    binary able to overwrite the engine its own seal pins.

    Two layouts, the same split the juggler check already handles: the Windows
    zip packs modules into omni.ja, the Linux tarball carries the loose
    dist/bin tree.
    """
    import zipfile
    found: list[str] = []

    for p in tree.rglob("*"):
        if p.is_file() and p.name in UPDATER_FILES:
            found.append(str(p.relative_to(tree)))

    # BOTH scans, always. The first draft did the loose one only when there was
    # no omni.ja, and looked at the ROOT omni.ja alone - which a real Windows
    # package immediately falsified: it ships two (omni.ja and
    # browser/omni.ja), and toolkit/mozapps/update modules can land in either.
    # A package can also carry an archive and loose modules at once.
    for omni in sorted(tree.rglob("omni.ja")):
        rel = str(omni.relative_to(tree))
        with zipfile.ZipFile(omni) as zf:
            for n in zf.namelist():
                if n.rsplit("/", 1)[-1] in UPDATER_MODULES:
                    found.append(rel + "!" + n)

    for p in tree.rglob("*.sys.mjs"):
        if p.name in UPDATER_MODULES:
            found.append(str(p.relative_to(tree)))

    if found:
        raise ValidationError(
            "the update machinery is back in this build: "
            + ", ".join(sorted(found)[:8])
            + ". It is removed at the BUILD level - build/moz.configure/"
              "update-programs.configure declares no --enable-updater at all - "
              "so this means a rebase restored that file, or a mozconfig "
              "re-added the option. Do not publish it: it brings back the "
              "'Update available' badge and a binary that can overwrite the "
              "engine its seal pins.")
    print("    no updater in the package (files and modules both absent)")


def smoke_linux(extracted: Path) -> None:
    """Run `firefox --version` inside WSL when invoked from Windows."""
    firefox = extracted / "firefox"
    if not firefox.exists():
        raise ValidationError("firefox binary not found after extract")

    if os.name == "nt":
        wsl_path = "/mnt/" + str(firefox).replace("\\", "/").replace("C:/", "c/")
        cmd = ["wsl.exe", "-d", "Ubuntu", "--", wsl_path, "--version"]
    else:
        cmd = [str(firefox), "--version"]

    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        raise ValidationError("firefox --version timed out (binary may be broken)")
    output = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        raise ValidationError(
            f"firefox --version exited {r.returncode}.\nOutput:\n{output[:1000]}"
        )
    if not VERSION_RE.search(output):
        raise ValidationError(
            f"firefox --version output didn't match expected pattern.\n{output[:500]}"
        )
    print(f"    smoke OK: {VERSION_RE.search(output).group(0)}")


def smoke_windows(extracted: Path) -> None:
    """Run firefox.exe --version on Windows."""
    firefox = extracted / "firefox.exe"
    if not firefox.exists():
        raise ValidationError("firefox.exe not found after extract")
    try:
        r = subprocess.run(
            [str(firefox), "--version"],
            capture_output=True, text=True, timeout=30,
        )
    except subprocess.TimeoutExpired:
        raise ValidationError("firefox.exe --version timed out")
    output = (r.stdout or "") + (r.stderr or "")
    if r.returncode != 0:
        raise ValidationError(
            f"firefox.exe --version exited {r.returncode}.\nOutput:\n{output[:1000]}"
        )
    if not VERSION_RE.search(output):
        raise ValidationError(
            f"firefox.exe --version output didn't match expected pattern.\n{output[:500]}"
        )
    print(f"    smoke OK: {VERSION_RE.search(output).group(0)}")


def validate_linux_tarball(archive: Path) -> None:
    print(f"[validate] LINUX  {archive.name}")
    if not archive.exists():
        raise ValidationError(f"archive not found: {archive}")

    print("  [1/4] no-symlinks check...")
    check_no_symlinks_tar(archive)

    print("  [2/4] path-leak check...")
    check_no_leaks_archive(archive)

    print("  [3/4] extract + critical-files check...")
    with tempfile.TemporaryDirectory(prefix="ff-validate-linux-") as td:
        td = Path(td)
        extract_tar(archive, td)
        check_critical_files(td, CRITICAL_LINUX)
        assert_juggler_provenance(td)
        assert_no_updater(td)

        print("  [4/4] smoke (firefox --version)...")
        smoke_linux(td)


def validate_windows_zip(archive: Path) -> None:
    print(f"[validate] WIN    {archive.name}")
    if not archive.exists():
        raise ValidationError(f"archive not found: {archive}")

    print("  [1/4] path-leak check...")
    check_no_leaks_archive(archive)

    print("  [2/4] extract + critical-files check...")
    with tempfile.TemporaryDirectory(prefix="ff-validate-win-") as td:
        td = Path(td)
        extract_zip(archive, td)
        check_critical_files(td, CRITICAL_WIN)
        assert_juggler_provenance(td)
        assert_no_updater(td)

        print("  [3/4] no-symlinks check (zip cannot store them, but verify)...")
        for p in td.rglob("*"):
            if p.is_symlink():
                raise ValidationError(f"unexpected symlink in zip: {p}")

        print("  [4/4] smoke (firefox.exe --version)...")
        if os.name == "nt":
            smoke_windows(td)
        else:
            print("    smoke SKIPPED (not on Windows; run on Windows to verify)")


def _selftest() -> int:
    """Known-bad inputs for assert_no_updater, and the near-misses that must NOT fire.

    A check that has only ever printed its OK line is not a gate. This one is
    cheap to exercise because it is a pure function of a directory: no archive,
    no browser, no network.
    """
    import shutil
    import tempfile
    import zipfile

    def tree(*extra, omni_members=(), nested_omni=()):
        d = Path(tempfile.mkdtemp(prefix="vr-selftest-"))
        (d / "chrome" / "juggler").mkdir(parents=True)
        (d / "modules").mkdir()
        (d / "firefox.exe").write_bytes(b"x")
        (d / "modules" / "AppConstants.sys.mjs").write_bytes(b"x")
        for rel in extra:
            f = d / rel
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(b"x")
        if omni_members:
            with zipfile.ZipFile(d / "omni.ja", "w") as zf:
                for m in omni_members:
                    zf.writestr(m, "x")
        if nested_omni:
            (d / "browser").mkdir(exist_ok=True)
            with zipfile.ZipFile(d / "browser" / "omni.ja", "w") as zf:
                for m in nested_omni:
                    zf.writestr(m, "x")
        return d

    #: (name, factory, must_raise)
    cases = [
        ("a clean tree", lambda: tree(), False),
        ("updater.exe at the root", lambda: tree("updater.exe"), True),
        ("a bare linux updater", lambda: tree("updater"), True),
        ("updater.ini", lambda: tree("updater.ini"), True),
        ("update-settings.ini", lambda: tree("update-settings.ini"), True),
        ("the maintenance service", lambda: tree("maintenanceservice.exe"), True),
        ("the loose UpdateListener module",
         lambda: tree("modules/UpdateListener.sys.mjs"), True),
        ("UpdateService packed INSIDE omni.ja",
         lambda: tree(omni_members=("modules/UpdateService.sys.mjs",)), True),
        # A real Windows package ships two archives, and the first draft of this
        # check read only the one at the root.
        ("BackgroundUpdate inside the NESTED browser/omni.ja",
         lambda: tree(nested_omni=("modules/BackgroundUpdate.sys.mjs",)), True),
        ("a loose module BESIDE a clean omni.ja",
         lambda: tree("modules/AppUpdater.sys.mjs",
                      omni_members=("modules/AppConstants.sys.mjs",)), True),
        # The omni.ja arm must not be blind to what sits beside it, and the
        # loose arm must not fire on a build that legitimately has an omni.ja.
        ("an omni.ja with nothing of ours in it",
         lambda: tree(omni_members=("modules/AppConstants.sys.mjs",)), False),
        # Near-misses. A gate that refuses everything is as useless as one that
        # passes everything.
        ("a file merely NAMED like an update", lambda: tree("updates.txt"), False),
        ("a directory called updater", lambda: tree("updater/keep.txt"), False),
    ]

    bad = 0
    for name, factory, must_raise in cases:
        d = factory()
        try:
            assert_no_updater(d)
            raised = False
        except ValidationError:
            raised = True
        finally:
            shutil.rmtree(d, ignore_errors=True)
        ok = raised == must_raise
        bad += 0 if ok else 1
        verdict = "ok" if ok else "GATE WRONG"
        expect = "must refuse" if must_raise else "must pass"
        print(f"  [{verdict:9}] {name}: {expect}, "
              f"{'refused' if raised else 'passed'}")

    fired = sum(1 for _, _, m in cases if m)
    print(f"[selftest] {len(cases)} cases ({fired} known-bad, "
          f"{len(cases) - fired} that must not fire), {bad} wrong")
    return 1 if bad else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true",
                    help="run the known-bad inputs for the no-updater check and exit")
    ap.add_argument("tag", nargs="?", help="release tag, e.g. firefox-4 (resolves to release/binary/<tag>/)")
    ap.add_argument("--linux", type=Path, help="explicit path to the Linux tar.gz (overrides tag)")
    ap.add_argument("--win", type=Path, help="explicit path to the Windows zip (overrides tag)")
    ap.add_argument("--linux-only", action="store_true")
    ap.add_argument("--win-only", action="store_true")
    ap.add_argument(
        "--no-site-patterns", action="store_true",
        help=(f"run without the {EXTRA_PATTERNS_ENV} token list and still exit 0. "
              f"For callers that cannot have it - the CI per-leg pre-packaging check "
              f"runs on a machine where the list does not exist, and the list must "
              f"stay out of this repository. NEVER for the pre-publish run: the "
              f"whole point of that run is the scan this flag turns off."))
    args = ap.parse_args()

    if args.selftest:
        return _selftest()

    # Resolved first, so the operator reads what will be scanned before the
    # scanning starts, and so the verdict below is decided even if every archive
    # turns out to be missing.
    _all_leak_patterns()

    if args.linux or args.win:
        linux_archive = args.linux or Path("/dev/null")
        win_archive = args.win or Path("/dev/null")
    else:
        if not args.tag:
            ap.error("provide either <tag> or --linux/--win paths")
        base = REPO_ROOT / "release" / "binary" / args.tag
        linux_archive = base / f"firefox-{UPSTREAM_VERSION}-stealth-linux-x86_64.tar.gz"
        win_archive = base / f"firefox-{UPSTREAM_VERSION}-stealth-win-x86_64.zip"

    failures: list[str] = []

    if not args.win_only:
        try:
            validate_linux_tarball(linux_archive)
            print("  -> LINUX OK\n")
        except ValidationError as e:
            failures.append(f"LINUX: {e}")
            print(f"  -> LINUX FAILED: {e}\n")

    if not args.linux_only:
        try:
            validate_windows_zip(win_archive)
            print("  -> WIN OK\n")
        except ValidationError as e:
            failures.append(f"WIN: {e}")
            print(f"  -> WIN FAILED: {e}\n")

    if failures:
        print(f"[validate] {len(failures)} failure(s):", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
        print("[validate] ABORT - do NOT publish this release", file=sys.stderr)
        return 1

    # Everything that RAN passed. Whether everything ran is a separate question,
    # and it is the one that used to go unanswered: the site-name half of the
    # leak scan was skipped with a warning that no caller could see, because a
    # caller sees an exit code. An incomplete scan is not a pass.
    if not _EXTRA_APPLIED:
        if args.no_site_patterns:
            print("[validate] all checks passed, EXCEPT the site-name leak scan, which "
                  "was explicitly waived with --no-site-patterns.")
            print("[validate] Nothing here says these archives are free of site names. "
                  "The pre-publish run must be done with the token list.")
            return 0
        print(file=sys.stderr)
        print("[validate] INCOMPLETE - the site-name leak scan did not run.", file=sys.stderr)
        print(f"[validate] Set {EXTRA_PATTERNS_ENV} to the token list (one token per "
              f"line; it lives OUTSIDE this repository on purpose, because a scanner "
              f"that carries the tokens it scans for publishes them) and run again.",
              file=sys.stderr)
        print("[validate] If this caller genuinely cannot have the list, say so with "
              "--no-site-patterns; do not let a partial scan pass as a full one.",
              file=sys.stderr)
        return 3

    print("[validate] all checks passed - safe to publish")
    return 0


if __name__ == "__main__":
    sys.exit(main())
