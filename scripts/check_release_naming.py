#!/usr/bin/env python3
"""Fail when the release asset names drift from the version the tree actually builds.

Why this exists
---------------
The archive basename is written by hand in every release workflow (matrix entries
cannot be computed) and independently derived on the client side from
`invisible_core.constants.FIREFOX_UPSTREAM_VERSION`. If the two disagree, CI
publishes `firefox-<A>-stealth-*` while the wrapper downloads
`firefox-<B>-stealth-*`, and every user gets a 404 - the issue-#14 failure mode,
which cost 265 users once already.

Nothing else catches it: the build is green, the archives are valid, and the
release page looks correct. Only a real download fails.

This asserts that every `firefox-<version>-stealth-*` name in the workflows and
release scripts matches `browser/config/version_display.txt`, i.e. the version the
binary is actually built from. Optionally also checks the client constant.

Usage:
  python scripts/check_release_naming.py
  python scripts/check_release_naming.py --core <path to invisible_core/src>
Exit 0 = aligned, 1 = drift.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

TARGETS = (
    ".github/workflows/release.yml",
    ".github/workflows/republish.yml",
    ".github/workflows/verify-assets.yml",
    ".github/workflows/verify-cloak.yml",
    "scripts/linux_release.sh",
    "scripts/make_release.sh",
)

NAME_RE = re.compile(r"firefox-(\d+(?:\.\d+)*)-stealth-")


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", help="path to invisible_core/src, to also check the client constant")
    args = ap.parse_args()

    vd = os.path.join(root, "browser", "config", "version_display.txt")
    if not os.path.exists(vd):
        print("FAIL: %s not found" % vd)
        return 1
    version = open(vd, encoding="utf-8").read().strip()
    print("tree builds Firefox %s (browser/config/version_display.txt)" % version)

    bad, seen = [], 0
    for rel in TARGETS:
        path = os.path.join(root, rel)
        if not os.path.exists(path):
            continue
        for lineno, line in enumerate(open(path, encoding="utf-8", errors="replace"), 1):
            for m in NAME_RE.finditer(line):
                seen += 1
                if m.group(1) != version:
                    bad.append((rel, lineno, m.group(1)))

    print("checked %d asset names across %d files" % (seen, len(TARGETS)))
    if not seen:
        print("FAIL: no asset names found - did the naming convention change?")
        return 1

    if args.core:
        sys.path.insert(0, args.core)
        try:
            from invisible_core.constants import FIREFOX_UPSTREAM_VERSION, BINARY_BASENAME
            if FIREFOX_UPSTREAM_VERSION != version:
                bad.append(("invisible_core/constants.py", 0, FIREFOX_UPSTREAM_VERSION))
            print("client expects %s" % BINARY_BASENAME)
        except Exception as exc:  # noqa: BLE001 - the check is advisory when the pkg is absent
            print("note: could not import invisible_core (%s)" % exc)

    if bad:
        print("\nRELEASE NAMING DRIFT - these would publish (or fetch) the wrong asset:")
        for rel, lineno, found in bad:
            where = "%s:%d" % (rel, lineno) if lineno else rel
            print("  %-44s has %s, tree builds %s" % (where, found, version))
        print("\nEvery user download 404s when these disagree (issue #14).")
        return 1

    print("RELEASE NAMING PASS - asset names match the version the tree builds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
