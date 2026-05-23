#!/usr/bin/env python3
"""Apply a generated patch series (from update_patches.py) to a fresh clone
of mozilla-firefox/firefox.

Verifies that the snapshot is genuinely reproducible — i.e. a new contributor
can clone vanilla Firefox + apply patches + build, without needing access to
the feder-cr/invisible_firefox fork branch.

This is the inverse of update_patches.py: it consumes what update_patches.py
produces.  Both are read-only against the current repo.

Usage:
  python scripts/apply_patches.py /path/to/fresh/firefox-clone \\
         --patches ../firefox-stealth/ \\
         --base FIREFOX_150_0_1_RELEASE

What it does:
  1. cd into the target clone
  2. git checkout <base-tag>
  3. git am --3way <patch-dir>/*.patch
  4. Report success / first conflict

Exits non-zero on first failure.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, check=check, text=True,
                          capture_output=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", type=Path,
                        help="Path to fresh mozilla-firefox/firefox clone")
    parser.add_argument("--patches", type=Path, required=True,
                        help="Path to firefox-stealth dir containing *.patch")
    parser.add_argument("--base", required=True,
                        help="Upstream tag to checkout before applying "
                             "(e.g. FIREFOX_150_0_1_RELEASE)")
    args = parser.parse_args()

    if not (args.target / ".git").exists():
        print(f"ERROR: {args.target} is not a git repo", file=sys.stderr)
        return 1
    if not args.patches.is_dir():
        print(f"ERROR: --patches {args.patches} is not a directory",
              file=sys.stderr)
        return 1

    patch_files = sorted(args.patches.glob("*.patch"))
    if not patch_files:
        print(f"ERROR: no *.patch files in {args.patches}", file=sys.stderr)
        return 1

    print(f"[apply_patches] Target:   {args.target}")
    print(f"[apply_patches] Patches:  {args.patches} ({len(patch_files)})")
    print(f"[apply_patches] Base tag: {args.base}")
    print()

    # Ensure clean working tree
    status = run(["git", "status", "--porcelain"], args.target)
    if status.stdout.strip():
        print("ERROR: target has uncommitted changes — clean it first",
              file=sys.stderr)
        return 1

    # Verify base tag exists
    rc = subprocess.run(
        ["git", "rev-parse", "--verify", args.base],
        cwd=args.target, capture_output=True)
    if rc.returncode != 0:
        print(f"ERROR: tag {args.base} not found in target. Fetch first.",
              file=sys.stderr)
        return 1

    print(f"--- Checking out {args.base} ---")
    run(["git", "checkout", "-B", "stealth-apply-check", args.base], args.target)

    print(f"--- Applying {len(patch_files)} patches ---")
    cmd = ["git", "am", "--3way", "--keep-cr"]
    cmd.extend(str(p) for p in patch_files)
    rc = subprocess.run(cmd, cwd=args.target, text=True)
    if rc.returncode != 0:
        print()
        print("APPLY FAILED. The clone is in 'git am' in-progress state.")
        print("Inspect with: git status -- in", args.target)
        print("Resume with:  git am --continue   (after fixing conflicts)")
        print("Abort with:   git am --abort")
        return 1

    final_sha = run(["git", "rev-parse", "--short", "HEAD"], args.target).stdout.strip()
    print()
    print(f"APPLY OK — target now at {final_sha}")
    print()
    print(f"Sanity check vs source-of-truth branch:")
    print(f"  cd {args.target}")
    print(f"  git diff stealth-apply-check..stealth/<N>")
    print(f"  # should be empty if patches are complete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
