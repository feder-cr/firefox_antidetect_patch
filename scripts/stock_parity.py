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

WHAT IT DOES NOT SEE, so that nobody reads more into its pass. It reads the
manifests; registrations made by JavaScript at run time (the newtab resource
host in AboutNewTabResourceMapping.sys.mjs, formautofill, extensions) are on a
page's surface and not on this one, though the bytes of the built-in add-ons
are, through resource://builtin-addons. Lines whose flags compare against the
host or the build's version (osversion, abi, appversion, platformversion) are
refused as undecidable rather than guessed; 151 has none. And it is the
surface of the engine's own files only: fonts, built-in stylesheets and every
other fingerprint are other gates' business.
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
#
# The chrome registry as Gecko builds it, because an approximation of it lets
# a difference through. xpcom/components/ManifestParser.cpp reads each manifest
# line by line and follows a `manifest` line where it stands, so the order is
# depth-first and file order; chrome/nsChromeRegistryChrome.cpp and
# SubstitutingProtocolHandler keep the LAST registration of a package, of a
# skin or locale provider, of a resource host and of an override. Reading in
# another order, or keeping every registration, passed a build whose later
# line moved a package to a directory with fewer files (independent review,
# 2026-10-15).

#: argc of every directive ManifestParser knows (kParsingTable); any other
#: first token is ignored, as Gecko ignores it.
DIRECTIVES = {"manifest": 1, "component": 2, "contract": 2, "category": 3,
              "content": 2, "locale": 3, "skin": 3, "override": 2, "resource": 2}
#: The directives whose lines take contentaccessible/remoteenabled/remoterequired
#: (`contentflags` in kParsingTable); on any other they are unknown modifiers.
CONTENT_FLAGS = ("content", "resource")
#: ManifestParser's os target for each platform (nsIXULRuntime.OS, lowercased).
OS_TARGET = {"win64": "winnt", "win64-aarch64": "winnt", "win32": "winnt",
             "linux-x86_64": "linux", "linux-aarch64": "linux", "linux-i686": "linux"}


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


def _string_flag(token: str, name: str, value: str, state):
    """ManifestParser's CheckStringFlag: `name=v` or `name!=v`. Returns the
    new tri-state (None unspecified, True ok, False bad), or the sentinel
    NotImplemented when the token is not this flag."""
    if len(token) < len(name) + 1 or not token.startswith(name):
        return NotImplemented
    rest = token[len(name):]
    if rest.startswith("!="):
        equal, wanted = False, rest[2:]
    elif rest.startswith("="):
        equal, wanted = True, rest[1:]
    else:
        return NotImplemented
    if state is True:
        return True
    return (wanted == value) == equal


class AppInfo:
    """What ManifestParser compares a line's flags with, read from the tree."""

    def __init__(self, files: "Files", platform: str):
        ini = files.read("application.ini").decode("utf-8", "replace")
        fields = dict(l.split("=", 1) for l in ini.splitlines() if "=" in l)
        self.app_id = fields.get("ID", "").lower()
        self.os = OS_TARGET[platform]


def _active(directive: str, flags: list, app: AppInfo, where: str):
    """(registered, content flags) for one line, as ManifestParser decides:
    an unknown modifier drops the line, a failing condition drops it."""
    states, content = {}, set()
    for token in (t.lower() for t in flags):
        for name, value in (("application", app.app_id), ("os", app.os), ("process", "main")):
            new = _string_flag(token, name, value, states.get(name))
            if new is not NotImplemented:
                states[name] = new
                break
        else:
            for name in ("osversion", "appversion", "platformversion", "abi"):
                if token.startswith(name):
                    raise ParityError(f"{where}: `{token}` compares against the host or the "
                                      f"build's version, which a static read cannot decide")
            background = _flag(token, "backgroundtask")
            if background is not None:
                # Not in background-task mode: a line for background tasks only
                # is not registered.
                if background:
                    states["backgroundtask"] = False
                continue
            if directive in CONTENT_FLAGS:
                hit = [n for n in ("contentaccessible", "remoteenabled", "remoterequired")
                       if _flag(token, n) is not None]
                if hit:
                    if _flag(token, hit[0]):
                        content.add(hit[0])
                    continue
            if _flag(token, "xpcnativewrappers") is not None:
                continue
            return False, set()  # "Unrecognized chrome manifest modifier": line dropped
    return all(v is not False for v in states.values()), content


def _normalize(path: str) -> str:
    out = []
    for part in path.split("/"):
        if part == "..":
            out.pop()
        elif part not in ("", "."):
            out.append(part)
    return "/".join(out) + ("/" if path.endswith("/") else "")


class Registry:
    """The registrations of a tree, last one winning, as Gecko keeps them."""

    def __init__(self, files: "Files", platform: str):
        self.files = files
        self.app = AppInfo(files, platform)
        self.packages = {}   # name -> (directory, content flags)
        self.providers = {}  # (kind, package, provider) -> directory
        self.hosts = {}      # resource host -> (target, content flags)
        self.overrides = {}  # chrome URL -> chrome URL
        roots = [m for m in ROOT_MANIFESTS if files.has(m)]
        if not roots:
            raise ParityError("no root chrome.manifest: this is not a Firefox tree")
        for root in roots:
            self._read(root, set())

    def _read(self, path: str, stack: set):
        if path in stack or not self.files.has(path):
            return  # Gecko logs a missing manifest and carries on
        base = path.rsplit("/", 1)[0] + "/" if "/" in path else ""
        for number, line in enumerate(self.files.read(path).decode("utf-8", "replace").splitlines(), 1):
            if line.startswith("#"):
                continue
            tokens = line.split()
            if not tokens or tokens[0] not in DIRECTIVES:
                continue
            argc = DIRECTIVES[tokens[0]]
            if len(tokens) < 1 + argc:
                continue
            args, flags = tokens[1:1 + argc], tokens[1 + argc:]
            registered, content = _active(tokens[0], flags, self.app, f"{path}:{number}")
            if not registered:
                continue
            kind = tokens[0]
            if kind == "manifest":
                self._read(_normalize(base + args[0]), stack | {path})
            elif kind == "content":
                self.packages[args[0]] = (self._dir(base, args[1]), frozenset(content))
            elif kind in ("skin", "locale"):
                self.providers[(kind, args[0], args[1])] = self._dir(base, args[2])
            elif kind == "resource":
                self.hosts[args[0]] = (self._dir(base, args[1]), frozenset(content))
            elif kind == "override":
                self.overrides[args[0]] = args[1]

    @staticmethod
    def _dir(base: str, target: str) -> str:
        return target if ":" in target else _normalize(base + target)

    def resolve(self, target: str) -> str:
        """A registration target - a logical path or a resource:// / chrome://
        URL - to a logical path."""
        if ":" not in target:
            return target
        scheme, rest = target.split("://", 1)
        host, _, sub = rest.partition("/")
        if scheme == "resource":
            host = host or "app"  # resource:/// is the app, ResolveSpecialCases
            if host in self.hosts:
                return self.resolve(self.hosts[host][0]) + sub
            if host in BUILTIN_HOSTS:
                return BUILTIN_HOSTS[host] + sub
            raise ParityError(f"{target}: no resource host {host!r}")
        if scheme == "chrome":
            kind, _, rel = sub.partition("/")
            if kind == "content" and host in self.packages:
                return self.resolve(self.packages[host][0]) + rel
            matches = [d for (k, p, _), d in self.providers.items() if k == kind and p == host]
            if len(matches) == 1:
                return self.resolve(matches[0]) + rel
            raise ParityError(f"{target}: {len(matches)} {kind} providers for {host!r}")
        raise ParityError(f"{target}: not a resource:// or chrome:// URL")


def surface(files: "Files", platform: str) -> dict:
    """{key: value} for everything a page can observe of a tree: one key per
    registration (value: its content flags, or its target for an override),
    and one per readable URL (value: the SHA-256 of what it answers)."""
    reg = Registry(files, platform)
    out = {}
    for name, (_, flags) in reg.packages.items():
        out[f"registers content {name}"] = " ".join(sorted(flags))
    for (kind, package, provider) in reg.providers:
        out[f"registers {kind} {package} {provider}"] = ""
    for host, (_, flags) in reg.hosts.items():
        out[f"registers resource {host}"] = " ".join(sorted(flags))
    for frm, to in reg.overrides.items():
        out[f"registers override {frm}"] = to

    def digest(path):
        return hashlib.sha256(reg.files.read(path)).hexdigest() if reg.files.has(path) else "missing"

    # Readable: every directory of a contentaccessible package - content, skin
    # and locale, since the flag is the package's (AllowContentToAccess) - and
    # of a contentaccessible resource host.
    readable = []
    for name, (directory, flags) in reg.packages.items():
        if "contentaccessible" in flags:
            readable.append((f"chrome://{name}/content/", reg.resolve(directory)))
            for (kind, package, provider), d in reg.providers.items():
                if package == name:
                    readable.append((f"chrome://{name}/{kind}/", reg.resolve(d), provider))
    for host, (target, flags) in reg.hosts.items():
        if "contentaccessible" in flags:
            readable.append((f"resource://{host}/", reg.resolve(target)))
    for prefix, directory, *provider in readable:
        suffix = f" [{provider[0]}]" if provider else ""
        for path in reg.files.under(directory):
            out[f"file {prefix}{path[len(directory):]}{suffix}"] = digest(path)
    # An override changes what a chrome URL answers: a page loading it from a
    # readable package reads the target, not the file the package holds.
    for frm, to in reg.overrides.items():
        host = frm.split("://", 1)[1].split("/", 1)[0] if frm.startswith("chrome://") else None
        package = reg.packages.get(host)
        if package and "contentaccessible" in package[1]:
            keys = [k for k in out if k == f"file {frm}" or k.startswith(f"file {frm} [")]
            for key in keys or [f"file {frm}"]:
                out[key] = digest(reg.resolve(to))
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
        sums.unlink()
        raise ParityError(f"SHA256SUMS of {version} lists {rel} {len(want)} times; "
                          f"removed, download again")
    mar = _download(f"{ARCHIVE}/{version}/{rel}", CACHE / version / f"{platform}.complete.mar")
    data = mar.read_bytes()
    if hashlib.sha256(data).hexdigest() != want[0]:
        # Either may be the bad one: both go, and the next run fetches both.
        mar.unlink()
        sums.unlink()
        raise ParityError(f"{mar} does not match SHA256SUMS; both removed, download again")
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
        if not (isinstance(entry["platforms"], list)
                and all(isinstance(p, str) for p in entry["platforms"])):
            raise ParityError(f"exception platforms must be a list of names: {entry}")
        if platform in entry["platforms"]:
            out[entry["key"]] = entry
    return out


def judge(tree: Path, retail=retail_files, exceptions: Path = EXCEPTIONS) -> tuple:
    """(platform, version, undeclared differences, stale declarations, size).
    `retail(version, platform)` gives the judge's Files; the selftest passes a
    tree of its own, everything else the MAR."""
    platform, version = platform_of(tree), version_of(tree)
    ours = surface(Files.from_directory(tree), platform)
    stock = surface(retail(version, platform), platform)
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
            "override chrome://global/content/license.html chrome://global/content/real.html\n"
            "override chrome://global/content/secret.html chrome://private/content/s.html\n"
            "manifest second.manifest\n"),
        "chrome/second.manifest": "override chrome://global/content/a.css chrome://global/content/a.css\n",
        "chrome/toolkit/content/global/a.css": "body { color: red }\n",
        "chrome/toolkit/content/global/real.html": "<p>license</p>\n",
        "chrome/toolkit/content/global/copy.html": "<p>license</p>\n",
        "chrome/toolkit/content/global/license.html": "<p>shadowed</p>\n",
        "chrome/private/content/s.html": "<p>secret</p>\n",
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
        # From the independent review of 2026-10-15: each was a way through.
        ("the locale of an accessible package differs",
         mutate(**{"chrome/en-US/locale/global/c.properties": "k=w\n"}), True, None),
        ("a content flag other than contentaccessible differs",
         mutate(chrome__chrome_DOT_manifest=manifest.replace(
             "content private private/content/\n", "content private private/content/ remoteenabled\n")),
         True, None),
        ("an override pointing at another file with the same bytes",
         mutate(chrome__chrome_DOT_manifest=manifest.replace(
             "chrome://global/content/real.html", "chrome://global/content/copy.html")), True, None),
        ("the target of an override in a package a page cannot load",
         mutate(chrome__private__content__s_DOT_html="<p>other secret</p>\n"), True, None),
        ("a later registration moves a package (Gecko keeps the last)",
         mutate(chrome__chrome_DOT_manifest=manifest + "content devimg devtools/\n"
                + "content devimg devtools/images/ contentaccessible=true\n"
                + "content devimg private/content/ contentaccessible=true\n"), True, None),
        ("the order of two manifests decides an override",
         mutate(chrome__second_DOT_manifest=
                "override chrome://global/content/a.css chrome://global/content/a.css\n"
                "override chrome://global/content/license.html chrome://global/content/copy.html\n"),
         True, None),
        ("a trailing # makes Gecko drop the line",
         mutate(chrome__chrome_DOT_manifest=manifest.replace(
             "content private private/content/\n", "content private private/content/ # moved\n")),
         True, None),
        ("a line that compares against the host cannot be decided",
         mutate(chrome__chrome_DOT_manifest=manifest + "content juggler juggler/ osversion>=10.0\n"),
         True, None),
        # Near-misses: what Gecko does not register, or a page does not read.
        ("a line for another os is not registered",
         mutate(chrome__chrome_DOT_manifest=manifest + "content juggler juggler/ os=darwin\n"),
         False, None),
        ("a file an override shadows is not what a page reads",
         mutate(chrome__toolkit__content__global__license_DOT_html="<p>other</p>\n"), False, None),
        ("the same registrations split over two manifests",
         mutate(chrome__chrome_DOT_manifest=manifest.replace("content private private/content/\n", ""),
                chrome__second_DOT_manifest="override chrome://global/content/a.css "
                "chrome://global/content/a.css\ncontent private private/content/\n"), False, None),
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
    except Exception as exc:  # a defect of the gate is not a verdict on the build
        print(f"NOT JUDGED: the gate failed: {type(exc).__name__}: {exc}")
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
