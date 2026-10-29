"""A tag's payload never changes: refuse a seal that differs from the published one.

    python3 scripts/ci payload-guard --tag firefox-N --repo OWNER/REPO --seal dl/seal.json

  no release under the tag          -> first publish, proceed
  a release without a seal.json     -> first seal, proceed
  a release whose seal is identical -> metadata-only re-cut, proceed
  a release whose seal differs      -> REFUSED: a new payload gets a new tag
  anything else (network, auth, 5xx)-> REFUSED: the answer is unknown

The shell this replaces ran `gh release download ... 2>/dev/null` and read
every failure as "no seal published yet", so a transient GitHub error let a
different payload go out under a tag that already had one.
"""
from __future__ import annotations

import json
import subprocess
import tempfile
from pathlib import Path


class GuardError(Exception):
    pass


def _gh(args, run=subprocess.run):
    p = run(["gh"] + args, capture_output=True, text=True)
    return p.returncode, p.stdout, p.stderr


def check(tag: str, repo: str, seal: Path, gh=_gh) -> str:
    if not seal.is_file() or seal.stat().st_size == 0:
        raise GuardError("%s is missing or empty: nothing to compare the published seal against" % seal)
    code, out, err = gh(["release", "view", tag, "-R", repo, "--json", "assets"])
    if code != 0:
        if "release not found" in (err + out).lower():
            return "no release under %s yet: first publish" % tag
        raise GuardError("could not read release %s (exit %d): %s" % (tag, code, (err or out).strip()))
    names = [a.get("name") for a in json.loads(out or "{}").get("assets", [])]
    if "seal.json" not in names:
        return "%s has no seal yet: first seal" % tag
    with tempfile.TemporaryDirectory() as tmp:
        code, out, err = gh(["release", "download", tag, "-R", repo, "--pattern", "seal.json", "--dir", tmp])
        if code != 0:
            raise GuardError("%s has a seal.json that could not be downloaded (exit %d): %s"
                             % (tag, code, (err or out).strip()))
        published = (Path(tmp) / "seal.json").read_bytes()
    if published != seal.read_bytes():
        raise GuardError("%s already has a published seal for a DIFFERENT payload. A new payload "
                         "gets a new tag: mint firefox-(N+1) from the same artifacts." % tag)
    return "seal identical: metadata-only re-cut"
