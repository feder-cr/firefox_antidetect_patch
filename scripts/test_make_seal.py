#!/usr/bin/env python3
"""make_seal.py, driven against the inputs it exists to refuse.

WHY THIS FILE WAS WRITTEN ON 2026-07-28. `18-gate-inventory.md` D1 quoted
`C:\\ff\\source\\tests\\test_make_seal.py` as this gate's known-bad input and
described the three cases it runs. **That file did not exist, and neither did
the directory.** By the inventory's own rule - "where that column says NONE, the
gate is unproven and should be treated as documentation, not as protection" - a
named-but-ABSENT known-bad input is worse than an empty column, because it reads
as proven.

`make_seal.py` writes the one file that pairs an invisible_core with one engine
build. Everything downstream is derived from it: the archive names, the cache
directory, the spoofed User-Agent, the pip-visible core version, the launch-time
engine check. It also refuses the issue-#14 failure mode - an asset whose
basename does not carry the version its own `application.ini` reports - which
broke 265 downloads. None of that had ever been run against a bad input.

WHAT IS COVERED, and each case is a real refusal rather than a plausible one:

  1. two legs disagreeing on `Version` - a leg built from a different tree;
  2. a basename that does not carry its own reported version (issue #14);
  3. an archive with no `chrome/juggler/` at all - Playwright cannot drive it;
  4. an archive whose juggler carries no stealth marker - a stock build;
  5. an incomplete release, missing platforms;
  6. a tag that is not `firefox-N`;
  7. an archive with no `application.ini`;
  8. and the POSITIVE case: a clean five-way set produces a seal whose fields
     are the ones every downstream consumer reads.

WHAT IS NOT COVERED, deliberately. The Playwright-range file has its own
refusals (missing, wrong line count, non-version token, min above max) and they
are checked here only to the extent of not writing a seal; the exhaustive set
lives with the range file. And nothing here touches a real archive: the point is
that these run in seconds with no build.

Run it standalone - `python scripts/test_make_seal.py` - because this repository
has no pytest configuration and a gate that needs one would not run.


RUN IT DIRECTLY: `python scripts/test_make_seal.py`. This is NOT a pytest suite - it
carries its own case registry and passes each case a temp directory, so
`pytest` collects it by name, finds no fixture called `d`, and reports every
case as an ERROR. That reads exactly like a broken gate, and it cost one
wrong conclusion on 2026-08-02 before the file was run the way it is built.
"""
from __future__ import annotations

import io
import json
import sys
import tarfile
import tempfile
import traceback
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import make_seal as m  # noqa: E402


# --------------------------------------------------------------- fixtures

def _app_ini(version: str, build_id: str) -> bytes:
    return (f"[App]\nVendor=Mozilla\nName=Firefox\nVersion={version}\n"
            f"BuildID={build_id}\n").encode()


def _juggler_files(marked: bool) -> dict:
    """The four entries make_seal counts, with or without a stealth marker.

    `marked=False` is a STOCK build: the files are there, so the "no juggler"
    check passes and the marker check is the one that has to fire. Two different
    refusals, and a fixture that conflated them would prove neither.
    """
    body = b"// juggler\nconst zoom = 1;\n" if not marked else \
           b"// juggler\nprefs.getBoolPref('zoom.stealth.humanize');\n"
    return {e: body for e in m.JUGGLER_ENTRIES}


def _archive(path: Path, *, version: str, build_id: str, marked: bool = True,
             juggler: bool = True, app_ini: bool = True) -> Path:
    """One leg, laid out the way a real asset is: members are TREE-RELATIVE at
    the archive root, not under a `firefox/` wrapper. `_norm` strips a leading
    `./` and nothing else, so a wrapper directory hides every file from every
    check - the first draft of this fixture failed eight cases for that reason
    alone, which is worth keeping written down.

    `.zip` gets an omni.ja; `.tar.gz` ships the juggler loose, which is what
    linux_release.sh actually produces.
    """
    entries: dict = {}
    if app_ini:
        entries["application.ini"] = _app_ini(version, build_id)
    entries["firefox"] = b"\x7fELF fake"

    if path.suffix == ".zip":
        omni = io.BytesIO()
        with zipfile.ZipFile(omni, "w") as z:
            if juggler:
                for name, body in _juggler_files(marked).items():
                    z.writestr(name, body)
        entries["omni.ja"] = omni.getvalue()
        with zipfile.ZipFile(path, "w") as z:
            for name, body in entries.items():
                z.writestr(name, body)
    else:
        if juggler:
            for name, body in _juggler_files(marked).items():
                entries[name] = body
        with tarfile.open(path, "w:gz") as t:
            for name, body in entries.items():
                info = tarfile.TarInfo(name)
                info.size = len(body)
                t.addfile(info, io.BytesIO(body))
    return path


_LEGS = {
    "win-x86_64": ".zip",
    "linux-x86_64": ".tar.gz",
    "linux-arm64": ".tar.gz",
    "macos-x86_64": ".tar.gz",
    "macos-arm64": ".tar.gz",
}


def _release(d: Path, version: str = "151.0", **overrides) -> Path:
    """A clean five-way set, unless an override breaks one leg on purpose."""
    for i, (leg, ext) in enumerate(_LEGS.items()):
        v = overrides.get(f"{leg}_version", version)
        name = overrides.get(f"{leg}_name", f"firefox-{v}-stealth-{leg}{ext}")
        _archive(d / name, version=v, build_id=f"2026072400{1000 + i}",
                 marked=overrides.get(f"{leg}_marked", True),
                 juggler=overrides.get(f"{leg}_juggler", True),
                 app_ini=overrides.get(f"{leg}_app_ini", True))
    return d


def _range_file(d: Path, text: str = "1.55.0\n1.61.0\n") -> Path:
    p = d / "stealth_playwright_range.txt"
    p.write_text(text, encoding="utf-8")
    return p


def _build(d: Path, tag: str = "firefox-18", allow_partial: bool = False):
    lo, hi = m.read_playwright_range(_range_file(d))
    return m.build_from_assets(tag, d, "deadbeef", lo, hi, allow_partial)


# ------------------------------------------------------------------ cases

CASES = []


def case(fn):
    CASES.append(fn)
    return fn


@case
def test_a_clean_five_way_set_produces_a_usable_seal(d: Path):
    """The positive case, and it is not decoration: every refusal below is only
    meaningful if the happy path actually builds something downstream can read."""
    seal, _ = _build(_release(d))
    assert seal["schema"] == m.SCHEMA, seal["schema"]
    assert seal["tag"] == "firefox-18"
    assert seal["upstream_version"] == "151.0"
    assert len(seal["assets"]) == 5, sorted(seal["assets"])
    for name, rec in seal["assets"].items():
        assert "151.0" in name, name
        assert len(rec["sha256"]) == 64, rec["sha256"]
        assert rec["size"] > 0
        assert rec["build_id"], name
    # Five independent CI runs give five BuildIDs. A seal-wide one matched
    # exactly one platform and refused the other four.
    assert len({r["build_id"] for r in seal["assets"].values()}) == 5
    assert seal["playwright"] == {"min": "1.55.0", "max": "1.61.0"}
    json.dumps(seal)          # it has to survive being written


@case
def test_two_legs_from_different_trees_are_refused(d: Path):
    """The cross-leg check. Five legs are five CI runs of ONE commit; a leg
    reporting another Version was built from another tree."""
    _release(d, **{"linux-arm64_version": "150.0.1",
                   "linux-arm64_name": "firefox-150.0.1-stealth-linux-arm64.tar.gz"})
    _expect(d, "not one base version")


@case
def test_a_basename_that_lies_about_its_own_version_is_refused(d: Path):
    """Issue #14, from inside the bytes rather than from a naming convention.
    Every user download 404s or misresolves when these disagree - it broke 265
    of them."""
    _release(d, **{"win-x86_64_name": "firefox-151.9-stealth-win-x86_64.zip"})
    _expect(d, "asset/base mismatch")


@case
def test_an_archive_with_no_juggler_is_refused(d: Path):
    """Playwright cannot drive it and invisible_core refuses every launch of it,
    so a seal naming it would pair the core to an engine nobody can use."""
    _release(d, **{"linux-x86_64_juggler": False})
    _expect(d, "no chrome/juggler")


@case
def test_a_stock_build_with_no_stealth_marker_is_refused(d: Path):
    """The files are present and carry no marker: this is upstream Firefox, not
    one of our patched builds. Distinct from the case above, and a fixture that
    conflated the two would prove neither."""
    _release(d, **{"win-x86_64_marked": False})
    _expect(d, "marker")


@case
def test_an_archive_with_no_application_ini_is_refused(d: Path):
    """A broken package. There is no version to cross-check and no version to
    compare the basename against, so everything downstream would be guessed."""
    _release(d, **{"macos-arm64_app_ini": False})
    _expect(d, "no application.ini")


@case
def test_an_incomplete_release_is_refused(d: Path):
    """Four legs is not a release: the missing platform's users get a 404 from a
    seal that looks complete."""
    _release(d)
    (d / "firefox-151.0-stealth-macos-arm64.tar.gz").unlink()
    _expect(d, "incomplete release")


@case
def test_allow_partial_is_the_only_way_past_an_incomplete_release(d: Path):
    """The escape hatch exists for a re-cut of a single leg. It must be
    explicit - and it must still build, or it is not an escape hatch."""
    _release(d)
    (d / "firefox-151.0-stealth-macos-arm64.tar.gz").unlink()
    seal, _ = _build(d, allow_partial=True)
    assert len(seal["assets"]) == 4


@case
def test_a_tag_that_is_not_firefox_N_is_refused(d: Path):
    """The tag becomes the cache directory name and the major of the core's
    version. `v18` or `firefox-18.1` would derive a version nobody chose."""
    _release(d)
    _expect(d, "tag must look like", tag="release-18")


@case
def test_an_empty_directory_is_refused_rather_than_producing_an_empty_seal(d: Path):
    """The shape a mis-pointed --assets produces. An empty seal is a seal that
    pairs the core to nothing and passes every downstream check."""
    _range_file(d)
    _expect(d, "no firefox-")


@case
def test_a_missing_playwright_range_file_writes_no_seal(d: Path):
    """The range reaches every user's import-time `assert_playwright_range`.
    Falling back to a literal here is what shipped an unvalidated window once."""
    _release(d)
    try:
        m.read_playwright_range(d / "does-not-exist.txt")
    except m.SealBuildError as exc:
        assert "does-not-exist" in str(exc) or "range" in str(exc).lower(), exc
        return
    raise AssertionError("a missing range file produced a range")


# ----------------------------------------------------------------- runner

def _expect(d: Path, needle: str, *, tag: str = "firefox-18") -> None:
    try:
        _build(d, tag=tag)
    except m.SealBuildError as exc:
        assert needle.lower() in str(exc).lower(), (
            f"refused, but not for the reason under test.\n"
            f"  wanted : {needle!r}\n  got    : {exc}")
        return
    raise AssertionError(f"NO REFUSAL: expected {needle!r}, a seal was built")


def main() -> int:
    failed = []
    for fn in CASES:
        with tempfile.TemporaryDirectory() as td:
            try:
                fn(Path(td))
                print(f"PASS  {fn.__name__}")
            except Exception:
                failed.append(fn.__name__)
                print(f"FAIL  {fn.__name__}")
                traceback.print_exc()
    print(f"\n{len(CASES) - len(failed)}/{len(CASES)} passed")
    if failed:
        print("failed: " + ", ".join(failed), file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
