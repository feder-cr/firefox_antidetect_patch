"""What a web page can read of the engine, against the retail Firefox of the
same version and platform.

A page reaches the engine's own files in two ways. It probes whether a name is
registered: `new Worker("chrome://<name>/...")` returns for a registered chrome
package and throws SecurityError at once for an absent one, which is how a
captcha vendor found the juggler (invisible_playwright #336). And it loads, with
<link>, <img> and <script>, every file of a package registered
`contentaccessible` - its content, skin and locale alike, because the flag
belongs to the package (nsChromeRegistry::AllowContentToAccess reads it from the
host) - and of every resource:// host so flagged, and reads what they render.
So the surface is every registration a manifest makes, and the bytes of every
file a page can load. The owner's rule (2026-10-14): "la nostra versione deve
essere uguale identica al firefox stock, non e' accettabile che ci siano
differenze se sono rilevabili". A difference a page cannot observe is declared
in stock_parity_exceptions.json with the measurement that says so, and a
declaration that no longer matches a difference fails the gate as well.

THE JUDGE IS THE RETAIL ITSELF, not a list kept here: the complete MAR Mozilla
publishes for the same version and platform, checked against the release's
SHA256SUMS. It holds the tree stock Firefox installs (its omni.ja are
byte-identical to an installed retail's, measured 2026-10-15). A baseline
captured by hand goes stale at the next rebase, and the Linux one was derived
from the Windows one (B304). The version comes from the tree's application.ini
and the platform from its executable's header, so nothing here names either.

The retail's omni.ja are in the optimized jar layout that Python's zipfile
refuses ("Bad magic number for central directory"); they are read with
Mozilla's own reader, python/mozbuild/mozpack/mozjar.py of this tree.
"""
from __future__ import annotations

import hashlib
import json
import lzma
import os
import struct
import sys
import urllib.request
from pathlib import Path

TREE_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TREE_ROOT / "python" / "mozbuild"))
from mozpack.mozjar import JarReader  # noqa: E402

ARCHIVE = "https://archive.mozilla.org/pub/firefox/releases"
EXCEPTIONS = Path(__file__).resolve().parent / "stock_parity_exceptions.json"
#: Where downloaded retail MARs are kept between runs.
CACHE = Path(os.environ.get("STEALTH_RETAIL_CACHE")
             or Path.home() / ".cache" / "stealth-retail")
#: The jars a Firefox tree packs its chrome into, by the logical prefix of
#: their entries.
JARS = {"omni.ja": "", "browser/omni.ja": "browser/"}
#: The manifests the chrome registry starts from; `manifest` lines lead on.
ROOT_MANIFESTS = ("chrome.manifest", "browser/chrome.manifest")
#: Built-in resource hosts, by the logical prefix they map to.
BUILTIN_HOSTS = {"gre": "", "app": "browser/"}
#: The instructions a page can observe: registered names, and overrides that
#: change what a chrome URL answers.
REGISTERING = ("content", "skin", "locale", "resource", "override")


class ParityError(Exception):
    """The surface could not be read or judged; never a pass."""


# ── which tree ──────────────────────────────────────────────────────────────

def platform_of(tree: Path) -> str:
    """The archive.mozilla.org platform of the tree, from its executable."""
    exe = tree / "firefox.exe"
    if exe.is_file():
        head = exe.read_bytes()[:4096]
        pe = struct.unpack_from("<I", head, 0x3C)[0]
        machine = struct.unpack_from("<H", head, pe + 4)[0]
        names = {0x8664: "win64", 0xAA64: "win64-aarch64", 0x14C: "win32"}
    else:
        exe = tree / "firefox"
        if not exe.is_file():
            raise ParityError(f"no firefox or firefox.exe in {tree}")
        head = exe.read_bytes()[:64]
        if head[:4] != b"\x7fELF":
            raise ParityError(f"{exe} is not an ELF executable")
        machine = struct.unpack_from("<H", head, 18)[0]
        names = {0x3E: "linux-x86_64", 0xB7: "linux-aarch64", 0x03: "linux-i686"}
    if machine not in names:
        raise ParityError(f"{exe}: machine 0x{machine:x} is not a platform Mozilla publishes")
    return names[machine]


def version_of(tree: Path) -> str:
    for line in (tree / "application.ini").read_text("utf-8").splitlines():
        if line.startswith("Version="):
            return line.split("=", 1)[1].strip()
    raise ParityError(f"no Version= in {tree / 'application.ini'}")


# ── the files of a tree, by logical path ────────────────────────────────────

class Files:
    """Every file of a Firefox tree by its logical path: loose files as they
    lie, and the entries of each omni.ja under the prefix of its directory.
    A packaged tree (Windows, the retail) keeps its chrome in the jars; the
    Linux archive we ship keeps it loose. Both answer the same paths."""

    def __init__(self, loaders: dict):
        self._loaders = loaders

    @classmethod
    def from_directory(cls, root: Path) -> "Files":
        loaders = {}
        for p in root.rglob("*"):
            rel = p.relative_to(root).as_posix()
            if p.is_file() and rel not in JARS:
                loaders[rel] = p.read_bytes
        for rel, prefix in JARS.items():
            if (root / rel).is_file():
                cls._add_jar(loaders, JarReader(str(root / rel)), prefix, rel)
        return cls(loaders)

    @classmethod
    def from_entries(cls, entries: dict) -> "Files":
        """From {path: bytes}, the content of a MAR."""
        loaders = {}
        for rel, data in entries.items():
            if rel not in JARS:
                loaders[rel] = (lambda d=data: d)
        for rel, prefix in JARS.items():
            if rel in entries:
                cls._add_jar(loaders, JarReader(data=entries[rel]), prefix, rel)
        return cls(loaders)

    @staticmethod
    def _add_jar(loaders, jar, prefix, where):
        for entry in jar:
            name = entry.filename
            if name.endswith("/"):
                continue
            path = prefix + name
            if path in loaders:
                raise ParityError(f"{path} is both loose and in {where}: which one a page "
                                  f"reads is not decidable here")
            loaders[path] = (lambda j=jar, n=name: j[n].read())

    def has(self, path: str) -> bool:
        return path in self._loaders

    def read(self, path: str) -> bytes:
        return self._loaders[path]()

    def under(self, prefix: str):
        for path in self._loaders:
            if path.startswith(prefix):
                yield path


# ── the surface ─────────────────────────────────────────────────────────────

def _flag(token: str, name: str):
    """ManifestParser's CheckFlag: `name`, `name=1|t...|y...` true,
    `name=` or `name=0|f...|n...` false, anything else not this flag."""
    token = token.lower()
    if not token.startswith(name):
        return None
    if len(token) == len(name):
        return True
    if token[len(name)] != "=":
        return None
    value = token[len(name) + 1:]
    return bool(value) and value[0] in "1ty"


def _instructions(files: Files):
    """(manifest path, tokens) for every instruction the registry reads,
    following `manifest` lines from the root manifests."""
    seen, todo = set(), [m for m in ROOT_MANIFESTS if files.has(m)]
    if not todo:
        raise ParityError("no root chrome.manifest: this is not a Firefox tree")
    while todo:
        path = todo.pop()
        if path in seen:
            continue
        seen.add(path)
        base = path.rsplit("/", 1)[0] + "/" if "/" in path else ""
        for line in files.read(path).decode("utf-8", "replace").splitlines():
            tokens = line.split("#", 1)[0].split()
            if not tokens:
                continue
            if tokens[0] == "manifest":
                todo.append(_normalize(base + tokens[1]))
            else:
                yield path, tokens


def _normalize(path: str) -> str:
    out = []
    for part in path.split("/"):
        if part == "..":
            out.pop()
        elif part not in ("", "."):
            out.append(part)
    return "/".join(out) + ("/" if path.endswith("/") else "")


def surface(files: Files) -> dict:
    """{key: value} for everything a page can observe: one key per
    registration (value: its flags), and one per readable file (value: the
    SHA-256 of its bytes)."""
    regs, targets, accessible_pkgs, accessible_hosts, overrides = {}, {}, set(), set(), []
    for manifest, t in _instructions(files):
        kind = t[0]
        if kind not in REGISTERING:
            continue
        base = manifest.rsplit("/", 1)[0] + "/" if "/" in manifest else ""
        if kind in ("content", "resource"):
            name, target, flags = t[1], t[2], t[3:]
            key = f"{kind} {name}"
        elif kind in ("skin", "locale"):
            name, target, flags = t[1], t[3], t[4:]
            key = f"{kind} {name} {t[2]}"
        else:  # override <from> <to>
            name, target, flags = t[1], t[2], t[3:]
            key = f"override {name}"
            overrides.append((name, target))
        flags_norm = " ".join(sorted(f.lower() for f in flags))
        regs.setdefault(key, set()).add(flags_norm)
        if kind != "override":
            targets.setdefault((kind, name), []).append(
                (t[2] if kind in ("skin", "locale") else None,
                 target if ":" in target else _normalize(base + target)))
        if kind in ("content", "resource") and any(_flag(f, "contentaccessible") for f in flags):
            (accessible_pkgs if kind == "content" else accessible_hosts).add(name)

    def resolve_url(url: str) -> str:
        """A resource:// or chrome:// URL target to a logical path."""
        scheme, rest = url.split("://", 1)
        host, _, sub = rest.partition("/")
        if scheme == "resource":
            if host in BUILTIN_HOSTS and ("resource", host) not in targets:
                return BUILTIN_HOSTS[host] + sub
            return resolve_target(("resource", host)) + sub
        if scheme == "chrome":
            provider, _, sub2 = sub.partition("/")
            return resolve_target((provider, host)) + sub2
        raise ParityError(f"target {url} is not a resource:// or chrome:// URL")

    def resolve_target(reg) -> str:
        found = targets.get(reg)
        if not found or len({t for _, t in found}) != 1:
            raise ParityError(f"{reg[0]} {reg[1]} has {len(found or [])} targets: not decidable")
        target = found[0][1]
        return resolve_url(target) if ":" in target else target

    out = {f"registers {k}": " | ".join(sorted(v)) for k, v in regs.items()}
    for (kind, name), entries in targets.items():
        if not ((kind != "resource" and name in accessible_pkgs)
                or (kind == "resource" and name in accessible_hosts)):
            continue
        for provider, target in entries:
            directory = resolve_url(target) if ":" in target else target
            for path in files.under(directory):
                rel = path[len(directory):]
                url = (f"resource://{name}/{rel}" if kind == "resource"
                       else f"chrome://{name}/{kind}/{rel}" + (f" [{provider}]" if provider else ""))
                digest = hashlib.sha256(files.read(path)).hexdigest()
                if out.setdefault(f"file {url}", digest) != digest:
                    raise ParityError(f"{url} is registered twice with different bytes: "
                                      f"which one a page reads is not decidable here")
    for frm, to in overrides:
        path = resolve_url(to)
        out[f"override target {frm}"] = (hashlib.sha256(files.read(path)).hexdigest()
                                          if files.has(path) else "missing")
    return out


# ── the retail ──────────────────────────────────────────────────────────────

def _download(url: str, dest: Path) -> Path:
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        part = dest.with_suffix(dest.suffix + ".part")
        try:
            with urllib.request.urlopen(url, timeout=120) as r, open(part, "wb") as f:
                while chunk := r.read(1 << 20):
                    f.write(chunk)
        except OSError as exc:
            raise ParityError(f"cannot download the judge {url}: {exc}") from exc
        part.replace(dest)
    return dest


def read_mar(data: bytes) -> dict:
    """{path: bytes} of a Mozilla MAR (modules/libmar): 'MAR1', the offset of
    the index, then entries of (offset, size, flags, NUL-terminated name).
    Complete updates compress each entry with xz."""
    if data[:4] != b"MAR1":
        raise ParityError("not a MAR file")
    index = struct.unpack_from(">I", data, 4)[0]
    size = struct.unpack_from(">I", data, index)[0]
    pos, end, out = index + 4, index + 4 + size, {}
    while pos < end:
        offset, length, _flags = struct.unpack_from(">III", data, pos)
        nul = data.index(b"\0", pos + 12)
        name = data[pos + 12:nul].decode("utf-8")
        pos = nul + 1
        raw = data[offset:offset + length]
        out[name] = lzma.decompress(raw) if raw[:6] == b"\xfd7zXZ\x00" else raw
    return out


def retail_files(version: str, platform: str) -> Files:
    """The tree of the retail Firefox `version` for `platform`, from Mozilla's
    complete MAR, verified against the release's SHA256SUMS."""
    rel = f"update/{platform}/en-US/firefox-{version}.complete.mar"
    sums = _download(f"{ARCHIVE}/{version}/SHA256SUMS", CACHE / version / "SHA256SUMS")
    want = [l.split()[0] for l in sums.read_text("utf-8").splitlines() if l.endswith("  " + rel)]
    if len(want) != 1:
        raise ParityError(f"SHA256SUMS of {version} lists {rel} {len(want)} times")
    mar = _download(f"{ARCHIVE}/{version}/{rel}", CACHE / version / f"{platform}.complete.mar")
    data = mar.read_bytes()
    if hashlib.sha256(data).hexdigest() != want[0]:
        mar.unlink()
        raise ParityError(f"{mar} does not match SHA256SUMS; removed, download again")
    return Files.from_entries(read_mar(data))


# ── the comparison ──────────────────────────────────────────────────────────

def differences(ours: dict, stock: dict) -> dict:
    """{key: description} of every key that differs."""
    out = {}
    for key in sorted(set(ours) | set(stock)):
        if key not in stock:
            out[key] = "only in this build"
        elif key not in ours:
            out[key] = "only in the retail"
        elif ours[key] != stock[key]:
            out[key] = (f"retail [{stock[key]}], this build [{ours[key]}]"
                        if key.startswith("registers ") else "content differs")
    return out


def load_exceptions(platform: str, path: Path = EXCEPTIONS) -> dict:
    """{key: entry} declared for `platform`. Each entry says why a page cannot
    tell the difference and where that was measured."""
    doc = json.loads(path.read_text("utf-8"))
    out = {}
    for entry in doc["exceptions"]:
        missing = {"key", "platforms", "why", "measured"} - set(entry)
        if missing:
            raise ParityError(f"exception without {sorted(missing)}: {entry}")
        if platform in entry["platforms"]:
            out[entry["key"]] = entry
    return out


def judge(tree: Path, retail=retail_files, exceptions: Path = EXCEPTIONS) -> tuple:
    """(platform, version, undeclared differences, stale declarations, size).
    `retail(version, platform)` gives the judge's Files; the selftest passes a
    tree of its own, everything else the MAR."""
    platform, version = platform_of(tree), version_of(tree)
    ours = surface(Files.from_directory(tree))
    stock = surface(retail(version, platform))
    diff = differences(ours, stock)
    declared = load_exceptions(platform, exceptions)
    undeclared = {k: v for k, v in diff.items() if k not in declared}
    stale = sorted(k for k in declared if k not in diff)
    return platform, version, undeclared, stale, len(stock)


def selftest() -> int:
    """The gate's known-bad inputs and the near-misses that must not fire, on
    trees built here: no archive, no browser, no network. The retail is a tree
    too, packed in omni.ja as Mozilla packs it; ours is loose unless a case
    says otherwise, so every case also proves the two layouts compare."""
    import shutil
    import tempfile
    import zipfile

    elf = b"\x7fELF" + bytes(14) + struct.pack("<H", 0x3E) + bytes(44)
    base = {
        "chrome.manifest": "manifest chrome/chrome.manifest\n",
        "chrome/chrome.manifest": (
            "content global toolkit/content/global/ contentaccessible=yes\n"
            "skin global classic/1.0 toolkit/skin/global/\n"
            "locale global en-US en-US/locale/global/\n"
            "content devimg devtools/images/ contentaccessible=true\n"
            "content private private/content/\n"
            "resource content-accessible resource://gre/contentaccessible/ contentaccessible=yes\n"
            "override chrome://global/content/license.html chrome://global/content/real.html\n"),
        "chrome/toolkit/content/global/a.css": "body { color: red }\n",
        "chrome/toolkit/content/global/real.html": "<p>license</p>\n",
        "chrome/toolkit/skin/global/b.css": "x {}\n",
        "chrome/en-US/locale/global/c.properties": "k=v\n",
        "chrome/devtools/images/d.svg": "<svg/>\n",
        "chrome/private/content/e.js": "1\n",
        "contentaccessible/f.css": "y {}\n",
    }

    def write(root, files, omni):
        root.mkdir(parents=True)
        (root / "firefox").write_bytes(elf)
        (root / "application.ini").write_bytes(b"[App]\nVersion=151.0\n")
        if omni:
            with zipfile.ZipFile(root / "omni.ja", "w") as z:
                for rel, text in files.items():
                    z.writestr(rel, text)
        else:
            for rel, text in files.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_bytes(text.encode())

    def mutate(**changes):
        files = dict(base)
        for rel, text in changes.items():
            rel = rel.replace("__", "/").replace("_DOT_", ".")
            if text is None:
                files.pop(rel)
            else:
                files[rel] = text
        return files

    manifest = base["chrome/chrome.manifest"]
    cases = [
        ("the same surface, ours loose and the retail in omni.ja", mutate(), False, None),
        ("a content juggler package (every build until firefox-47)",
         mutate(chrome__chrome_DOT_manifest=manifest + "content juggler juggler/content/\n"), True, None),
        ("a package the retail registers and we do not",
         mutate(chrome__chrome_DOT_manifest=manifest.replace("content private private/content/\n", "")),
         True, None),
        ("the same package without contentaccessible",
         mutate(chrome__chrome_DOT_manifest=manifest.replace(
             "toolkit/content/global/ contentaccessible=yes", "toolkit/content/global/")), True, None),
        ("a readable file whose bytes differ",
         mutate(chrome__toolkit__content__global__a_DOT_css="body { color: blue }\n"), True, None),
        ("a readable file we lack",
         mutate(chrome__toolkit__content__global__a_DOT_css=None), True, None),
        ("a readable file only we have",
         mutate(chrome__toolkit__content__global__z_DOT_js="2\n"), True, None),
        ("the skin of an accessible package differs (the flag is the package's)",
         mutate(chrome__toolkit__skin__global__b_DOT_css="x { y: 1 }\n"), True, None),
        ("contentaccessible=true counts as accessible",
         mutate(chrome__devtools__images__d_DOT_svg="<svg viewBox='0 0 1 1'/>\n"), True, None),
        ("a resource host that points into resource://gre/",
         mutate(contentaccessible__f_DOT_css="z {}\n"), True, None),
        ("the target of an override differs",
         mutate(chrome__toolkit__content__global__real_DOT_html="<p>other</p>\n"), True, None),
        # Near-miss: a page cannot load a file of a package that is not
        # contentaccessible, so its bytes are not on the surface.
        ("a file of a package a page cannot load",
         mutate(chrome__private__content__e_DOT_js="3\n"), False, None),
        ("a difference declared with its measurement",
         mutate(chrome__toolkit__content__global__a_DOT_css="body { color: blue }\n"), False,
         ["file chrome://global/content/a.css"]),
        ("a declaration with nothing left to excuse", mutate(), True,
         ["file chrome://global/content/a.css"]),
    ]
    bad = 0
    work = Path(tempfile.mkdtemp(prefix="stock-parity-selftest-"))
    try:
        write(work / "retail", base, omni=True)
        retail = Files.from_directory(work / "retail")
        for i, (name, ours, must_refuse, declared) in enumerate(cases):
            tree = work / f"ours{i}"
            write(tree, ours, omni=False)
            exc = work / f"exceptions{i}.json"
            exc.write_bytes(json.dumps({"exceptions": [
                {"key": k, "platforms": ["linux-x86_64"], "why": "selftest", "measured": "selftest"}
                for k in declared or []]}).encode())
            try:
                _, _, undeclared, stale, _ = judge(tree, lambda v, p: retail, exc)
                refused = bool(undeclared or stale)
            except ParityError:
                refused = True
            ok = refused == must_refuse
            bad += not ok
            print(f"  [{'ok' if ok else 'GATE WRONG':9}] {name}: "
                  f"{'must refuse' if must_refuse else 'must pass'}, "
                  f"{'refused' if refused else 'passed'}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    fired = sum(1 for c in cases if c[2])
    print(f"[stock_parity selftest] {len(cases)} cases ({fired} known-bad, "
          f"{len(cases) - fired} that must not fire), {bad} wrong")
    return 1 if bad else 0


def main(argv) -> int:
    if argv[1:] == ["--selftest"]:
        return selftest()
    if len(argv) != 2:
        print("usage: stock_parity.py <extracted firefox tree> | --selftest")
        return 2
    try:
        platform, version, undeclared, stale, size = judge(Path(argv[1]))
    except ParityError as exc:
        print(f"NOT JUDGED: {exc}")
        return 3
    print(f"{platform} {version}: {size} keys on the retail's surface, "
          f"{len(undeclared)} undeclared differences, {len(stale)} stale declarations")
    for key, what in undeclared.items():
        print(f"  {what:40s} {key}")
    for key in stale:
        print(f"  {'declared but not different':40s} {key}")
    return 1 if undeclared or stale else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
