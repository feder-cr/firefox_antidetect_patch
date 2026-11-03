#!/usr/bin/env python3
"""When a leg's compile cache is saved, and which old entries go after it.

    cache_rules.py worth --stats sccache-stats.txt
        prints save=true|false for $GITHUB_OUTPUT, and how many objects it counted
    cache_rules.py trim --run R --attempt A --ref refs/heads/main
        after this run's saves: this ref's older entries of the legs it saved
    cache_rules.py guard
        for the step summary: what enters the compile's key and can move by itself

Worth it: the node uploads at about 5 MB/s, so an entry of 4-5 GB costs about 15
minutes of a machine paid by the hour. A run after a small fix recompiled a handful
of objects and the next run loses seconds without them, so it does not save.

Trim: the repository's caches are over their quota (504 of 500 GB) and GitHub evicts
by itself, without knowing which entries matter. Every entry this removes is proved
to belong to this ref and to a leg this run saved: other refs belong to other work
on the same repository and are never touched. It needs actions: write, which is why
it runs in a job on GitHub's runners and never on the node.

Standard library only, from the series' own copy: no source tree needed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import legs  # noqa: E402

# Objects compiled by this build, below which its entry is not worth the upload.
# A leg has about 5,500; change this after the first real runs.
NEW_OBJECTS_WORTH_SAVING = 300
KEEP_NEWEST = 2
# The entries with a run in their key. Each is <prefix><leg>-<run>-<attempt>.
PREFIXES = ("sccache-",)
# The environment that can move a compile's key.
KEY_ENV = re.compile(r"^(CARGO|RUSTUP|SDKROOT|CCC_|SCCACHE_C|CPATH|C_INCLUDE|CPLUS)")


def new_objects(stats: str):
    """What sccache compiled and stored, from --show-stats: every miss is an object
    written, less those it failed to store. None if the statistics say nothing."""
    misses = re.search(r"^Cache misses\s+(\d+)\s*$", stats, re.M)
    if not misses:
        return None
    errors = re.search(r"^Cache write errors\s+(\d+)\s*$", stats, re.M)
    return max(0, int(misses.group(1)) - (int(errors.group(1)) if errors else 0))


def worth_saving(stats: str, threshold: int = NEW_OBJECTS_WORTH_SAVING) -> tuple:
    """(save or not, why). Statistics that say nothing save: losing a build's objects
    costs far more than one needless upload."""
    count = new_objects(stats)
    if count is None:
        return True, "no object count in the statistics, saving"
    if count >= threshold:
        return True, "%d new objects, at least %d: saving" % (count, threshold)
    return False, "%d new objects, below %d: not worth the upload" % (count, threshold)


def to_delete(caches, ref, leg, run, attempt, prefix="sccache-", keep=KEEP_NEWEST) -> list:
    """The ids of the entries to delete for one leg: nothing unless this run saved
    that leg's entry on this ref, and never another ref's or another leg's."""
    start = "%s%s-" % (prefix, leg)
    mine = [c for c in caches if c.get("ref") == ref and c.get("key", "").startswith(start)
            and re.fullmatch(r"\d+-\d+", c["key"][len(start):])]
    if "%s%s-%s" % (start, run, attempt) not in [c["key"] for c in mine]:
        return []
    mine.sort(key=lambda c: c.get("created_at", ""), reverse=True)
    return [c["id"] for c in mine[keep:]]


def sha256_of(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(2**20), b""):
            h.update(chunk)
    return h.hexdigest()


def key_guard(home: Path, cwd: Path, environ: dict, sysroot) -> list:
    """What enters the compile cache's key and can move without anyone deciding it, one
    line each for the step summary: two runs that miss for no reason differ here. For
    Rust sccache hashes every inherited CARGO_* variable (but CARGO_MAKEFLAGS,
    CARGO_BUILD_JOBS, CARGO_ENCODED_RUSTFLAGS and CARGO_REGISTRIES_*), the working
    directory and the compiler's files; for C and C++ the compiler."""
    def named(label, files):
        files = sorted(p for p in files if p.is_file())
        return "%s: %s" % (label, " ".join(sha256_of(p)[:16] for p in files) or "absent")

    env = sorted("%s=%s" % (k, v) for k, v in environ.items() if KEY_ENV.match(k))
    mozbuild = home / ".mozbuild"
    tarballs = list(mozbuild.glob("sysroot*.tar*")) + list(mozbuild.glob("toolchains/*sysroot*.tar*"))
    tree = sorted(str(p.relative_to(mozbuild)) + ":%d" % p.stat().st_size
                  for d in mozbuild.glob("sysroot*") if d.is_dir() for p in d.rglob("*") if p.is_file())
    return ["cwd: %s" % cwd, "HOME: %s" % home,
            "env: %s" % hashlib.sha256("\n".join(env).encode()).hexdigest()[:16],
            named("clang", [mozbuild / "clang" / "bin" / "clang"]),
            named("librustc_driver", list((Path(sysroot) / "lib").glob("librustc_driver-*")) if sysroot else []),
            named("sysroot tarball", tarballs),
            "sysroot tree: %s" % (hashlib.sha256("\n".join(tree).encode()).hexdigest()[:16] if tree else "absent")]


def urllib_transport(method, url, headers):
    req = urllib.request.Request(url, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def trim(repo, token, ref, run, attempt, transport=urllib_transport, out=print) -> list:
    headers = {"Accept": "application/vnd.github+json", "Authorization": "Bearer %s" % token,
               "User-Agent": "firefox-ci", "X-GitHub-Api-Version": "2022-11-28"}
    base = "https://api.github.com/repos/%s/actions/caches" % repo
    deleted = []
    for leg in legs.LEGS:
        for prefix in PREFIXES:
            caches, page = [], 1
            while True:
                query = urllib.parse.urlencode({"ref": ref, "key": "%s%s-" % (prefix, leg.leg), "per_page": 100, "page": page})
                status, raw = transport("GET", "%s?%s" % (base, query), headers)
                if status != 200:
                    raise RuntimeError("listing the caches: HTTP %d" % status)
                found = json.loads(raw).get("actions_caches", [])
                caches += found
                if len(found) < 100:
                    break
                page += 1
            for cache_id in to_delete(caches, ref, leg.leg, run, attempt, prefix):
                status, _ = transport("DELETE", "%s/%d" % (base, cache_id), headers)
                out("%s %d of %s on %s" % ("deleted" if status in (200, 204) else "could not delete (HTTP %d)" % status,
                                           cache_id, leg.leg, ref))
                if status in (200, 204):
                    deleted.append(cache_id)
    return deleted


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="the compile cache's save and trim rules")
    sub = ap.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("worth")
    w.add_argument("--stats", type=Path, required=True)
    t = sub.add_parser("trim")
    t.add_argument("--run", required=True)
    t.add_argument("--attempt", required=True)
    t.add_argument("--ref", required=True)
    sub.add_parser("guard")
    a = ap.parse_args(argv)
    if a.cmd == "guard":
        try:
            sysroot = subprocess.run(["rustc", "--print", "sysroot"], capture_output=True, text=True).stdout.strip()
        except OSError:
            sysroot = ""
        print("\n".join(["```"] + key_guard(Path.home(), Path.cwd(), dict(os.environ), sysroot) + ["```"]))
        return 0
    if a.cmd == "worth":
        text = a.stats.read_text(encoding="utf-8", errors="replace") if a.stats.is_file() else ""
        save, why = worth_saving(text)
        print("compile cache: %s" % why, file=sys.stderr)
        print("save=%s" % str(save).lower())
        return 0
    repo, token = os.environ.get("GITHUB_REPOSITORY"), os.environ.get("GH_TOKEN")
    if not repo or not token:
        print("ERROR: GITHUB_REPOSITORY and GH_TOKEN are required", file=sys.stderr)
        return 1
    try:
        print("trimmed %d entries" % len(trim(repo, token, a.ref, a.run, a.attempt)))
    except (RuntimeError, OSError) as exc:
        # Only housekeeping, GitHub evicts anyway: it never fails a run.
        print("::warning::the old cache entries stay: %s" % exc, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
