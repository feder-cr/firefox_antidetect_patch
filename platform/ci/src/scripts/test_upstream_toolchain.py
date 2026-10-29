#!/usr/bin/env python3
"""upstream_toolchain.py against a fake Taskcluster, on the inputs it must refuse.

  known-bad
    1. a profile whose bytes do not match the sha256 of its chain of trust;
    2. a chain of trust that lists no sha256 for the profile;
    3. a changeset with no retail build in the index;
    4. a retail build that fetched no profile;
    5. a Rust toolchain task with no X.Y.Z release (a nightly channel);
  known-good
    6. the Rust release of the toolchain the retail build fetched;
    7. the profile unpacked, and the mozconfig lines with the retail's LTO mode
       (full on x86_64, thin on arm64).

Run it directly: `python3 scripts/test_upstream_toolchain.py`.
"""
from __future__ import annotations

import hashlib
import io
import json
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import upstream_toolchain as u  # noqa: E402

SOURCE = {"repo": "https://hg.mozilla.org/releases/mozilla-release", "changeset": "a" * 40}


def profile_tar() -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:xz") as tar:
        for name, data in (("merged.profdata", b"PROF"), ("en-US.log", b"LOG")):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def fake(profile=None, cot_sha="right", indexed=True, fetches=("rustc", "profdata"),
         channel="1.95.0"):
    profile = profile if profile is not None else profile_tar()
    sha = hashlib.sha256(profile).hexdigest() if cot_sha == "right" else cot_sha
    fetch = []
    if "rustc" in fetches:
        fetch.append({"task": "RUST", "artifact": "public/build/rustc.tar.zst"})
    if "profdata" in fetches:
        fetch.append({"task": "PROF", "artifact": "public/build/profdata.tar.xz"})
    build = {"metadata": {"name": "build"}, "payload": {"env": {"MOZ_FETCHES": json.dumps(fetch)}}}
    rust = {"metadata": {"name": "toolchain-rust"},
            "payload": {"command": ["repack", "--channel", channel]}}
    cot = {"artifacts": {"public/build/profdata.tar.xz": {"sha256": sha}} if sha else {}}

    def get(url):
        if "/index/v1/task/" in url:
            if not indexed:
                raise OSError("404")
            return json.dumps({"taskId": "BUILD"}).encode()
        table = {"/queue/v1/task/BUILD": build, "/queue/v1/task/RUST": rust,
                 "/queue/v1/task/PROF/artifacts/public/chain-of-trust.json": cot}
        for suffix, obj in table.items():
            if url.endswith(suffix):
                return json.dumps(obj).encode()
        if url.endswith("/queue/v1/task/PROF/artifacts/public/build/profdata.tar.xz"):
            return profile
        raise AssertionError("unexpected url %s" % url)
    return get


def refuses(fn):
    try:
        fn()
    except u.UpstreamError:
        return True
    return False


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "pgo"
        cases = [
            ("a profile that does not match its chain of trust",
             refuses(lambda: u.pgo("win-x86_64", out, fake(cot_sha="0" * 64), SOURCE))),
            ("a chain of trust with no sha256 for the profile",
             refuses(lambda: u.pgo("win-x86_64", out, fake(cot_sha=""), SOURCE))),
            ("a changeset with no retail build",
             refuses(lambda: u.rust_version("win-x86_64", fake(indexed=False), SOURCE))),
            ("a retail build that fetched no profile",
             refuses(lambda: u.pgo("win-x86_64", out, fake(fetches=("rustc",)), SOURCE))),
            ("a Rust toolchain with no X.Y.Z release",
             refuses(lambda: u.rust_version("win-x86_64", fake(channel="nightly"), SOURCE))),
            ("the Rust release the retail fetched",
             u.rust_version("win-x86_64", fake(), SOURCE) == "1.95.0"),
        ]
        lines = u.pgo("win-x86_64", out, fake(), SOURCE)
        arm = u.pgo("linux-arm64", out, fake(), SOURCE)
        cases.append(("the profile unpacked, with the retail's LTO mode",
                      (out / "merged.profdata").read_bytes() == b"PROF"
                      and (out / "en-US.log").read_bytes() == b"LOG"
                      and lines[-1] == "ac_add_options --enable-lto=full,cross"
                      and arm[-1] == "ac_add_options --enable-lto=cross"))
    failures = 0
    for name, ok in cases:
        print(("ok    " if ok else "FAIL  ") + name)
        failures += not ok
    print("%d/%d cases as expected" % (len(cases) - failures, len(cases)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
