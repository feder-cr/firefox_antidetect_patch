#!/usr/bin/env python3
"""Regenerate the public patch series at feder-cr/firefox-stealth from the
current stealth/<N> branch.

Modeled on Brave's `npm run update_patches`: the source of truth is the
branch, the .patch files are a generated artifact.

Usage:
  python scripts/update_patches.py
  python scripts/update_patches.py --output ../firefox-stealth/

What it does:
  1. Detects current stealth/<N> branch + matching stealth-base/v<N>.0.1 tag
  2. Runs `git format-patch <base>..HEAD -o <output>` to generate one
     .patch file per commit
  3. Numbers are zero-padded for stable sort
  4. Writes a manifest.txt with commit→patch mapping for sanity-checking

NOT done here (manual step):
  - Bucket-grouping into the 15 logical area patches (canvas, webgl, ...)
  - Adjusting commit-author dates for reproducibility
  - Pushing to feder-cr/firefox-stealth
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path | None = None) -> str:
    res = subprocess.run(cmd, cwd=cwd, check=True, text=True,
                         capture_output=True)
    return res.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output", "-o", type=Path,
        default=Path("../firefox-stealth"),
        help="Output directory (relative to c:/ff/source)",
    )
    parser.add_argument(
        "--branch", default=None,
        help="Branch name (default: current HEAD)",
    )
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    cur_branch = args.branch or run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=repo)

    m = re.match(r"^stealth/(\d+)$", cur_branch)
    if not m:
        print(f"ERROR: current branch '{cur_branch}' is not stealth/<N>",
              file=sys.stderr)
        return 1
    major = m.group(1)
    base_tag = f"stealth-base/v{major}.0.1"

    # Verify base tag exists
    try:
        run(["git", "rev-parse", "--verify", base_tag], cwd=repo)
    except subprocess.CalledProcessError:
        print(f"ERROR: base tag {base_tag} not found", file=sys.stderr)
        return 1

    output = (repo / args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)

    # Clean previous .patch files
    for p in output.glob("*.patch"):
        p.unlink()

    print(f"[update_patches] Branch: {cur_branch}")
    print(f"[update_patches] Base:   {base_tag}")
    print(f"[update_patches] Output: {output}")
    print()

    # Generate patches
    cmd = ["git", "format-patch", f"{base_tag}..HEAD",
           "-o", str(output), "--no-numbered-files"]
    output_text = run(cmd, cwd=repo)
    patch_files = sorted(output.glob("*.patch"))
    print(f"[update_patches] Generated {len(patch_files)} patch files:")
    for p in patch_files:
        print(f"  {p.name}")

    # Manifest
    manifest = output / "MANIFEST.txt"
    log_lines = run(
        ["git", "log", "--oneline", "--no-merges",
         f"{base_tag}..HEAD"], cwd=repo).splitlines()
    with manifest.open("w", encoding="utf-8") as f:
        f.write(f"# Generated from {cur_branch} @ "
                f"{run(['git', 'rev-parse', '--short', 'HEAD'], cwd=repo)}\n")
        f.write(f"# Base: {base_tag}\n")
        f.write(f"# {len(patch_files)} patches, {len(log_lines)} commits\n\n")
        for line in reversed(log_lines):  # chronological order
            f.write(f"{line}\n")
    print(f"[update_patches] Manifest: {manifest}")

    print()
    print("Next steps:")
    print(f"  1. Review: cd {output} && ls *.patch")
    print(f"  2. (Optional) bucket-group into 15 area patches matching README")
    print(f"  3. cd {output} && git add -A && git commit -m 'regen from "
          f"{cur_branch} HEAD' && git push")

    return 0


if __name__ == "__main__":
    sys.exit(main())
