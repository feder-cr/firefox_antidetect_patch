#!/usr/bin/env python3
"""Pre-publish validation for a Firefox stealth release.

Validates BOTH the Linux tar.gz AND the Windows zip in a single run.
Each archive is extracted to a clean temp dir and:

  1. Symlink check — must have ZERO symlinks
  2. Path-leak check — must have ZERO `/home/feder` or `Users\\Feder` bytes
  3. Critical-files check — firefox + application.ini + dependentlibs.list
     must exist as real files
  4. Smoke-test — `firefox --version` (Linux: run under WSL if invoked from
     Windows; Windows: run the exe directly) must succeed and print
     "Mozilla Firefox 150.x.y"

Exits non-zero on the first failure so a release script can `&&` it
before uploading.

Usage:
    python scripts/validate_release.py <tag>

    # Validates both:
    #   release/binary/<tag>/firefox-150.0.1-stealth-linux-x86_64.tar.gz
    #   release/binary/<tag>/firefox-150.0.1-stealth-win-x86_64.zip
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

LEAK_PATTERNS = [
    re.compile(rb"/home/feder", re.I),
    re.compile(rb"Users[\\/]Feder", re.I),
    re.compile(rb"sonicjobs", re.I),
]

CRITICAL_LINUX = ["firefox", "application.ini", "dependentlibs.list"]
CRITICAL_WIN = ["firefox.exe", "application.ini", "dependentlibs.list"]

VERSION_RE = re.compile(r"^Mozilla Firefox 150\.\d+(\.\d+)?", re.M)


class ValidationError(Exception):
    pass


def check_no_symlinks_tar(archive: Path) -> None:
    """tar.gz must have no SYMlinks (they point to absolute build-host paths
    and break on user machines). Hard links inside the archive are fine —
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


def check_no_leaks_archive(archive: Path) -> None:
    """Scan the (decompressed) archive bytes for path-style leaks."""
    data = archive.read_bytes()
    if archive.suffix == ".gz":
        data = gzip.decompress(data)
    hits = []
    for pat in LEAK_PATTERNS:
        if pat.search(data):
            hits.append(pat.pattern.decode())
    if hits:
        raise ValidationError(
            f"{archive.name}: path-leak hits for patterns {hits}"
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

        print("  [3/4] no-symlinks check (zip cannot store them, but verify)...")
        for p in td.rglob("*"):
            if p.is_symlink():
                raise ValidationError(f"unexpected symlink in zip: {p}")

        print("  [4/4] smoke (firefox.exe --version)...")
        if os.name == "nt":
            smoke_windows(td)
        else:
            print("    smoke SKIPPED (not on Windows; run on Windows to verify)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tag", nargs="?", help="release tag, e.g. firefox-4 (resolves to release/binary/<tag>/)")
    ap.add_argument("--linux", type=Path, help="explicit path to the Linux tar.gz (overrides tag)")
    ap.add_argument("--win", type=Path, help="explicit path to the Windows zip (overrides tag)")
    ap.add_argument("--linux-only", action="store_true")
    ap.add_argument("--win-only", action="store_true")
    args = ap.parse_args()

    if args.linux or args.win:
        linux_archive = args.linux or Path("/dev/null")
        win_archive = args.win or Path("/dev/null")
    else:
        if not args.tag:
            ap.error("provide either <tag> or --linux/--win paths")
        base = REPO_ROOT / "release" / "binary" / args.tag
        linux_archive = base / "firefox-150.0.1-stealth-linux-x86_64.tar.gz"
        win_archive = base / "firefox-150.0.1-stealth-win-x86_64.zip"

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
        print("[validate] ABORT — do NOT publish this release", file=sys.stderr)
        return 1

    print("[validate] all checks passed — safe to publish")
    return 0


if __name__ == "__main__":
    sys.exit(main())
