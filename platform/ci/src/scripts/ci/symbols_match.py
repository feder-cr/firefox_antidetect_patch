"""The crash symbols describe the binaries that ship: refuse a symbols archive
that does not.

    python3 scripts/ci symbols-match --package out/<asset> --symbols symbols/<zip>

Every binary of the package that has a directory in the symbols archive (a
.dll/.exe by its .pdb name, an ELF by its own name) must have one under its
OWN debug id - the CodeView id of a PE, the GNU build id of an ELF, in
breakpad's form. xul must be among them, so the check never passes empty.

Why it exists: in run 40593659072 the symbols step recompiled Rust and
relinked xul after the package was made, and the archive it uploaded described
a xul.dll (3E2BB500...) that was not the one shipped (030D79D8...). A crash
report from that build could never have been read, and nothing said so.
"""
from __future__ import annotations

import struct
import tarfile
import zipfile
from pathlib import Path


class MismatchError(Exception):
    pass


def _guid_id(raw16: bytes, age: int) -> str:
    a, b, c, rest = struct.unpack("<IHH8s", raw16)
    return "%08X%04X%04X%s%X" % (a, b, c, rest.hex().upper(), age)


def pe_debug_id(data: bytes) -> str | None:
    """The CodeView (RSDS) id of a PE, from its debug directory."""
    if data[:2] != b"MZ":
        return None
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        return None
    nsect, = struct.unpack_from("<H", data, pe + 6)
    optsize, = struct.unpack_from("<H", data, pe + 20)
    opt = pe + 24
    magic, = struct.unpack_from("<H", data, opt)
    dirs = opt + (112 if magic == 0x20B else 96)
    dbg_rva, dbg_size = struct.unpack_from("<II", data, dirs + 6 * 8)
    sections = opt + optsize

    def to_offset(rva):
        for i in range(nsect):
            s = sections + 40 * i
            vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
            if vaddr <= rva < vaddr + max(vsize, rawsize):
                return rawptr + rva - vaddr
        return None

    at = to_offset(dbg_rva) if dbg_rva else None
    if at is None:
        return None
    for i in range(dbg_size // 28):
        kind, size, _, ptr = struct.unpack_from("<IIII", data, at + 28 * i + 12)
        if kind == 2 and data[ptr:ptr + 4] == b"RSDS":
            age, = struct.unpack_from("<I", data, ptr + 20)
            return _guid_id(data[ptr + 4:ptr + 20], age)
    return None


def elf_debug_id(data: bytes) -> str | None:
    """The GNU build id of a 64-bit little-endian ELF, as breakpad names it."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    shoff, = struct.unpack_from("<Q", data, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    for i in range(shnum):
        s = shoff + i * shentsize
        kind, = struct.unpack_from("<I", data, s + 4)
        if kind != 7:  # SHT_NOTE
            continue
        off, size = struct.unpack_from("<QQ", data, s + 0x18)
        p = off
        while p + 12 <= off + size:
            namesz, descsz, ntype = struct.unpack_from("<III", data, p)
            name = data[p + 12:p + 12 + namesz]
            desc_at = p + 12 + ((namesz + 3) & ~3)
            if ntype == 3 and name == b"GNU\0":
                return _guid_id(data[desc_at:desc_at + descsz][:16].ljust(16, b"\0"), 0)
            p = desc_at + ((descsz + 3) & ~3)
    return None


def _symbol_name(name: str) -> str:
    base, dot, ext = name.rpartition(".")
    return base + ".pdb" if dot and ext.lower() in ("dll", "exe") else name


def package_binaries(package: Path) -> dict:
    """basename -> bytes, for the package's PE and ELF files."""
    out = {}
    if package.name.endswith(".zip"):
        with zipfile.ZipFile(package) as z:
            for info in z.infolist():
                if not info.is_dir():
                    head = z.open(info).read(4)
                    if head[:2] == b"MZ" or head == b"\x7fELF":
                        out[info.filename.rsplit("/", 1)[-1]] = z.read(info)
    else:
        with tarfile.open(package) as t:
            for m in t.getmembers():
                if m.isfile():
                    f = t.extractfile(m)
                    head = f.read(4)
                    if head[:2] == b"MZ" or head == b"\x7fELF":
                        out[m.name.rsplit("/", 1)[-1]] = head + f.read()
    return out


def symbol_ids(symbols: Path) -> dict:
    """symbol file name -> the debug ids the archive has for it."""
    ids = {}
    with zipfile.ZipFile(symbols) as z:
        for name in z.namelist():
            parts = name.split("/")
            if len(parts) == 3 and parts[2].endswith(".sym"):
                ids.setdefault(parts[0], set()).add(parts[1])
    return ids


def check(binaries: dict, ids: dict) -> list:
    """The lines of a passing check; MismatchError naming every mismatch."""
    ok, bad = [], []
    for name, data in sorted(binaries.items()):
        sym = _symbol_name(name)
        if sym not in ids:
            continue
        got = pe_debug_id(data) or elf_debug_id(data)
        if got is None:
            bad.append("%s: no debug id could be read" % name)
        elif got in ids[sym]:
            ok.append("%s %s" % (name, got))
        else:
            bad.append("%s ships as %s, the symbols describe %s" % (name, got, ", ".join(sorted(ids[sym]))))
    if not any(l.split()[0] in ("xul.dll", "libxul.so") for l in ok) and not any(
            l.startswith(("xul.dll", "libxul.so")) for l in bad):
        bad.append("no xul in the package with symbols to check it against")
    if bad:
        raise MismatchError("; ".join(bad))
    return ok


def run(package: Path, symbols: Path) -> list:
    return check(package_binaries(package), symbol_ids(symbols))
