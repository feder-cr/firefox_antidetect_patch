#!/usr/bin/env python3
"""Generate the release seal from the archives a release is about to publish.

The seal is the ONE machine-generated file that pairs an invisible_core with one
engine build. Everything downstream is derived from it: the archive names, the
cache directory name, the spoofed User-Agent, the pip-visible core version, and
the launch-time engine check.

Two modes, one code path:

  --assets DIR      read every firefox-*-stealth-* archive in DIR. Used by
                    release.yml, and by hand over a directory of downloaded
                    assets when CI is unavailable.
  --from-tree DIR   emit a LOCAL seal for an unpacked build (dev, CI gates).
                    Its assets map is empty, so it can verify a binary but can
                    never be used to download one.

Do not hand-write a seal. This script IS the manual path.

Two facts about our own published archives, measured on the five firefox-18
assets on 2026-07-25, are baked into the schema:

  * The five legs are five independent CI builds, so they carry five different
    application.ini BuildIDs (151.0 / 20260724001621, 20260724001829,
    20260724001606, 20260724001555, 20260724001949). They agree on Version, and
    that agreement is the cross-leg check. The BuildID goes in the per-asset
    record, because a client verifies the leg it actually runs; one seal-wide
    BuildID matched exactly one platform and refused the other four.
  * The Linux archives carry NO omni.ja: linux_release.sh tars the pre-package
    dist/bin layout, so chrome/juggler/ is loose in the tree. Provenance is read
    from whichever layout the archive has, and omni_sha256 is left empty for a
    leg that has nothing to hash.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import sys
import tarfile
import zipfile
from pathlib import Path

# 1 -> 2 (2026-07-25): BuildID moved into the per-asset record. Bumped because
# a schema-1 reader would take the top-level BuildID as the authority, which is
# the bug; invisible_core refuses a schema it does not know rather than guess.
SCHEMA = 2
JUGGLER_ENTRIES = (
    "chrome/juggler/content/protocol/PageHandler.js",
    "chrome/juggler/content/TargetRegistry.js",
    "chrome/juggler/content/content/main.js",
    "chrome/juggler/content/content/Runtime.js",
)
JUGGLER_DIR_REL = "chrome/juggler"
JUGGLER_MARKERS = (b"stealthfox", b"zoom.stealth")
ENTRY_REL = {
    "win32": "firefox.exe",
    "linux": "firefox",
    "darwin": "Firefox.app/Contents/MacOS/firefox",
}
# (platform, arch) -> the archive-name infix release.yml produces
EXPECTED = {
    ("linux", "x86_64"): "-linux-x86_64.tar.gz",
    ("linux", "arm64"): "-linux-arm64.tar.gz",
    ("win32", "x86_64"): "-win-x86_64.zip",
    ("darwin", "arm64"): "-macos-arm64.tar.gz",
    ("darwin", "x86_64"): "-macos-x86_64.tar.gz",
}
NAME_RE = re.compile(r"^firefox-(?P<ver>[0-9.]+)-stealth-(?P<os>win|linux|macos)-(?P<arch>x86_64|arm64)\.(zip|tar\.gz)$")


class SealBuildError(RuntimeError):
    pass


def sha256_path(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _norm(name: str) -> str:
    """Archive member name as a tree-relative path.

    tar members from `tar -czf ... .` are written "./firefox", "./chrome/...";
    zip members are written bare. Normalising here means every downstream match
    (application.ini, omni.ja, the juggler entries) works on both.
    """
    n = name.replace("\\", "/")
    while n.startswith("./"):
        n = n[2:]
    return n


def _members(archive: Path):
    """Yield (name, reader) for every regular file in the archive."""
    if archive.suffix == ".zip":
        zf = zipfile.ZipFile(archive)
        for info in zf.infolist():
            if not info.is_dir():
                yield _norm(info.filename), (lambda n=info.filename: zf.read(n))
    else:
        tf = tarfile.open(archive, "r:gz")
        for info in tf.getmembers():
            if info.isfile():
                yield _norm(info.name), (lambda i=info: tf.extractfile(i).read())


def _shallowest(names: list[str], leaf: str) -> str | None:
    cands = [n for n in names if n.rstrip("/").split("/")[-1] == leaf]
    if not cands:
        return None
    return sorted(cands, key=lambda n: (n.count("/"), len(n)))[0]


def _parse_ini(text: str) -> dict[str, dict[str, str]]:
    out: dict[str, dict[str, str]] = {}
    sect = ""
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line[0] in ";#":
            continue
        if line.startswith("[") and line.endswith("]"):
            sect = line[1:-1].strip()
            out.setdefault(sect, {})
            continue
        if "=" in line:
            k, _, v = line.partition("=")
            out.setdefault(sect, {})[k.strip()] = v.strip()
    return out


def read_archive(archive: Path) -> dict:
    m = NAME_RE.match(archive.name)
    if not m:
        raise SealBuildError(f"unexpected asset name: {archive.name}")
    plat = {"win": "win32", "linux": "linux", "macos": "darwin"}[m.group("os")]
    index = {name: reader for name, reader in _members(archive)}
    names = list(index)
    ini_name = _shallowest(names, "application.ini")
    if ini_name is None:
        raise SealBuildError(f"{archive.name}: no application.ini inside (broken package)")
    ini = _parse_ini(index[ini_name]().decode("utf-8", "replace"))
    version = ini.get("App", {}).get("Version")
    build_id = ini.get("App", {}).get("BuildID")
    if not version or not build_id:
        raise SealBuildError(f"{archive.name}: application.ini has no App/Version or App/BuildID")
    omni_name = _shallowest(names, "omni.ja")
    omni_sha = ""
    if omni_name is not None:
        layout = "omni.ja"
        blob = index[omni_name]()
        omni_sha = hashlib.sha256(blob).hexdigest()
        with zipfile.ZipFile(io.BytesIO(blob)) as zf:
            inner = set(zf.namelist())
            marked = sum(1 for e in JUGGLER_ENTRIES
                         if e in inner and any(mk in zf.read(e) for mk in JUGGLER_MARKERS))
            juggler_files = sum(1 for n in inner if n.startswith(JUGGLER_DIR_REL + "/"))
    else:
        # The Linux archives ship the juggler unpacked and carry no omni.ja at
        # all. Same markers, same four entries, read straight out of the archive.
        layout = "loose"
        juggler_files = sum(1 for n in names if n.startswith(JUGGLER_DIR_REL + "/"))
        marked = sum(1 for e in JUGGLER_ENTRIES
                     if e in index and any(mk in index[e]() for mk in JUGGLER_MARKERS))

    if not juggler_files:
        where = ("inside its omni.ja" if layout == "omni.ja"
                 else "in the archive, and no omni.ja either")
        raise SealBuildError(
            f"{archive.name}: no {JUGGLER_DIR_REL}/ {where}. Playwright cannot drive this "
            f"build, and invisible_core would refuse every launch of it.")
    # ⛔ La sbarra e' 4/4, la stessa di validate_release.py, e fino al 2026-08-17
    # qui era 0/4. Non erano "tre soglie per lo stesso segnale": le tre
    # rispondono a domande diverse e due sono legittime - questa chiede "e' una
    # delle NOSTRE build?", validate_release chiede "e' INTEGRA?", seal.py a
    # runtime chiede "posso LANCIARLA?" e tollera 2/4 apposta, perche' li' una
    # rinomina upstream non deve diventare un rifiuto di avvio su ogni macchina.
    #
    # L'incoerenza era una sola e bastava: a 1/4 il produttore coniava il
    # sigillo per un binario che l'esecuzione (soglia 2) avrebbe rifiutato
    # OVUNQUE. Il produttore non deve poter sigillare cio' che nessuno lancia.
    #
    # E per macOS questo e' l'UNICO cancello che esiste: nessun percorso di
    # validate_release.py copre quelle gambe, quindi la sbarra lasca non
    # arrivava mai a un secondo controllo. Prima di alzarla la premessa e' stata
    # verificata invece che assunta - scaricate e aperte le due gambe mac di
    # firefox-19: layout omni.ja sotto Firefox.app/Contents/Resources/, 21 file
    # juggler, 4/4 marcate su ENTRAMBE. Alzarla non rompe mac.
    #
    # La sbarra si scrive `len(JUGGLER_ENTRIES)` e non un numero, come fa
    # validate_release.py: quella tupla e' gia' confrontata fra le tre copie da
    # invisible_core/tests/test_juggler_contract.py, quindi non nasce un secondo
    # posto che sappia quanto vale.
    if marked < len(JUGGLER_ENTRIES):
        perche = (
            "A stock build scores 0: this leg is not one of our patched builds"
            if not marked else
            "This leg IS one of ours but has lost markers - an eroding build")
        raise SealBuildError(
            f"{archive.name}: {marked}/{len(JUGGLER_ENTRIES)} juggler entries carry a stealth "
            f"marker ({b'/'.join(JUGGLER_MARKERS).decode()}) in the {layout} layout. {perche}, "
            f"and sealing it would hand the launch-time provenance guard a build it refuses "
            f"on every machine.")

    return {
        "name": archive.name,
        "platform": plat,
        "arch": m.group("arch"),
        "declared_version": m.group("ver"),
        "sha256": sha256_path(archive),
        "size": archive.stat().st_size,
        "entry_rel": ENTRY_REL[plat],
        # Empty for a leg with no omni.ja. Nothing may read that emptiness as a
        # verified payload: it means "there is nothing to hash here".
        "omni_sha256": omni_sha,
        "build_id": build_id,
        "_version": version,
        "_layout": layout,
        "_marked": marked,
    }


REPO_ROOT = Path(__file__).resolve().parent.parent
PW_RANGE_REL = "browser/config/stealth_playwright_range.txt"
PW_VERSION_RE = re.compile(r"\d+\.\d+(\.\d+)?")


def read_playwright_range(path: Path | None) -> tuple[str, str]:
    """The supported Playwright window, read from the tree. ONE source.

    This used to be a pair of literals in main() with the file as an OPTIONAL
    override, applied only `if a.playwright_range and a.playwright_range.exists()`.
    Two copies of two numbers, and the fallback was silent: omit the flag, or
    misspell the path, and the seal still carried a window - the one frozen in
    this file, which nothing keeps equal to the file the humans edit. The seal is
    what the wrapper checks its own Playwright version against, so a stale window
    is a refusal (or an acceptance) on every user's machine, from a release whose
    log said nothing.

    Omitting the flag now means "the copy of the file next to this script", not
    "some numbers I remember" - still one source - and anything unreadable is a
    failed seal rather than a quiet default.
    """
    if path is None:
        path = REPO_ROOT / PW_RANGE_REL
    if not path.is_file():
        raise SealBuildError(
            f"--playwright-range {path} does not exist. Expected the two-line "
            f"{PW_RANGE_REL} (min on line 1, max on line 2).")
    lines = [ln.strip() for ln in path.read_text(encoding="utf-8").splitlines()
             if ln.strip() and not ln.strip().startswith("#")]
    if len(lines) != 2:
        raise SealBuildError(
            f"{path}: expected exactly 2 non-comment lines - the min Playwright version "
            f"then the max - got {len(lines)}: {lines}")
    lo, hi = lines
    for v in (lo, hi):
        if not PW_VERSION_RE.fullmatch(v):
            raise SealBuildError(f"{path}: {v!r} is not a Playwright version like 1.55.0")
    key = lambda v: tuple(int(p) for p in v.split("."))  # noqa: E731
    if key(lo) > key(hi):
        raise SealBuildError(
            f"{path}: min {lo} is above max {hi}, so the supported window is empty and "
            f"the wrapper would refuse every Playwright there is.")
    return lo, hi


def build_from_assets(tag: str, assets_dir: Path, source_commit: str,
                      pw_min: str, pw_max: str, allow_partial: bool) -> tuple[dict, list]:
    archives = sorted(p for p in assets_dir.iterdir()
                      if NAME_RE.match(p.name))
    if not archives:
        raise SealBuildError(f"no firefox-*-stealth-* archives in {assets_dir}")
    rows = [read_archive(p) for p in archives]

    # The cross-leg check is on Version, and ONLY on Version. The five legs are
    # five independent CI runs of one commit, so five different BuildIDs is the
    # normal, correct outcome (measured on firefox-18: five distinct values
    # inside one minute). Requiring one BuildID here would fail every real
    # release; requiring one Version catches the thing that actually goes wrong,
    # a leg built from a different tree.
    versions = {r["_version"] for r in rows}
    if len(versions) != 1:
        detail = "\n".join(f"  {r['name']}: {r['_version']} build {r['build_id']}" for r in rows)
        raise SealBuildError(
            "these archives are not one base version - a leg built from a different tree:\n"
            + detail)
    version = versions.pop()

    for r in rows:
        if version not in r["name"]:
            raise SealBuildError(
                f"asset/base mismatch: {r['name']} does not carry the version its own "
                f"application.ini reports ({version}). Every user download 404s or "
                f"misresolves when these disagree.")

    have = {(r["platform"], r["arch"]) for r in rows}
    missing = set(EXPECTED) - have
    if missing and not allow_partial:
        raise SealBuildError(f"incomplete release, missing platforms: {sorted(missing)}")

    if not re.fullmatch(r"firefox-\d+", tag):
        raise SealBuildError(f"tag must look like firefox-N, got {tag!r}")

    # No top-level build_id: there is no single value that is true of all five
    # legs, and a scalar that is true for one platform is a refusal for the other
    # four. (Nothing is lost to older readers either - a schema-1 reader rejects
    # a schema-2 seal outright, so a compatibility field would never be read.)
    return ({
        "comment": "Generated by scripts/make_seal.py. Do not edit by hand.",
        "schema": SCHEMA,
        "tag": tag,
        "upstream_version": version,
        "source_commit": source_commit,
        "playwright": {"min": pw_min, "max": pw_max},
        "assets": {
            r["name"]: {
                "platform": r["platform"], "arch": r["arch"],
                "build_id": r["build_id"],
                "sha256": r["sha256"], "size": r["size"],
                "entry_rel": r["entry_rel"], "omni_sha256": r["omni_sha256"],
            } for r in rows
        },
    }, rows)


def build_from_tree(tag: str, tree: Path, pw_min: str, pw_max: str) -> dict:
    root = tree
    if (tree / "Firefox.app" / "Contents" / "Resources" / "application.ini").exists():
        root = tree / "Firefox.app" / "Contents" / "Resources"
    ini_p = root / "application.ini"
    if not ini_p.exists():
        raise SealBuildError(f"no application.ini under {tree}")
    ini = _parse_ini(ini_p.read_text(encoding="utf-8", errors="replace"))
    return {
        "comment": "LOCAL seal generated from an unpacked tree. Not a release.",
        "schema": SCHEMA,
        "tag": tag,
        "upstream_version": ini["App"]["Version"],
        "build_id": ini["App"]["BuildID"],
        "source_commit": "",
        "playwright": {"min": pw_min, "max": pw_max},
        "assets": {},
    }


def dump_canonical(seal: dict, out: Path, rows: list | None = None) -> None:
    blob = json.dumps(seal, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8") + b"\n"
    json.loads(blob.decode("utf-8"))  # prove it round-trips before it ships
    out.write_bytes(blob)
    print(f"seal: {out} ({len(blob)} bytes)")
    print(f"  tag={seal['tag']} firefox={seal['upstream_version']}")
    if seal.get("build_id"):
        print(f"  build={seal['build_id']} (single tree, no assets)")
    for r in rows or []:
        print(f"  {r['name']:<44} build {r['build_id']}  juggler "
              f"{r['_marked']}/{len(JUGGLER_ENTRIES)} ({r['_layout']})"
              + ("" if r["omni_sha256"] else "  omni.ja: none, nothing to hash"))
    print(f"  digest={hashlib.sha256(blob).hexdigest()}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="generate the release seal")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--assets", type=Path)
    ap.add_argument("--from-tree", type=Path)
    ap.add_argument("--source-commit-file", type=Path)
    ap.add_argument("--playwright-range", type=Path,
                    help=f"the two-line supported-Playwright window "
                         f"(default: {PW_RANGE_REL} in this tree). No built-in default "
                         f"pair exists - unreadable is a hard error, never a fallback.")
    ap.add_argument("--allow-partial", action="store_true")
    ap.add_argument("-o", "--out", type=Path, required=True)
    a = ap.parse_args(argv)

    commit = ""
    if a.source_commit_file and a.source_commit_file.exists():
        commit = a.source_commit_file.read_text().strip()

    rows: list = []
    try:
        pw_range = a.playwright_range or (REPO_ROOT / PW_RANGE_REL)
        pw_min, pw_max = read_playwright_range(pw_range)
        print(f"playwright window: {pw_min} .. {pw_max}  (from {pw_range})")
        if a.from_tree:
            seal = build_from_tree(a.tag, a.from_tree, pw_min, pw_max)
        elif a.assets:
            seal, rows = build_from_assets(a.tag, a.assets, commit, pw_min, pw_max,
                                           a.allow_partial)
        else:
            ap.error("one of --assets or --from-tree is required")
    except SealBuildError as e:
        print(f"SEAL ERROR: {e}", file=sys.stderr)
        return 1
    dump_canonical(seal, a.out, rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
