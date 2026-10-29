"""The toolchain inputs of the retail build of this tree, fetched from Mozilla's CI.

    python3 scripts/upstream_toolchain.py --leg win-x86_64 rust-version
    python3 scripts/upstream_toolchain.py --leg win-x86_64 pgo --out DIR

Nothing here names a version or a task. Everything starts from
browser/config/upstream_source.txt (the retail's repository and changeset,
check_upstream_source.py) and follows Mozilla's own records:

  Taskcluster index  gecko.v2.<project>.shippable.revision.<changeset>.firefox.<platform>-opt
    -> the retail build task of this changeset and platform
    -> its MOZ_FETCHES: the rustc toolchain task and the PGO profile task it used
    -> rustc: the toolchain task's repack command (--channel X.Y.Z), the
       official Rust release Mozilla repackages. rustup installs that release.
    -> PGO: public/build/profdata.tar.xz of the profile task (merged.profdata
       and en-US.log), checked against the sha256 the same task lists in its
       public/chain-of-trust.json before anything is unpacked.

So a new Firefox needs a new upstream_source.txt and nothing else, and
toolchain_parity.py compares the result with the retail's about:buildconfig
after the build.

The chain of trust's signature (chain-of-trust.json.sig) is not verified: the
sha256 comes from Mozilla's Taskcluster over HTTPS, the same trust the artifact
itself has.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
UPSTREAM_SOURCE = ROOT / "browser" / "config" / "upstream_source.txt"
TC = "https://firefox-ci-tc.services.mozilla.com/api"

#: release.yml's legs -> Mozilla's build platform names.
PLATFORMS = {"win-x86_64": "win64", "linux-x86_64": "linux64", "linux-arm64": "linux64-aarch64"}

#: What Mozilla's own CI builds with LTO: lto-pgo.configure turns MOZ_LTO=cross
#: into FULL LTO when PGO and MOZ_AUTOMATION are on and the target is x86_64
#: Windows or Linux, and into thin LTO otherwise. We do not set MOZ_AUTOMATION
#: (it changes far more than LTO), so the mode the retail got is asked for.
LTO = {"win-x86_64": "full,cross", "linux-x86_64": "full,cross", "linux-arm64": "cross"}


class UpstreamError(Exception):
    pass


def http_get(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=120) as r:
        return r.read()


def upstream_source(path: Path = UPSTREAM_SOURCE) -> dict:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import check_upstream_source
    return check_upstream_source.parse(path.read_text("utf-8"))


def retail_build_task(leg: str, get=http_get, source: dict | None = None) -> dict:
    """The task definition of the retail build of this tree for `leg`."""
    source = source or upstream_source()
    project = source["repo"].rstrip("/").rsplit("/", 1)[-1]
    platform = PLATFORMS[leg]
    route = "gecko.v2.%s.shippable.revision.%s.firefox.%s-opt" % (project, source["changeset"], platform)
    try:
        task_id = json.loads(get("%s/index/v1/task/%s" % (TC, route)))["taskId"]
    except Exception as exc:
        raise UpstreamError("no retail build indexed at %s: %s" % (route, exc)) from None
    return json.loads(get("%s/queue/v1/task/%s" % (TC, task_id)))


def fetched_task(build: dict, artifact_suffix: str) -> str:
    """The task id the build fetched `*artifact_suffix` from."""
    fetches = json.loads(build["payload"].get("env", {}).get("MOZ_FETCHES", "[]"))
    found = [f["task"] for f in fetches if f["artifact"].endswith(artifact_suffix)]
    if len(found) != 1:
        raise UpstreamError("the retail build %s fetched %d artifacts ending %s"
                            % (build["metadata"]["name"], len(found), artifact_suffix))
    return found[0]


def rust_version(leg: str, get=http_get, source: dict | None = None) -> str:
    build = retail_build_task(leg, get, source)
    toolchain = json.loads(get("%s/queue/v1/task/%s" % (TC, fetched_task(build, "/rustc.tar.zst"))))
    m = re.search(r"--channel\W+(\d+\.\d+\.\d+)\b", json.dumps(toolchain["payload"].get("command", "")))
    if not m:
        raise UpstreamError("the retail's Rust toolchain task %s names no X.Y.Z release"
                            % toolchain["metadata"]["name"])
    return m.group(1)


def pgo(leg: str, out: Path, get=http_get, source: dict | None = None) -> list:
    """Download and verify the retail's PGO profile into `out`; the mozconfig
    lines that use it, as the retail build does."""
    build = retail_build_task(leg, get, source)
    task = fetched_task(build, "/profdata.tar.xz")
    name = "public/build/profdata.tar.xz"
    cot = json.loads(get("%s/queue/v1/task/%s/artifacts/public/chain-of-trust.json" % (TC, task)))
    want = cot.get("artifacts", {}).get(name, {}).get("sha256")
    if not want:
        raise UpstreamError("the chain of trust of %s lists no sha256 for %s" % (task, name))
    data = get("%s/queue/v1/task/%s/artifacts/%s" % (TC, task, name))
    got = hashlib.sha256(data).hexdigest()
    if got != want:
        raise UpstreamError("%s of %s is %s, its chain of trust says %s" % (name, task, got, want))
    out.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:xz") as tar:
        members = {m.name: m for m in tar.getmembers()}
        for wanted in ("merged.profdata", "en-US.log"):
            if wanted not in members or not members[wanted].isfile():
                raise UpstreamError("%s of %s has no %s" % (name, task, wanted))
            (out / wanted).write_bytes(tar.extractfile(members[wanted]).read())
    return ["ac_add_options --enable-profile-use=cross",
            "ac_add_options --with-pgo-profile-path=%s" % (out / "merged.profdata"),
            "ac_add_options --with-pgo-jarlog=%s" % (out / "en-US.log"),
            "ac_add_options --enable-lto=%s" % LTO[leg]]


def main(argv) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--leg", required=True, choices=sorted(PLATFORMS))
    sub = ap.add_subparsers(dest="what", required=True)
    sub.add_parser("rust-version")
    p = sub.add_parser("pgo")
    p.add_argument("--out", type=Path, required=True)
    a = ap.parse_args(argv)
    try:
        if a.what == "rust-version":
            print(rust_version(a.leg))
        else:
            print("\n".join(pgo(a.leg, a.out)))
    except UpstreamError as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
