"""The two legs, as data: Linux and Windows, x86_64, both on the build node.

linux-arm64 is gone from firefox-56 on (Federico, 2026-11-04): no arm64 archive is
built or published. The seal still reads an arm64 name, for the old tags that have one.

Every leg runs the same steps; what differs is here and nowhere else. The
asset names are derived from browser/config/version_display.txt, the version
the tree builds, so a port to a new Firefox changes no name by hand: the
names the release used to write in each matrix entry drifted from the
wrapper's once already, and every download 404'd while the build was green
(issue #14; check_release_naming.py still checks the client side).
"""
from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
VERSION_DISPLAY = ROOT / "browser" / "config" / "version_display.txt"


@dataclass(frozen=True)
class Leg:
    leg: str
    runner: str
    family: str          # linux | win: how the leg packages
    rust_target: str
    # ⛔ host is a property of the RUNNER, not of the leg: every leg on
    # ubuntu-24.04 declares it, or configure stops with "Don't know how to
    # translate x86_64-pc-linux-gnu for rustc" (the image's C compiler reports
    # that name). win-x86_64 cross-compiles from that runner, so it needs it
    # too.
    host: str = ""
    target: str = ""
    extra_pkgs: str = ""
    # Compiled on a build node, and nowhere else (scripts/ci/theta_node.py): such a
    # leg names no runner, so there is nothing for it to run on without the node.
    node: bool = False

    def asset(self, version: str) -> str:
        os_name, arch = self.leg.split("-", 1)
        ext = "zip" if self.family == "win" else "tar.gz"
        return "firefox-%s-stealth-%s-%s.%s" % (version, os_name, arch, ext)


LEGS = (
    Leg("linux-x86_64", "", "linux", "x86_64-unknown-linux-gnu",
        host="x86_64-unknown-linux-gnu", node=True),
    Leg("win-x86_64", "", "win", "x86_64-pc-windows-msvc",
        host="x86_64-unknown-linux-gnu", target="x86_64-pc-windows-msvc",
        extra_pkgs="msitools p7zip-full zip libc6-i386 lib32gcc-s1 lib32stdc++6", node=True),
)


def by_name(name: str) -> Leg:
    for leg in LEGS:
        if leg.leg == name:
            return leg
    raise KeyError("no leg %r (legs: %s)" % (name, ", ".join(l.leg for l in LEGS)))


def tree_version(path: Path = VERSION_DISPLAY) -> str:
    return path.read_text(encoding="utf-8").strip()


def matrix(version: str) -> str:
    """The build job's matrix, for `fromJSON` in release.yml."""
    include = [dict(asdict(leg), asset=leg.asset(version)) for leg in LEGS]
    return json.dumps({"include": include}, separators=(",", ":"))
