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
import json
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


#: The containers inside a Firefox tree whose members are compressed one by one,
#: so the bytes on disk do not show what they carry.
NESTED_ARCHIVES = (".ja", ".jar", ".xpi", ".zip")


def _payloads(root: Path):
    """(where, bytes) for every file of an extracted tree, and for every member
    of every nested archive in it (NESTED_ARCHIVES), read with Mozilla's own jar
    reader because the omni.ja Mozilla packs are in a layout zipfile refuses."""
    from stock_parity import open_jar
    for p in sorted(root.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        data = p.read_bytes()
        yield rel, data
        if p.suffix.lower() in NESTED_ARCHIVES:
            try:
                jar = open_jar(data, rel)
                members = [e.filename for e in jar if not e.filename.endswith("/")]
            except Exception as exc:  # an archive that cannot be read cannot be cleared
                raise ValidationError(f"{rel}: cannot open it to scan its members: {exc}") from exc
            for name in members:
                yield f"{rel}!{name}", jar[name].read()


def check_no_leaks_tree(root: Path) -> None:
    """No build path and no site name in any file of the extracted tree, nor in
    any member of an archive inside it.

    ⛔ IT USED TO SCAN THE ARCHIVE'S RAW BYTES (check_no_leaks_archive), and a zip
    compresses each member on its own: on every Windows zip the scan saw the
    member names and the stored members only, and on both platforms nothing
    inside omni.ja or an .xpi. Measured on 2026-10-15 with a deflated zip member
    and an omni.ja inside a tarball, each carrying a home directory: both passed.
    """
    patterns = _all_leak_patterns()
    hits: dict = {}
    scanned = 0
    for where, data in _payloads(root):
        scanned += 1
        for pat in patterns:
            if pat.search(data):
                hits.setdefault(pat.pattern.decode(), []).append(where)
    if hits:
        shown = {k: v[:5] + ([f"... {len(v) - 5} more"] if len(v) > 5 else []) for k, v in hits.items()}
        raise ValidationError(f"leak hits: {shown}")
    print(f"    no leaks in {scanned} files and nested members")


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
    "modules/juggler/protocol/PageHandler.js",
    "modules/juggler/TargetRegistry.js",
    "modules/juggler/content/main.js",
    "modules/juggler/content/Runtime.js",
)
# Since firefox-47 juggler is not a chrome package: it loads from
# resource://gre/modules/juggler/ (juggler/moz.build says why).
JUGGLER_DIR_REL = "modules/juggler"
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
    the pre-package dist/bin layout, where the juggler is loose in the tree
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
        # Mozilla's reader, not zipfile: a PGO build packs omni.ja in the
        # optimized layout the retail has (central directory first), which
        # zipfile refuses with "Bad magic number for central directory".
        import stock_parity
        jar = stock_parity.open_jar(omni.read_bytes(), str(omni))
        names = set(jar.entries)
        if not any(n.startswith(JUGGLER_DIR_REL + "/") for n in names):
            raise SystemExit(f"validate_release: omni.ja has no {JUGGLER_DIR_REL}/ - "
                             "Playwright cannot drive this build")
        marked = 0
        for entry in JUGGLER_ENTRIES:
            if entry not in names:
                raise SystemExit(f"validate_release: juggler entry missing from omni.ja: "
                                 f"{entry}")
            if any(m in jar[entry].read() for m in JUGGLER_MARKERS):
                marked += 1
    elif (root / JUGGLER_DIR_REL).is_dir():
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


def flavor() -> str:
    """The engine flavor the CI builds (STEALTH_FLAVOR): full unless it says minimal."""
    value = os.environ.get("STEALTH_FLAVOR", "") or "full"
    if value not in ("full", "minimal"):
        raise ValidationError(f"STEALTH_FLAVOR={value!r}: full or minimal")
    return value


# What --enable-stealth-minimal leaves out of the build (minimal/build in the series):
# omni.ja prefixes and files at the root that must NOT be there.
MINIMAL_ABSENT_ENTRIES = ("modules/juggler/", "chrome/remote/")
MINIMAL_ABSENT_FILES = ("crashreporter", "crashreporter.exe", "crashhelper", "crashhelper.exe",
                        "minidump-analyzer", "minidump-analyzer.exe", "notification-helper.exe",
                        "notificationserver.dll")
# Strings only Glean's initialization uses (the glean.init thread, its upload metrics):
# with Glean removed at the source, LTO leaves them out of the library.
GLEAN_RUNTIME_MARKERS = (b"glean.init", b"glean.upload")


def assert_minimal_leaves_out(tree: Path) -> None:
    """The minimal engine carries none of the components it leaves out. Glean's
    runtime is reported, not refused, until a first minimal build has measured it."""
    root = _juggler_root(tree)
    present = [f for f in MINIMAL_ABSENT_FILES if (root / f).exists()]
    omni = root / "omni.ja"
    if omni.exists():
        import stock_parity
        names = stock_parity.open_jar(omni.read_bytes(), str(omni)).entries
        present += [p for p in MINIMAL_ABSENT_ENTRIES if any(n.startswith(p) for n in names)]
    elif (root / "modules" / "juggler").is_dir():
        present.append("modules/juggler/")
    for lib in ("xul.dll", "libxul.so"):
        if (root / lib).exists():
            data = (root / lib).read_bytes()
            found = [m.decode() for m in GLEAN_RUNTIME_MARKERS if m in data]
            print(f"    Glean runtime markers in {lib}: {found or 'none'} (reported, not refused yet)")
    if present:
        raise ValidationError(f"the minimal engine still carries: {', '.join(present)}")
    print("    minimal engine leaves out " + ", ".join(MINIMAL_ABSENT_ENTRIES + ("crash reporter",)))


def assert_engine_contents(tree: Path) -> None:
    """Full: the juggler is there and marked. Minimal: what it leaves out is gone."""
    if flavor() == "minimal":
        assert_minimal_leaves_out(tree)
    else:
        assert_juggler_provenance(tree)


def assert_page_surface_for_flavor(tree: Path) -> None:
    """Full: the page surface is the retail's, refused otherwise. Minimal: the same
    judgement, printed in full and not refused, until its differences are declared."""
    if flavor() != "minimal":
        assert_page_surface_is_stock(tree)
        return
    try:
        assert_page_surface_is_stock(tree)
    except ValidationError as exc:
        print(f"    MINIMAL page surface, reported not refused: {exc}")


def declared_branding() -> Path:
    """The branding archive invisible_core declares by default, asked of the
    installed core the way a launch asks it - a profile translated to prefs -
    so that this gate holds no default and no second copy of where it lives.
    The release workflow installs the core it pairs with the build first."""
    import urllib.parse
    import urllib.request
    try:
        from invisible_core import generate_profile, translate_profile_to_prefs
    except ImportError as exc:
        raise ValidationError(
            "this engine serves the branding invisible_core declares, and no invisible_core "
            f"is installed to say which: {exc}") from exc
    # A launch declares the session's zone, and since invisible_core 49.46.0 a
    # profile translated without one is refused, because an undeclared zone is
    # the host's. The branding does not depend on the zone: any IANA name the
    # core accepts asks the question a launch asks. Without it, every release
    # run of firefox-50 died here, at the packaging step of all three legs.
    prefs = translate_profile_to_prefs(generate_profile(seed=1), timezone="UTC")
    spec = prefs.get("zoom.stealth.branding.archive")
    if not spec:
        raise ValidationError("the installed invisible_core declares no zoom.stealth.branding.archive: "
                              "it is older than the engine, and the engine refuses to launch with it")
    return Path(urllib.request.url2pathname(urllib.parse.urlparse(spec).path))


def assert_page_surface_is_stock(tree: Path) -> None:
    """What a web page can read of this build - every chrome package and
    resource substitution it registers, with their flags, and the bytes of
    every file a page can load - must be what the retail Firefox of the same
    version and platform has. The judge is Mozilla's own complete MAR for that
    release, checked against its SHA256SUMS: see stock_parity.py, which also
    holds the differences declared as invisible to a page and why.

    It replaces assert_declares_nothing_beyond_stock (firefox-47), which
    compared only the registered NAMES, only one way (ours not beyond stock),
    against a list kept by hand whose Linux half was derived from Windows. A
    name stock has and we lack is probed the same way, and a file of a
    readable package that differs is read the same way: both were invisible to
    it. Run on firefox-47 on 2026-10-15 this gate found 44 differences on
    Windows and 87 on each Linux architecture, the names gate none.

    On an engine that serves the branding invisible_core declares (P60) the
    page reads that directory, not the build's own brand, so it is judged in
    its place: declared_branding() asks the installed core for it.
    """
    import stock_parity
    root = _juggler_root(tree)
    try:
        branding = (declared_branding()
                    if stock_parity.serves_declared_branding(root, stock_parity.platform_of(root))
                    else None)
        platform, version, undeclared, stale, size = stock_parity.judge(root, branding=branding)
    except stock_parity.ParityError as exc:
        raise ValidationError(f"the page surface could not be judged: {exc}") from exc
    if undeclared or stale:
        lines = [f"      {what}: {key}" for key, what in undeclared.items()]
        lines += [f"      declared but not different: {key}" for key in stale]
        shown = "\n".join(lines[:40]) + (f"\n      ... and {len(lines) - 40} more" if len(lines) > 40 else "")
        raise ValidationError(
            f"this build differs from the retail Firefox {version} ({platform}) in "
            f"{len(undeclared)} thing(s) a page can read, and {len(stale)} declaration(s) "
            f"excuse a difference that is gone:\n{shown}\n"
            f"    A difference a page cannot observe is declared in "
            f"{stock_parity.EXCEPTIONS.name} with its measurement; anything else is fixed.")
    print(f"    page surface is stock OK ({platform} {version}, {size} keys)")


def assert_windows_runtime_is_shipped(tree: Path) -> None:
    """Every DLL the build's binaries import, directly or by delay load, is
    shipped in the archive or is one the retail of the same version also
    expects from Windows (runtime_imports.py). Until firefox-48 the C++ runtime
    was imported and not shipped, so a clean Windows could not start the
    browser (B306) while every machine that ran the e2e had it installed.
    """
    import runtime_imports
    import stock_parity
    root = _juggler_root(tree)
    try:
        retail = stock_parity.retail_files(stock_parity.version_of(root), stock_parity.platform_of(root))
        missing = runtime_imports.missing_runtime(stock_parity.Files.from_directory(root), retail)
    except stock_parity.ParityError as exc:
        raise ValidationError(f"the imported DLLs could not be judged: {exc}") from exc
    if missing:
        shown = {dll: users[:4] + ([f"... {len(users) - 4} more"] if len(users) > 4 else [])
                 for dll, users in missing.items()}
        raise ValidationError(
            f"the binaries import DLLs that are neither shipped nor provided by Windows "
            f"(the retail ships them): {shown}. The C++ runtime ships with --with-redist.")
    print("    every imported DLL is shipped or provided by Windows OK")


#: The retail's update settings: the MAR channel a release accepts.
RETAIL_MAR_CHANNELS = b"ACCEPTED_MAR_CHANNEL_IDS=firefox-mozilla-release"
#: The calls that keep an answer of the update server from being acted on, as
#: UpdateService.sys.mjs carries them (toolkit/mozapps/update/StealthUpdate.sys.mjs).
UPDATE_REFUSALS = (b"StealthUpdate.noneOffered(", b"StealthUpdate.refusesDownload(")


def assert_updater_is_retails(tree: Path) -> None:
    """The update machinery is the retail's, and it never acts on an answer.

    Since 2026-10-30 build/moz.configure/update-programs.configure is Mozilla's
    file: the updater is compiled and packaged as in a release, and the update
    check goes out as official Firefox sends it. Three facts make that safe and
    the same as the retail, and each is a BUILD fact a rebase or a mozconfig can
    undo in silence, the browser still starting:

    - the updater binary is in the package (else the build is not the retail's:
      the check would not even run the same code);
    - update-settings.ini accepts the retail's MAR channel (Mozilla passes it
      from its CI environment, we from .mozconfig; without it the file is empty);
    - UpdateService.sys.mjs carries our refusals, so no offered update is taken
      and nothing is downloaded: an update would overwrite the engine its seal
      pins.

    Two layouts, the same split the juggler check already handles: the Windows
    zip packs modules into omni.ja (two of them), the Linux tarball may carry
    the loose dist/bin tree.
    """
    problems: list[str] = []

    if not any(p.is_file() and p.name in ("updater.exe", "updater")
               for p in tree.rglob("updater*")):
        problems.append("no updater binary in the package (MOZ_UPDATER off?)")

    settings = [p for p in tree.rglob("update-settings.ini") if p.is_file()]
    if not settings:
        problems.append("no update-settings.ini")
    elif not all(RETAIL_MAR_CHANNELS in p.read_bytes() for p in settings):
        problems.append("update-settings.ini does not accept the retail's MAR "
                        "channel (MAR_CHANNEL_ID / ACCEPTED_MAR_CHANNEL_IDS in .mozconfig)")

    # BOTH scans, always: a real Windows package ships two archives (omni.ja and
    # browser/omni.ja), and a package can carry an archive and loose modules at
    # once.
    services: list[tuple[str, bytes]] = []
    for omni in sorted(tree.rglob("omni.ja")):
        rel = str(omni.relative_to(tree))
        import stock_parity  # Mozilla's reader: optimized jars too
        jar = stock_parity.open_jar(omni.read_bytes(), rel)
        for n in jar.entries:
            if n.rsplit("/", 1)[-1] == "UpdateService.sys.mjs":
                services.append((rel + "!" + n, jar[n].read()))
    for p in tree.rglob("UpdateService.sys.mjs"):
        if p.is_file():
            services.append((str(p.relative_to(tree)), p.read_bytes()))
    if not services:
        problems.append("no UpdateService.sys.mjs in the package")
    for where, body in services:
        missing = [c.decode() for c in UPDATE_REFUSALS if c not in body]
        if missing:
            problems.append(f"{where} lacks {', '.join(missing)}: an offered "
                            "update would be downloaded and applied")

    if problems:
        raise ValidationError(
            "the update machinery is not the retail's with our refusals: "
            + "; ".join(problems) + ". Do not publish it.")
    print("    updater as the retail, offered updates refused OK")


def _build_id(ini: Path) -> str:
    for line in ini.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("BuildID="):
            return line.split("=", 1)[1].strip()
    return ""


def assert_build_ids_agree(tree: Path) -> None:
    """application.ini and platform.ini carry the same BuildID.

    They are generated from the same buildid.h, and a retail build always has
    one BuildID in both (157.0.1: 20261005135250 in each, on every platform).
    The first 157 candidates did not: from 157 the build graph generates
    buildid.h twice in one `mach build` (export, and again near the end), and
    without MOZ_BUILD_DATE each generation takes the clock, so application.ini
    said 13:25 and platform.ini 15:11 (run 40329304164), invisible_core's seal
    check refused the launch (platform.ini's BuildID is not the sealed one), and
    the package was not the retail's in a file every crash report and update
    check carries. MOZ_BUILD_DATE is now fixed once per run (.mozconfig,
    release.yml); this refuses an archive in which it was not.
    """
    app, platform = tree / "application.ini", tree / "platform.ini"
    missing = [p.name for p in (app, platform) if not p.is_file()]
    if missing:
        raise ValidationError(f"{', '.join(missing)} missing at the package root")
    a, p = _build_id(app), _build_id(platform)
    if not a or a != p:
        raise ValidationError(
            f"application.ini BuildID {a or '<none>'} != platform.ini BuildID "
            f"{p or '<none>'}: buildid.h was generated more than once in this build "
            f"without one MOZ_BUILD_DATE. Do not publish it: the seal check refuses "
            f"the launch, and the retail has one BuildID in both.")
    print(f"    one BuildID in application.ini and platform.ini ({a})")


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

    print("  [2/4] extract + critical-files check...")
    with tempfile.TemporaryDirectory(prefix="ff-validate-linux-") as td:
        td = Path(td)
        extract_tar(archive, td)
        check_critical_files(td, CRITICAL_LINUX)
        assert_build_ids_agree(td)
        print("  [3/4] path-leak check, every file and nested member...")
        check_no_leaks_tree(td)
        assert_engine_contents(td)
        assert_page_surface_for_flavor(td)
        assert_updater_is_retails(td)

        print("  [4/4] smoke (firefox --version)...")
        smoke_linux(td)


def validate_windows_zip(archive: Path) -> None:
    print(f"[validate] WIN    {archive.name}")
    if not archive.exists():
        raise ValidationError(f"archive not found: {archive}")

    print("  [1/4] extract + critical-files check...")
    with tempfile.TemporaryDirectory(prefix="ff-validate-win-") as td:
        td = Path(td)
        extract_zip(archive, td)
        check_critical_files(td, CRITICAL_WIN)
        assert_build_ids_agree(td)
        print("  [2/4] path-leak check, every file and nested member...")
        check_no_leaks_tree(td)
        assert_engine_contents(td)
        assert_page_surface_for_flavor(td)
        assert_windows_runtime_is_shipped(td)
        assert_updater_is_retails(td)

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
    """Known-bad inputs for assert_updater_is_retails, and the near-misses that must NOT fire.

    A check that has only ever printed its OK line is not a gate. This one is
    cheap to exercise because it is a pure function of a directory: no archive,
    no browser, no network.
    """
    import shutil
    import tempfile
    import zipfile

    def tree(*extra, omni_members=(), nested_omni=()):
        d = Path(tempfile.mkdtemp(prefix="vr-selftest-"))
        (d / JUGGLER_DIR_REL).mkdir(parents=True)
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

    refusing = b"x = StealthUpdate.noneOffered(u); if (StealthUpdate.refusesDownload()) {}"
    settings_ok = b"[Settings]\n" + RETAIL_MAR_CHANNELS + b"\n"

    def updtree(binary="updater.exe", settings=settings_ok, service=refusing,
                where="omni"):
        """A package with the update machinery; each argument breaks one fact."""
        d = tree()
        if binary:
            (d / binary).write_bytes(b"x")
        if settings is not None:
            (d / "update-settings.ini").write_bytes(settings)
        if service is not None:
            member = "modules/UpdateService.sys.mjs"
            if where == "loose":
                (d / member).write_bytes(service)
            else:
                jar = d / ("browser/omni.ja" if where == "nested" else "omni.ja")
                jar.parent.mkdir(exist_ok=True)
                with zipfile.ZipFile(jar, "w") as zf:
                    zf.writestr(member, service)
        return d

    #: (name, factory, must_raise)
    cases = [
        ("the retail's updater, refusals inside omni.ja", lambda: updtree(), False),
        ("the linux layout: a bare updater, loose modules",
         lambda: updtree(binary="updater", where="loose"), False),
        # A real Windows package ships two archives; the module can be in either.
        ("UpdateService inside the NESTED browser/omni.ja",
         lambda: updtree(where="nested"), False),
        ("a tree with no update machinery at all", lambda: tree(), True),
        ("no updater binary", lambda: updtree(binary=None), True),
        ("a file merely NAMED like the updater",
         lambda: updtree(binary="updater.txt"), True),
        ("no update-settings.ini", lambda: updtree(settings=None), True),
        ("update-settings.ini with an empty channel",
         lambda: updtree(settings=b"[Settings]\nACCEPTED_MAR_CHANNEL_IDS=\n"), True),
        ("no UpdateService module", lambda: updtree(service=None), True),
        ("Mozilla's UpdateService, without our refusals",
         lambda: updtree(service=b"updates = this.#parseUpdates(request);"), True),
        ("only one of the two refusals",
         lambda: updtree(service=b"StealthUpdate.noneOffered(u)"), True),
        ("a loose Mozilla UpdateService BESIDE a refusing omni.ja",
         lambda: (lambda d: ((d / "modules/UpdateService.sys.mjs").write_bytes(b"x"), d)[1])(
             updtree()), True),
    ]

    cases = [(name, factory, must_raise, assert_updater_is_retails)
             for name, factory, must_raise in cases]

    leak = b"built in /home/feder/ff-build\n" * 20

    def with_archive(rel, members):
        d = tree()
        (d / rel).parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(d / rel, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for name, body in members.items():
                zf.writestr(name, body)
        return d

    def loose(rel, body):
        d = tree()
        (d / rel).parent.mkdir(parents=True, exist_ok=True)
        (d / rel).write_bytes(body)
        return d

    leaks = [
        ("a clean tree", lambda: tree(), False),
        ("a home directory in a loose file", lambda: loose("buildconfig.html", leak), True),
        # The two the raw-bytes scan could not see (2026-10-15).
        ("a home directory in a DEFLATED member of omni.ja",
         lambda: with_archive("omni.ja", {"chrome/toolkit/content/global/buildconfig.html": leak}), True),
        ("a home directory inside a built-in .xpi",
         lambda: with_archive("browser/features/x.xpi", {"manifest.json": leak}), True),
        ("a nested archive that cannot be opened is not cleared",
         lambda: loose("browser/features/broken.xpi", b"PK not really a zip"), True),
        ("the same words without a home directory",
         lambda: with_archive("omni.ja", {"a.txt": b"built in /builds/worker"}), False),
    ]
    cases += [(name, factory, must_raise, check_no_leaks_tree)
              for name, factory, must_raise in leaks]

    def inis(app_id, platform_id):
        d = tree()
        if app_id is not None:
            (d / "application.ini").write_text(f"[App]\nVersion=157.0.1\nBuildID={app_id}\n")
        if platform_id is not None:
            (d / "platform.ini").write_text(f"[Build]\nBuildID={platform_id}\nMilestone=157.0.1\n")
        return d

    build_ids = [
        ("one BuildID in both", lambda: inis("20261005135250", "20261005135250"), False),
        # The first 157 candidates (run 40329304164).
        ("application.ini and platform.ini from two generations of buildid.h",
         lambda: inis("20261026132525", "20261026151134"), True),
        ("platform.ini without a BuildID", lambda: inis("20261005135250", ""), True),
        ("no platform.ini at all", lambda: inis("20261005135250", None), True),
    ]
    cases += [(name, factory, must_raise, assert_build_ids_agree)
              for name, factory, must_raise in build_ids]

    # A PGO build packs omni.ja in Mozilla's optimized layout (central directory
    # first, as the retail's), which zipfile refuses: the first PGO candidate
    # (run 40446702889) failed here with "Bad magic number for central
    # directory" on a Juggler that was there.
    import stock_parity  # noqa: F401  (puts mozpack on the path)
    from mozpack.mozjar import JarWriter

    def omni(members: dict, optimized: bool):
        d = tree()
        if optimized:
            # A preload list is what makes the layout optimized: a PGO build
            # passes the jarlog, and the archive opens with the central
            # directory and the preloaded members first.
            with JarWriter(file=str(d / "omni.ja")) as jar:
                for name, body in members.items():
                    jar.add(name, body)
                jar.preload(list(members)[:1])
        else:
            with zipfile.ZipFile(d / "omni.ja", "w") as zf:
                for name, body in members.items():
                    zf.writestr(name, body)
        return d

    juggler = {e: b"// stealthfox" for e in JUGGLER_ENTRIES}

    def provenance(d):
        try:
            assert_juggler_provenance(d)
        except SystemExit as exc:
            raise ValidationError(str(exc))

    cases += [
        ("the Juggler in a plain omni.ja", lambda: omni(juggler, False), False, provenance),
        ("the Juggler in an OPTIMIZED omni.ja (a PGO build, the retail's layout)",
         lambda: omni(juggler, True), False, provenance),
        ("an optimized omni.ja without the Juggler",
         lambda: omni({"modules/AppConstants.sys.mjs": b"x"}, True), True, provenance),
        ("Mozilla's UpdateService packed inside an OPTIMIZED omni.ja",
         lambda: omni({"modules/UpdateService.sys.mjs": b"x"}, True), True,
         assert_updater_is_retails),
    ]

    def minimal(d):
        os.environ["STEALTH_FLAVOR"] = "minimal"
        try:
            assert_engine_contents(d)
        finally:
            os.environ.pop("STEALTH_FLAVOR", None)

    def with_root_file(name):
        d = omni({"modules/AppConstants.sys.mjs": b"x"}, True)
        (d / name).write_bytes(b"x")
        return d

    cases += [
        ("a minimal engine without Juggler, remote agent or crash reporter",
         lambda: omni({"modules/AppConstants.sys.mjs": b"x"}, True), False, minimal),
        ("a minimal engine that still packs the Juggler", lambda: omni(juggler, True), True, minimal),
        ("a minimal engine that still packs the remote agent",
         lambda: omni({"chrome/remote/content/components/RemoteAgent.sys.mjs": b"x"}, True), True, minimal),
        ("a minimal engine that still ships the crash reporter",
         lambda: with_root_file("crashreporter.exe"), True, minimal),
    ]

    bad = 0
    for name, factory, must_raise, check in cases:
        d = factory()
        try:
            check(d)
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

    fired = sum(1 for _, _, m, _ in cases if m)
    print(f"[selftest] {len(cases)} cases ({fired} known-bad, "
          f"{len(cases) - fired} that must not fire), {bad} wrong")
    # The page-surface gate carries its own known-bad cases, on trees it builds.
    import runtime_imports
    import stock_parity
    parity = stock_parity.selftest()
    runtime = runtime_imports.selftest()
    return 1 if bad or parity or runtime else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true",
                    help="run the known-bad inputs for the updater and the "
                         "page-surface checks and exit")
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
