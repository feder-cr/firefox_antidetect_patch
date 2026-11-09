# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""libxul's dynamic symbol table has no entry for the math it defines itself.

gfx/idwr/libm_ucrt.c gives libxul hidden definitions of sin, cos, tan, sinf, cosf,
tanf, powf, hypot, sincos, sincosf, pow, expf, logf, asinf, acosf, atanf and atan2f
(ucrtbase's results, outside Windows). Two ways to lose them, and both leave a dynamic symbol of one of these names:
- an undefined one, versioned GLIBC_*: some reference still binds to the host
  libm (the definition is missing, or LTO dropped it before code generation
  emitted a call to it, such as a sin and a cos merged into sincos);
- a defined one: libxul exports the name, which a probe of the process can see.

Reads the ELF file itself (.dynsym, .gnu.version, .gnu.version_r), no binutils.
Linux only; on a Windows or macOS build it has nothing to check.

Run: python gfx/idwr/tests/check_libxul_math.py <objdir>/dist/bin/libxul.so
Exit: 0 none of the names, 1 an entry of one of them, 2 the check cannot run (a file that
does not exist, is not ELF64 little-endian, or has no .dynsym: nothing would be checked).
"""

import struct
import sys


class Unreadable(Exception):
    """The file cannot be checked: the result would say nothing about it."""

NAMES = ("sin", "cos", "tan", "sinf", "cosf", "tanf", "powf", "hypot", "sincos", "sincosf",
         "pow", "expf", "logf", "asinf", "acosf", "atanf", "atan2f")


def dynamic_symbols(path):
    """[(name, defined, version)] of an ELF64 little-endian file's .dynsym."""
    try:
        b = open(path, "rb").read()
    except OSError as e:
        raise Unreadable("cannot read it (%s)" % e.strerror)
    if len(b) < 64 or b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 1:
        raise Unreadable("not an ELF64 little-endian file")
    shoff, = struct.unpack_from("<Q", b, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x3A)
    secs = [struct.unpack_from("<IIQQQQIIQQ", b, shoff + i * shentsize) for i in range(shnum)]
    # (name, type, flags, addr, offset, size, link, info, addralign, entsize)
    by_type = {}
    for s in secs:
        by_type.setdefault(s[1], s)
    dynsym = by_type.get(11)  # SHT_DYNSYM
    if dynsym is None:
        raise Unreadable("no .dynsym: not a dynamically linked library")
    strtab = secs[dynsym[6]]
    versym = by_type.get(0x6FFFFFFF)  # SHT_GNU_versym
    verneed = by_type.get(0x6FFFFFFE)  # SHT_GNU_verneed

    def cstr(sec, off):
        start = sec[4] + off
        return b[start:b.index(b"\0", start)].decode()

    need = {}  # version index -> name
    if verneed is not None:
        vstr = secs[verneed[6]]
        off = verneed[4]
        for _ in range(verneed[7]):
            _, cnt, _, aux, nxt = struct.unpack_from("<HHIII", b, off)
            a = off + aux
            for _ in range(cnt):
                _, _, other, name, anext = struct.unpack_from("<IHHII", b, a)
                need[other] = cstr(vstr, name)
                a += anext
            off += nxt
    out = []
    for i in range(dynsym[5] // 24):
        name, info, other, shndx, value, size = struct.unpack_from("<IBBHQQ", b, dynsym[4] + i * 24)
        ver = ""
        if versym is not None:
            ver = need.get(struct.unpack_from("<H", b, versym[4] + 2 * i)[0] & 0x7FFF, "")
        out.append((cstr(strtab, name), shndx != 0, ver))
    return out


def problems(path):
    bad = []
    for name, defined, ver in dynamic_symbols(path):
        if name not in NAMES:
            continue
        if defined:
            bad.append("%s is exported" % name)
        else:
            bad.append("%s is imported%s: a reference binds to the host libm" % (name, " (%s)" % ver if ver else ""))
    return bad


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    failed = unreadable = 0
    for path in argv[1:]:
        try:
            bad = problems(path)
        except (Unreadable, struct.error, ValueError, IndexError) as e:
            print("%s: CANNOT CHECK: %s" % (path, e))
            unreadable += 1
            continue
        for p in bad:
            print("%s: %s" % (path, p))
        print("%s: %s" % (path, "FAIL" if bad else "ok, none of %s in the dynamic symbol table" % ", ".join(NAMES)))
        failed += bool(bad)
    return 2 if unreadable else 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
