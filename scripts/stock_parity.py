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
So the surface is every registration a manifest makes, and what a page reads
of every file it can load.

WHAT A PAGE READS OF A FILE is decided by its type, and it is less than its
bytes (measured 2026-10-16 on the retail 151 and on firefox-47, Windows, a page
in standards and in quirks mode loading every differing URL through <script>,
<link rel=stylesheet>, fetch in both modes, <img> with canvas and
createImageBitmap, <iframe>, <object>, Worker and <link rel=preload>, against
an absent file of the same package as the control). See observable().
  - An image loads and gives its intrinsic size; its pixels do not come back,
    because the canvas is tainted (getImageData throws SecurityError on both).
    So a PNG compares by its size, an ICO by the size of each of its images,
    an SVG by what of its root decides its size.
  - An HTML, an XHTML and a .properties file answer every channel exactly as a
    file that does not exist, so they are not on the surface at all: a page
    cannot tell ours from the retail's, or either from nothing. That covers
    buildconfig.html, which spells out our mozconfig.
  - Anything else - a stylesheet, whose rules apply to the page, a script,
    every type nobody measured - compares by its bytes. The owner's rule (2026-10-14): "la nostra versione deve
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

THE BRANDING A PAGE READS IS THE CORE'S, not the build's, on an engine that
redirects it (P60): with the engine on, every URI under the branding package's
content directory - chrome://branding/content/ and the same files through
resource://usercontext-content/branding/ - is served from the zip archive
invisible_core declares in zoom.stealth.branding.archive (as jar:, the channel
class the retail uses), and the build's own brand in omni.ja is never read. So
on such an engine the archive's entries take the place of the package's files,
those it lacks included, exactly as the redirect does, and they pass through
observable() like any other file. Whether the engine redirects is read from the
tree, the pref's name in the engine library, because no manifest changes;
judging such an engine without the declared archive is refused as not judged,
since what would be judged is what a page never reads. What this gate cannot
see is HOW the engine serves the entries: it compares what a file holds, not
the channel it comes through. A file: channel keeps the content type a loader
suggests where a jar: one replaces it with the entry's own, so a PNG loaded as
a script or a stylesheet loaded on a file:-served branding and failed on the
retail - 62 loads a page saw and this gate did not (2026-10-16). That is the
page probe's to see, tests/gates/page_surface_probe in the workbench.

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
#: The chrome package whose content directory an engine with the branding
#: redirect serves from the declared archive (netwerk/protocol/res/
#: StealthBranding.h), and the pref that declares it: its name is in the engine
#: library exactly when the engine has the redirect (StaticPrefList.yaml).
BRANDING_PACKAGE = "branding"
BRANDING_PREF = b"zoom.stealth.branding.archive"


class ParityError(Exception):
    """The surface could not be read or judged; never a pass."""


def open_jar(data: bytes, where: str) -> "JarReader":
    """Mozilla's JarReader over `data`, refusing first what is not a zip.

    ⛔ ITS SEARCH FOR THE END RECORD NEVER ENDS ON DATA SHORTER THAN THE RECORD:
    mozjar.py starts at -22 and stops only at `offset == -len(data)`, which a
    19-byte file never reaches (measured 2026-10-15: a corrupt nested archive
    hung the gate instead of failing it). Every real omni.ja, Mozilla's
    optimized ones included, ends with the record, so this refuses nothing
    legitimate.
    """
    if len(data) < 22 or data[-(0xFFFF + 22):].rfind(b"PK\x05\x06") < 0:
        raise ParityError(f"{where} is not a zip: no end of central directory record")
    return JarReader(data=data)


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


def serves_declared_branding(tree: Path, platform: str) -> bool:
    """Whether the engine of `tree` serves the branding from the archive
    invisible_core declares (the module docstring), read from its library.

    ⛔ The declared branding is judged by its bytes and observable() ONLY
    because the engine serves it through jar:, the channel class the retail
    serves omni.ja with: observable() projects a file onto what a page reads
    ASSUMING that channel, and the page probe measured it with that assumption
    true on both sides. Served any other way (the declared directory of
    2026-10-16 went through file:), the projection is false and this gate is
    blind to it, so Files.serving refuses anything but an archive."""
    lib = tree / ("xul.dll" if platform.startswith("win") else "libxul.so")
    if not lib.is_file():
        raise ParityError(f"no {lib.name} in {tree}: which branding a page reads is not decidable")
    return BRANDING_PREF in lib.read_bytes()


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
                cls._add_jar(loaders, open_jar((root / rel).read_bytes(), rel), prefix, rel)
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
                cls._add_jar(loaders, open_jar(entries[rel], rel), prefix, rel)
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

    def serving(self, directory: str, archive: Path) -> "Files":
        """These files with `directory` (a logical path ending in /) holding
        exactly the root entries of the zip `archive`, as the branding redirect
        serves them. The engine serves only a regular file that is a zip with
        nothing but files at its root (StealthBrandingDeclaredArchive) and
        refuses to launch on any other; such an archive is not judged either."""
        import zipfile
        if archive.is_symlink() or not archive.is_file():
            raise ParityError(f"the declared branding archive {archive} is not a regular file")
        try:
            with zipfile.ZipFile(archive) as z:
                entries = {i.filename: z.read(i) for i in z.infolist()}
        except zipfile.BadZipFile as exc:
            raise ParityError(f"the declared branding archive {archive} is not a zip: {exc}") from exc
        nested = sorted(n for n in entries if "/" in n)
        if nested or not entries:
            raise ParityError(f"the declared branding archive {archive} holds "
                              f"{nested or 'nothing'} besides files at its root: "
                              f"the engine refuses to launch on it")
        loaders = {p: f for p, f in self._loaders.items() if not p.startswith(directory)}
        for name, data in sorted(entries.items()):
            loaders[directory + name] = (lambda d=data: d)
        return Files(loaders)

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


#: Types a page cannot load in any way, measured: every channel answers as for
#: an absent file (the module docstring). Only what was measured is here.
UNLOADABLE = (".html", ".xhtml", ".properties")


def _png_size(data: bytes, where: str) -> str:
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        raise ParityError(f"{where}: not a PNG, so its size cannot be read")
    w, h = struct.unpack(">II", data[16:24])
    # An eXIf chunk can rotate the image, which swaps its natural size.
    exif, i = b"", 8
    while i + 8 <= len(data):
        n, = struct.unpack(">I", data[i:i + 4])
        if data[i + 4:i + 8] == b"eXIf":
            exif = data[i + 8:i + 8 + n]
        i += 12 + n
    return f"{w}x{h}" + (f" exif {hashlib.sha256(exif).hexdigest()}" if exif else "")


def _ico_sizes(data: bytes, where: str) -> str:
    if len(data) < 6 or data[:2] != b"\0\0":
        raise ParityError(f"{where}: not an ICO, so its sizes cannot be read")
    kind, count = struct.unpack("<HH", data[2:6])
    sizes = []
    for k in range(count):
        entry = data[6 + 16 * k:22 + 16 * k]
        if len(entry) < 16:
            raise ParityError(f"{where}: the ICO directory is cut short")
        bpp, size, offset = struct.unpack("<HII", entry[6:16])
        image = data[offset:offset + size]
        # The size an image REALLY has is in its own header: the directory's
        # bytes are a hint the decoder does not trust.
        if image[:8] == b"\x89PNG\r\n\x1a\n":
            sizes.append(f"png {_png_size(image, where)}")
        elif len(image) >= 16:
            w, h = struct.unpack("<ii", image[4:12])
            sizes.append(f"bmp {w}x{h // 2}/{bpp}")
        else:
            raise ParityError(f"{where}: an ICO image lies outside the file")
    return f"ico {kind} " + " ".join(sizes)


def _svg_box(data: bytes, where: str) -> str:
    """What of an SVG decides the size an <img> gives it: the root's width,
    height, viewBox, preserveAspectRatio and style, and every <style> in the
    document, since a rule can size the root. The drawing itself is pixels."""
    import re
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise ParityError(f"{where}: an SVG that is not UTF-8")
    root = re.search(r"<svg\b[^>]*>", text)
    if not root:
        raise ParityError(f"{where}: no <svg> root")
    attrs = dict((k, v1 or v2) for k, v1, v2 in
                 re.findall(r"([\w:.-]+)\s*=\s*(?:\"([^\"]*)\"|'([^']*)')", root.group(0)))
    box = {k: attrs[k] for k in ("width", "height", "viewBox", "preserveAspectRatio", "style")
           if k in attrs}
    styles = re.findall(r"<style\b[^>]*>.*?</style>", text, re.S)
    return json.dumps({"root": box, "style": styles}, sort_keys=True)


def observable(path: str, data: bytes) -> str:
    """What a page reads of the file at `path` (the module docstring)."""
    ext = path[path.rfind("."):].lower() if "." in path.rsplit("/", 1)[-1] else ""
    # Gecko picks the decoder from the bytes, not the name (imgLoader sniffs
    # the content): the retail ships favicons named .ico that are PNGs.
    if ext in (".png", ".ico"):
        if data[:8] == b"\x89PNG\r\n\x1a\n":
            return "image " + _png_size(data, path)
        return "image " + _ico_sizes(data, path)
    if ext == ".svg":
        return "image " + _svg_box(data, path)
    return "bytes " + hashlib.sha256(data).hexdigest()


def surface(files: "Files", platform: str) -> dict:
    """{key: value} for everything a page can observe of a tree: one key per
    registration (value: its content flags, or its target for an override),
    and one per URL a page can load (value: what it reads there,
    observable())."""
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
        return observable(path, reg.files.read(path)) if reg.files.has(path) else "missing"

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
            if path.lower().endswith(UNLOADABLE):
                continue
            out[f"file {prefix}{path[len(directory):]}{suffix}"] = digest(path)
    # An override changes what a chrome URL answers: a page loading it from a
    # readable package reads the target, not the file the package holds.
    for frm, to in reg.overrides.items():
        host = frm.split("://", 1)[1].split("/", 1)[0] if frm.startswith("chrome://") else None
        package = reg.packages.get(host)
        if package and "contentaccessible" in package[1] and not frm.lower().endswith(UNLOADABLE):
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
                        if key.startswith("registers ") or not stock[key].startswith("bytes ")
                        else "content differs")
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


def judge(tree: Path, retail=retail_files, exceptions: Path = EXCEPTIONS,
          branding: Path | None = None) -> tuple:
    """(platform, version, undeclared differences, stale declarations, size).
    `retail(version, platform)` gives the judge's Files; the selftest passes a
    tree of its own, everything else the MAR. `branding` is the archive
    invisible_core declares, required exactly when the engine serves it."""
    platform, version = platform_of(tree), version_of(tree)
    files = Files.from_directory(tree)
    if serves_declared_branding(tree, platform):
        if branding is None:
            raise ParityError("this engine serves the branding invisible_core declares, not "
                              "its own: pass the declared archive (--branding)")
        reg = Registry(files, platform)
        if BRANDING_PACKAGE not in reg.packages:
            raise ParityError(f"no {BRANDING_PACKAGE} package registered: nothing to redirect")
        files = files.serving(reg.resolve(reg.packages[BRANDING_PACKAGE][0]), branding)
    elif branding is not None:
        raise ParityError("a branding archive was passed, but this engine serves its own")
    ours = surface(files, platform)
    stock = surface(retail(version, platform), platform)
    diff = differences(ours, stock)
    declared = load_exceptions(platform, exceptions)
    undeclared = {k: v for k, v in diff.items() if k not in declared}
    stale = sorted(k for k in declared if k not in diff)
    return platform, version, undeclared, stale, len(stock)


def judge_branding(archive: Path, version: str, platform: str, retail=retail_files) -> dict:
    """{key: description} of every difference a page could read between the
    retail `version` for `platform` and the same retail serving `archive` as
    its branding, the way an engine with the redirect serves it.

    The branding a page reads belongs to invisible_core, so a release of the
    core can change it without any build of the engine: this is the judgement
    the core's CI runs on its own declared set, with this file of the engine
    the seal pins, so that the comparison and observable() are one code."""
    files = retail(version, platform)
    reg = Registry(files, platform)
    if BRANDING_PACKAGE not in reg.packages:
        raise ParityError(f"the retail {version} ({platform}) registers no {BRANDING_PACKAGE} package")
    served = files.serving(reg.resolve(reg.packages[BRANDING_PACKAGE][0]), archive)
    return differences(surface(served, platform), surface(files, platform))


def selftest() -> int:
    """The gate's known-bad inputs and the near-misses that must not fire, on
    trees built here: no archive, no browser, no network. The retail is a tree
    too, packed in omni.ja as Mozilla packs it; ours is loose unless a case
    says otherwise, so every case also proves the two layouts compare."""
    import shutil
    import tempfile
    import zipfile

    elf = b"\x7fELF" + bytes(14) + struct.pack("<H", 0x3E) + bytes(44)

    def png(w, h, body=b"A", exif=None):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + b"CRC!"
        out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">II", w, h) + b"\x08\x06\0\0\0")
        if exif is not None:
            out += chunk(b"eXIf", exif)
        return out + chunk(b"IDAT", body) + chunk(b"IEND", b"")

    def ico(*images):
        head = struct.pack("<HHH", 0, 1, len(images))
        offset, dirs, blobs = 6 + 16 * len(images), b"", b""
        for image in images:
            dirs += struct.pack("<BBBBHHII", 0, 0, 0, 0, 1, 32, len(image), offset + len(blobs))
            blobs += image
        return head + dirs + blobs

    # Placeholders the trees are written with, so that a case names an image.
    IMAGES = {"PNG16x16": png(16, 16), "PNG16x16-redrawn": png(16, 16, b"B"),
              "PNG16x17": png(16, 17), "PNG16x16-rotated": png(16, 16, exif=b"MM\0*"),
              "ICO16+32": ico(png(16, 16), png(32, 32)),
              "ICO16+32-redrawn": ico(png(16, 16, b"B"), png(32, 32, b"B")),
              "ICO16+48": ico(png(16, 16), png(48, 48)),
              "NOTAPNG": b"GIF89a not a png"}
    base = {
        "chrome.manifest": "manifest chrome/chrome.manifest\n",
        "chrome/chrome.manifest": (
            "content global toolkit/content/global/ contentaccessible=yes\n"
            "skin global classic/1.0 toolkit/skin/global/\n"
            "locale global en-US en-US/locale/global/\n"
            "content devimg devtools/images/ contentaccessible=true\n"
            "content private private/content/\n"
            "resource content-accessible resource://gre/contentaccessible/ contentaccessible=yes\n"
            "override chrome://global/content/license.css chrome://global/content/real.css\n"
            "override chrome://global/content/secret.css chrome://private/content/s.css\n"
            "content branding browsercontent/branding/ contentaccessible=yes\n"
            "resource usercontext-content browsercontent/ contentaccessible=yes\n"
            "manifest second.manifest\n"),
        "chrome/second.manifest": "override chrome://global/content/a.css chrome://global/content/a.css\n",
        "chrome/toolkit/content/global/a.css": "body { color: red }\n",
        "chrome/toolkit/content/global/real.css": "<p>license</p>\n",
        "chrome/toolkit/content/global/copy.css": "<p>license</p>\n",
        "chrome/toolkit/content/global/license.css": "<p>shadowed</p>\n",
        "chrome/private/content/s.css": "<p>secret</p>\n",
        "chrome/toolkit/skin/global/b.css": "x {}\n",
        "chrome/en-US/locale/global/c.ftl": "k=v\n",
        "chrome/devtools/images/d.svg": "<svg/>\n",
        "chrome/toolkit/content/global/i.png": "PNG16x16",
        "chrome/toolkit/content/global/k.ico": "ICO16+32",
        "chrome/toolkit/content/global/fav.ico": "PNG16x16",
        "chrome/toolkit/content/global/w.svg": '<svg viewBox="0 0 359 80"><path d="M0 0h9"/></svg>\n',
        "chrome/toolkit/content/global/about.xhtml": "<window/>\n",
        "chrome/toolkit/content/global/buildconfig.html": "<p>--enable-official</p>\n",
        "chrome/en-US/locale/global/brand.properties": "brandShortName=Firefox\n",
        "chrome/private/content/e.js": "1\n",
        "contentaccessible/f.css": "y {}\n",
        "chrome/browsercontent/branding/about.png": "PNG16x16",
        "chrome/browsercontent/branding/aboutDialog.css": "#a {}\n",
        "chrome/browsercontent/branding/wordmark.svg": '<svg viewBox="0 0 359 80"/>\n',
        "chrome/browsercontent/other.css": "o {}\n",
    }
    # The retail's brand, as invisible_core ships it, and a build whose own
    # brand differs from it in a stylesheet and an image's size.
    RETAIL_BRAND = {"about.png": "PNG16x16", "aboutDialog.css": "#a {}\n",
                    "wordmark.svg": '<svg viewBox="0 0 359 80"/>\n'}
    OWN_BRAND = {"chrome/browsercontent/branding/aboutDialog.css": "#b {}\n",
                 "chrome/browsercontent/branding/about.png": "PNG16x17"}

    def write(root, files, omni, redirects=False):
        files = {rel: IMAGES.get(text, text) for rel, text in files.items()}
        root.mkdir(parents=True)
        (root / "firefox").write_bytes(elf)
        (root / "libxul.so").write_bytes(elf + (BRANDING_PREF if redirects else b"") + b"\0")
        (root / "application.ini").write_bytes(b"[App]\nVersion=151.0\n")
        if omni:
            with zipfile.ZipFile(root / "omni.ja", "w") as z:
                for rel, text in files.items():
                    z.writestr(rel, text)
        else:
            for rel, text in files.items():
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_bytes(text if isinstance(text, bytes) else text.encode())

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
         mutate(chrome__toolkit__content__global__real_DOT_css="<p>other</p>\n"), True, None),
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
         mutate(**{"chrome/en-US/locale/global/c.ftl": "k=w\n"}), True, None),
        ("a content flag other than contentaccessible differs",
         mutate(chrome__chrome_DOT_manifest=manifest.replace(
             "content private private/content/\n", "content private private/content/ remoteenabled\n")),
         True, None),
        ("an override pointing at another file with the same bytes",
         mutate(chrome__chrome_DOT_manifest=manifest.replace(
             "chrome://global/content/real.css", "chrome://global/content/copy.css")), True, None),
        ("the target of an override in a package a page cannot load",
         mutate(chrome__private__content__s_DOT_css="<p>other secret</p>\n"), True, None),
        ("a later registration moves a package (Gecko keeps the last)",
         mutate(chrome__chrome_DOT_manifest=manifest + "content devimg devtools/\n"
                + "content devimg devtools/images/ contentaccessible=true\n"
                + "content devimg private/content/ contentaccessible=true\n"), True, None),
        ("the order of two manifests decides an override",
         mutate(chrome__second_DOT_manifest=
                "override chrome://global/content/a.css chrome://global/content/a.css\n"
                "override chrome://global/content/license.css chrome://global/content/copy.css\n"),
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
         mutate(chrome__toolkit__content__global__license_DOT_css="<p>other</p>\n"), False, None),
        ("a corrupt omni.ja is refused as unreadable, not hung on", "CORRUPT_OMNI", True, None),
        # What a page READS of a file, not its bytes (measured 2026-10-16).
        ("an image redrawn in the same size",
         mutate(chrome__toolkit__content__global__i_DOT_png="PNG16x16-redrawn"), False, None),
        ("an image of another size",
         mutate(chrome__toolkit__content__global__i_DOT_png="PNG16x17"), True, None),
        ("an image an eXIf chunk rotates",
         mutate(chrome__toolkit__content__global__i_DOT_png="PNG16x16-rotated"), True, None),
        ("an image we lack (a page sees load against error)",
         mutate(chrome__toolkit__content__global__i_DOT_png=None), True, None),
        ("a png that is not a png is not judged, never passed",
         mutate(chrome__toolkit__content__global__i_DOT_png="NOTAPNG"), True, None),
        ("an icon whose images are redrawn in the same sizes",
         mutate(chrome__toolkit__content__global__k_DOT_ico="ICO16+32-redrawn"), False, None),
        ("a favicon named .ico that is a PNG, redrawn in the same size (the retail ships them)",
         mutate(chrome__toolkit__content__global__fav_DOT_ico="PNG16x16-redrawn"), False, None),
        ("an icon one of whose images has another size",
         mutate(chrome__toolkit__content__global__k_DOT_ico="ICO16+48"), True, None),
        ("an svg drawn differently in the same box",
         mutate(chrome__toolkit__content__global__w_DOT_svg='<svg viewBox="0 0 359 80"><g><path d="M1 1h8"/></g></svg>\n'),
         False, None),
        ("an svg whose viewBox differs (the wordmarks until firefox-48)",
         mutate(chrome__toolkit__content__global__w_DOT_svg='<svg viewBox="0 0 372 99"><path d="M0 0h9"/></svg>\n'),
         True, None),
        ("an svg whose <style> could size its root",
         mutate(chrome__toolkit__content__global__w_DOT_svg='<svg viewBox="0 0 359 80"><style>svg{width:9px}</style><path d="M0 0h9"/></svg>\n'),
         True, None),
        ("an xhtml whose bytes differ (the updater's about dialog)",
         mutate(chrome__toolkit__content__global__about_DOT_xhtml="<window><deck/></window>\n"), False, None),
        ("an html only the retail has", mutate(chrome__toolkit__content__global__buildconfig_DOT_html=None),
         False, None),
        ("a .properties whose bytes differ (the brand name)",
         mutate(**{"chrome/en-US/locale/global/brand.properties": "brandShortName=Nightly\n"}), False, None),
        ("a stylesheet still compares by its bytes",
         mutate(chrome__toolkit__content__global__a_DOT_css="body { color: red; }\n"), True, None),
        ("the same registrations split over two manifests",
         mutate(chrome__chrome_DOT_manifest=manifest.replace("content private private/content/\n", ""),
                chrome__second_DOT_manifest="override chrome://global/content/a.css "
                "chrome://global/content/a.css\ncontent private private/content/\n"), False, None),
        # The branding an engine with the redirect serves from the archive
        # invisible_core declares (P60). The fifth field: (the engine library
        # names the pref, the entries of the declared archive, "NOTAZIP", or None).
        ("an engine that serves the declared brand, the retail's, over its own that differs",
         mutate(**OWN_BRAND), False, None, (True, RETAIL_BRAND)),
        ("the declared brand's stylesheet differs (both ways in read it)",
         mutate(**OWN_BRAND), True, None, (True, {**RETAIL_BRAND, "aboutDialog.css": "#c {}\n"})),
        ("the declared brand's image redrawn in the same size",
         mutate(**OWN_BRAND), False, None, (True, {**RETAIL_BRAND, "about.png": "PNG16x16-redrawn"})),
        ("the declared brand's image of another size",
         mutate(), True, None, (True, {**RETAIL_BRAND, "about.png": "PNG16x17"})),
        ("a declared brand with a file the retail lacks",
         mutate(), True, None, (True, {**RETAIL_BRAND, "extra.css": "e {}\n"})),
        ("a declared brand without a file the retail has",
         mutate(), True, None, (True, {k: v for k, v in RETAIL_BRAND.items() if k != "wordmark.svg"})),
        ("a declared archive with an entry under a directory, on which the engine refuses to launch",
         mutate(), True, None, (True, {**RETAIL_BRAND, "sub/x.png": "PNG16x16"})),
        ("a declared archive that is not a zip, on which the engine refuses to launch",
         mutate(), True, None, (True, "NOTAZIP")),
        ("a declared DIRECTORY, served through file: where observable() assumes jar:",
         mutate(), True, None, (True, "DIRECTORY")),
        ("an engine that serves the declared brand, judged without it, is not judged",
         mutate(), True, None, (True, None)),
        ("a declared archive for an engine that serves its own brand",
         mutate(), True, None, (False, RETAIL_BRAND)),
        ("an engine library without the pref: its own brand is what a page reads",
         mutate(**OWN_BRAND), True, None, (False, None)),
        # The same judgement on the declared set alone, as the core's CI runs it
        # (judge_branding): the sixth field marks it.
        ("the core's declared set alone, the retail's", None, False, None, (True, RETAIL_BRAND), "SET"),
        ("the core's declared set alone, a stylesheet changed", None, True, None,
         (True, {**RETAIL_BRAND, "aboutDialog.css": "#a { }\n"}), "SET"),
        ("the core's declared set alone, a file missing", None, True, None,
         (True, {k: v for k, v in RETAIL_BRAND.items() if k != "about.png"}), "SET"),
    ]
    bad = 0
    work = Path(tempfile.mkdtemp(prefix="stock-parity-selftest-"))
    try:
        write(work / "retail", base, omni=True)
        retail = Files.from_directory(work / "retail")
        for i, (name, ours, must_refuse, declared, *brand) in enumerate(cases):
            redirects, brand_files = brand[0] if brand else (False, None)
            set_only = brand[1:] == ["SET"]
            tree = work / f"ours{i}"
            if ours == "CORRUPT_OMNI":
                write(tree, base, omni=False)
                (tree / "omni.ja").write_bytes(b"PK not really a zip")
            elif not set_only:
                write(tree, ours, omni=False, redirects=redirects)
            branding = None
            if brand_files == "NOTAZIP":
                branding = work / f"brand{i}.zip"
                branding.write_bytes(b"PK not really a zip")
            elif brand_files == "DIRECTORY":
                # The retail's own files, loose: right bytes, wrong channel.
                branding = work / f"brand{i}"
                branding.mkdir()
                for rel, text in RETAIL_BRAND.items():
                    text = IMAGES.get(text, text)
                    (branding / rel).write_bytes(text if isinstance(text, bytes) else text.encode())
            elif brand_files is not None:
                branding = work / f"brand{i}.zip"
                with zipfile.ZipFile(branding, "w") as z:
                    for rel, text in brand_files.items():
                        text = IMAGES.get(text, text)
                        z.writestr(rel, text if isinstance(text, bytes) else text.encode())
            exc = work / f"exceptions{i}.json"
            exc.write_bytes(json.dumps({"exceptions": [
                {"key": k, "platforms": ["linux-x86_64"], "why": "selftest", "measured": "selftest"}
                for k in declared or []]}).encode())
            try:
                if set_only:
                    refused = bool(judge_branding(branding, "151.0", "linux-x86_64", lambda v, p: retail))
                else:
                    _, _, undeclared, stale, _ = judge(tree, lambda v, p: retail, exc, branding)
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
    if len(argv) >= 5 and argv[1] == "--branding-against-retail":
        archive, version, platforms = Path(argv[2]), argv[3], argv[4:]
        try:
            found = {p: judge_branding(archive, version, p) for p in platforms}
        except ParityError as exc:
            print(f"NOT JUDGED: {exc}")
            return 3
        except Exception as exc:  # a defect of the gate is not a verdict on the set
            print(f"NOT JUDGED: the gate failed: {type(exc).__name__}: {exc}")
            return 3
        for platform, diff in found.items():
            print(f"{platform} {version}: {len(diff)} differences a page reads in {archive}")
            for key, what in diff.items():
                print(f"  {what:40s} {key}")
        return 1 if any(found.values()) else 0
    if len(argv) == 4 and argv[2] == "--branding":
        branding = Path(argv[3])
    elif len(argv) == 2:
        branding = None
    else:
        print("usage: stock_parity.py <extracted firefox tree> [--branding <declared archive>] | "
              "--branding-against-retail <archive> <version> <platform>... | --selftest")
        return 2
    try:
        platform, version, undeclared, stale, size = judge(Path(argv[1]), branding=branding)
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
