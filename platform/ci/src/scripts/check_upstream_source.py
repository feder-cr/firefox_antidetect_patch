#!/usr/bin/env python3
"""Fail when browser/config/upstream_source.txt is not the source of the version
this tree builds.

Why this exists
---------------
The .mozconfig builds as MOZILLA_OFFICIAL, and an official build states its
source repository and changeset (source-repo.h, application.ini SourceRepository
and SourceStamp). Built from a git checkout of this fork, the build cannot name
them, so upstream_source.txt states the retail's values and the .mozconfig
exports them as MOZ_SOURCE_REPO / MOZ_SOURCE_CHANGESET.

Until 157 nothing noticed when they were missing: packager.mk extracted the URL
with awk, which printed nothing, and every binary up to firefox-53 shipped
without SourceRepository and SourceStamp while the retail has both. From 157
make_sourcestamp_file.py refuses the empty header and `mach package` fails.
The values are per release, so a port that moves the base without updating the
file would state the previous release's changeset: that is what this refuses.

Usage:
  python scripts/check_upstream_source.py            # the version matches the tree
  python scripts/check_upstream_source.py --online   # and the values match Mozilla's record
Exit 0 = aligned, 1 = drift or a malformed file.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE_FILE = ROOT / "browser" / "config" / "upstream_source.txt"
VERSION_FILE = ROOT / "browser" / "config" / "version_display.txt"
KEYS = ("version", "repo", "changeset")
CANDIDATES = "https://archive.mozilla.org/pub/firefox/candidates/{v}-candidates/"
RECORD = CANDIDATES + "{build}/linux-x86_64/en-US/firefox-{v}.json"


class SourceError(ValueError):
    pass


def parse(text: str) -> dict[str, str]:
    """The three values of upstream_source.txt, or SourceError."""
    values: dict[str, str] = {}
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition("=")
        if not sep or key not in KEYS or key in values or not value:
            raise SourceError(f"line {n}: expected one of {KEYS} as key=value, once: {line!r}")
        values[key] = value
    missing = [k for k in KEYS if k not in values]
    if missing:
        raise SourceError(f"missing {', '.join(missing)}")
    if not re.fullmatch(r"[0-9a-f]{40}", values["changeset"]):
        raise SourceError(f"changeset is not a 40-hex revision: {values['changeset']!r}")
    if not values["repo"].startswith("https://"):
        raise SourceError(f"repo is not an https URL: {values['repo']!r}")
    return values


def check(values: dict[str, str], tree_version: str) -> None:
    """SourceError unless the file describes the version the tree builds."""
    if values["version"] != tree_version:
        raise SourceError(
            f"upstream_source.txt describes {values['version']} but the tree builds "
            f"{tree_version}: take repo and changeset from Mozilla's record of "
            f"{tree_version} (see the file's header) and update all three lines")


def mozilla_record(version: str) -> dict:
    """Mozilla's build record of the release (its last candidate build)."""
    listing = urllib.request.urlopen(CANDIDATES.format(v=version), timeout=30).read().decode()
    builds = sorted(set(re.findall(r"build(\d+)/", listing)), key=int)
    if not builds:
        raise SourceError(f"no candidate builds listed for {version}")
    url = RECORD.format(v=version, build="build" + builds[-1])
    return json.load(urllib.request.urlopen(url, timeout=30))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--online", action="store_true",
                    help="also compare repo and changeset with Mozilla's record")
    a = ap.parse_args(argv)
    try:
        values = parse(SOURCE_FILE.read_text(encoding="utf-8"))
        check(values, VERSION_FILE.read_text(encoding="utf-8").strip())
        if a.online:
            rec = mozilla_record(values["version"])
            got = (rec.get("moz_source_repo"), rec.get("moz_source_stamp"))
            if got != (values["repo"], values["changeset"]):
                raise SourceError(f"Mozilla's record of {values['version']} says {got}, "
                                  f"the file says {(values['repo'], values['changeset'])}")
    except (SourceError, OSError) as exc:
        print(f"UPSTREAM SOURCE DRIFT: {exc}")
        return 1
    print(f"UPSTREAM SOURCE PASS - {values['version']} from {values['repo']} "
          f"at {values['changeset']}" + (" (matches Mozilla's record)" if a.online else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
