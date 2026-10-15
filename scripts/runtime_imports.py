"""Every DLL a Windows build's binaries import is either shipped with it or one
Windows provides - and which ones Windows provides is the retail's answer, not
a list kept here.

firefox.exe, xul.dll, mozglue.dll and nss3.dll import the C++ runtime
(VCRUNTIME140, VCRUNTIME140_1, MSVCP140). The retail ships it beside them; until
firefox-48 we did not (B306), because windows.configure packages it only under
MOZ_AUTOMATION or --with-redist and our cross leg had neither. Nothing saw it:
this machine and the CI runners have the Visual C++ Redistributable, so every
e2e passed, and on a clean Windows - a fresh install, a VM, Windows Sandbox -
the browser did not start.

The rule is about the CLASS, not the four names: an imported DLL that the
archive does not carry must be one the retail also imports without carrying,
which is what Windows provides. The retail is Mozilla's complete MAR of the
same version (stock_parity.retail_files). Direct and delay-load imports both
count: a delay-loaded DLL that is missing fails at its first use instead of at
start, which is later, not better.
"""
from __future__ import annotations

import struct
from pathlib import Path

import stock_parity

BINARY_SUFFIXES = (".exe", ".dll")


class RuntimeError_(stock_parity.ParityError):
    """The imports could not be read or judged; never a pass."""


def pe_imports(data: bytes, where: str) -> set:
    """The DLL names a PE image imports, directly and by delay load, lowercased."""
    if data[:2] != b"MZ":
        raise RuntimeError_(f"{where}: not a PE image")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise RuntimeError_(f"{where}: no PE signature")
    sections, opt_size = struct.unpack_from("<H", data, pe + 6)[0], struct.unpack_from("<H", data, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:
        image_base, dirs = struct.unpack_from("<Q", data, opt + 24)[0], opt + 112
    elif magic == 0x10B:
        image_base, dirs = struct.unpack_from("<I", data, opt + 28)[0], opt + 96
    else:
        raise RuntimeError_(f"{where}: unknown optional header 0x{magic:x}")
    table = opt + opt_size
    spans = []
    for i in range(sections):
        base = table + 40 * i
        vsize, va, rsize, raw = struct.unpack_from("<IIII", data, base + 8)
        spans.append((va, max(vsize, rsize), raw))

    def offset(rva: int) -> int:
        for va, size, raw in spans:
            if va <= rva < va + size:
                return rva - va + raw
        raise RuntimeError_(f"{where}: RVA 0x{rva:x} is in no section")

    def cstring(rva: int) -> str:
        at = offset(rva)
        return data[at:data.index(b"\0", at)].decode("ascii").lower()

    names = set()
    imp_rva, imp_size = struct.unpack_from("<II", data, dirs + 8 * 1)
    if imp_rva:
        at = offset(imp_rva)
        while True:
            desc = struct.unpack_from("<IIIII", data, at)
            if not any(desc):
                break
            names.add(cstring(desc[3]))
            at += 20
    delay_rva, _ = struct.unpack_from("<II", data, dirs + 8 * 13)
    if delay_rva:
        at = offset(delay_rva)
        while True:
            desc = struct.unpack_from("<8I", data, at)
            if not any(desc):
                break
            attributes, name = desc[0], desc[1]
            # Attribute bit 0 says the fields are RVAs; old images kept VAs.
            names.add(cstring(name if attributes & 1 else name - image_base))
            at += 32
    return names


def _inventory(files: "stock_parity.Files", where: str) -> tuple:
    """({importer: imports}, {shipped dll names}) of a tree."""
    imports, shipped = {}, set()
    for path in files.under(""):
        if path.lower().endswith(BINARY_SUFFIXES):
            name = path.rsplit("/", 1)[-1].lower()
            if name.endswith(".dll"):
                shipped.add(name)
            imports[path] = pe_imports(files.read(path), f"{where} {path}")
    return imports, shipped


def make_pe(imports=(), delay_imports=()) -> bytes:
    """A minimal PE32+ image whose import and delay-import tables name `imports`
    and `delay_imports`: what the selftest feeds the parser."""
    sect_rva, sect_raw = 0x1000, 0x200
    body = bytearray()
    n_imp, n_del = len(imports), len(delay_imports)
    names_at = 20 * (n_imp + 1) + 32 * (n_del + 1)
    name_rvas = {}
    names = bytearray()
    for dll in list(imports) + list(delay_imports):
        if dll not in name_rvas:
            name_rvas[dll] = sect_rva + names_at + len(names)
            names += dll.encode("ascii") + b"\0"
    for dll in imports:
        body += struct.pack("<IIIII", 0, 0, 0, name_rvas[dll], 0)
    body += bytes(20)
    for dll in delay_imports:
        body += struct.pack("<8I", 1, name_rvas[dll], 0, 0, 0, 0, 0, 0)
    body += bytes(32)
    body += names
    size = len(body)
    dos = bytearray(0x40)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 0x40)
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = bytearray(240)
    struct.pack_into("<H", opt, 0, 0x20B)
    struct.pack_into("<Q", opt, 24, 0x140000000)
    struct.pack_into("<I", opt, 108, 16)
    if n_imp:
        struct.pack_into("<II", opt, 112 + 8 * 1, sect_rva, 20 * (n_imp + 1))
    if n_del:
        struct.pack_into("<II", opt, 112 + 8 * 13, sect_rva + 20 * (n_imp + 1), 32 * (n_del + 1))
    section = struct.pack("<8sIIIIIIHHI", b".idata", size, sect_rva, size, sect_raw, 0, 0, 0, 0, 0xC0000040)
    head = bytes(dos) + b"PE\0\0" + coff + bytes(opt) + section
    return head + bytes(sect_raw - len(head)) + bytes(body)


def selftest() -> int:
    """Known-bad inputs and near-misses, on trees of minimal PE images."""
    def tree(binaries):
        return stock_parity.Files({rel: (lambda d=data: d) for rel, data in binaries.items()})

    os_dlls = ["kernel32.dll", "api-ms-win-crt-runtime-l1-1-0.dll"]
    retail = tree({"firefox.exe": make_pe(os_dlls + ["vcruntime140.dll", "mozglue.dll"]),
                   "mozglue.dll": make_pe(os_dlls + ["vcruntime140.dll"]),
                   "vcruntime140.dll": make_pe(["kernel32.dll"])})
    ok_exe = make_pe(os_dlls + ["vcruntime140.dll", "mozglue.dll"])
    cases = [
        ("the runtime shipped beside the binaries", {
            "firefox.exe": ok_exe, "mozglue.dll": make_pe(["kernel32.dll", "vcruntime140.dll"]),
            "vcruntime140.dll": make_pe(["kernel32.dll"])}, False),
        ("the runtime imported and not shipped (every build until firefox-48)", {
            "firefox.exe": ok_exe, "mozglue.dll": make_pe(["kernel32.dll", "vcruntime140.dll"])}, True),
        ("a delay-loaded DLL that is not shipped", {
            "firefox.exe": make_pe(["kernel32.dll"], delay_imports=["notshipped.dll"])}, True),
        ("a DLL Windows provides, as the retail expects it", {
            "firefox.exe": make_pe(os_dlls)}, False),
        ("a firefox.exe whose imports read as nothing", {
            "firefox.exe": make_pe([])}, True),
    ]
    bad = 0
    for name, binaries, must_refuse in cases:
        try:
            refused = bool(missing_runtime(tree(binaries), retail))
        except stock_parity.ParityError:
            refused = True
        ok = refused == must_refuse
        bad += not ok
        print(f"  [{'ok' if ok else 'GATE WRONG':9}] {name}: "
              f"{'must refuse' if must_refuse else 'must pass'}, {'refused' if refused else 'passed'}")
    fired = sum(1 for c in cases if c[2])
    print(f"[runtime_imports selftest] {len(cases)} cases ({fired} known-bad, "
          f"{len(cases) - fired} that must not fire), {bad} wrong")
    return 1 if bad else 0


def missing_runtime(ours: "stock_parity.Files", retail: "stock_parity.Files") -> dict:
    """{dll: [importers]} for every DLL our binaries import that is neither
    shipped with them nor one the retail expects from Windows."""
    our_imports, our_shipped = _inventory(ours, "this build")
    exe = [p for p in our_imports if p.lower().endswith("firefox.exe")]
    if not exe or "kernel32.dll" not in our_imports[exe[0]]:
        # A parser that reads nothing would pass everything.
        raise RuntimeError_("firefox.exe imports nothing readable: the imports were not read")
    retail_imports, retail_shipped = _inventory(retail, "the retail")
    from_windows = set().union(*retail_imports.values()) - retail_shipped
    missing = {}
    for importer, names in sorted(our_imports.items()):
        for dll in sorted(names - our_shipped - from_windows):
            missing.setdefault(dll, []).append(importer)
    return missing
