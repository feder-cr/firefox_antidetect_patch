#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Generates StealthSkiaMathTables.inc, the tables of the RCPPS and RSQRTPS
models that StealthSkiaMath.h computes in place of the instructions.

The input is the output of rcpps_model.c (invisible_skia_cpu study, captures/
canvas-blend/rcpps-by-cpu) or of rcpps_probe (invisible_D3D11Core, captures/cpu):
one text file per CPU vendor with the lines `rcp <8 words>` (the RCPPS table: the
output bits for 1.m, exponent 127, m = the k_rcp high mantissa bits) and `rsq <8
words>` (the RSQRTPS table: the 2^k_rsq words of the odd exponent 127, then those
of the even exponent 128), the checks of the model against the instruction
(`model_mismatch_* 0`) and, when the file prints them, the sha256 of the tables.
k is read from the number of words, per vendor and per instruction: k_rcp =
log2(RCPPS words), k_rsq = log2(RSQRTPS words / 2). Intel: 11 and 10 (2048 and
2 x 1024 words); AMD, from Zen 1 to Zen 5: 12 and 12 (4096 and 2 x 4096). The
files are kept in stealth_skia_math/:

  model_intel.txt   Intel Core Ultra 9 275HX (the 16 Intel CPUs of the measured
                    campaign, Westmere to Arrow Lake, share its sweeps)
  model_amd.txt     AMD Ryzen 9 7900X (Zen 4; the 5 AMD CPUs, Zen 1 to Zen 5,
                    share its sweeps), copied as it is from the probe's output

The script refuses a file whose model checks are not all 0, whose table sizes are
not powers of two (RSQRTPS: an even count of them), or whose printed table hashes
are not those of its tables. The sha256 of each table (little-endian words) and
the sweep hashes the file prints go into the comments of the .inc; the gtest pins
the sweeps, which are the instruction's.

Usage (standard library only):
  gen_stealth_skia_math.py             write StealthSkiaMathTables.inc from
                                       stealth_skia_math/model_{intel,amd}.txt
  gen_stealth_skia_math.py --check     fail if the .inc on disk is not what this
                                       script generates
  gen_stealth_skia_math.py --intel F --amd F   read other files
"""

import hashlib
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "stealth_skia_math")
INC = os.path.join(HERE, "StealthSkiaMathTables.inc")
WORDS_PER_LINE = 8


def fail(msg):
    sys.stderr.write("gen_stealth_skia_math.py: " + msg + chr(10))
    sys.exit(1)


def digest(words):
    return hashlib.sha256(b"".join(struct.pack("<I", w) for w in words)).hexdigest()


def log2_exact(n, what, path):
    if n < 2 or n & (n - 1):
        fail("%s: %s has %d words, not a power of two" % (path, what, n))
    return n.bit_length() - 1


def read_model(path):
    """Returns (info, rcp words, rsq words, k_rcp, k_rsq, rcp sha256, rsq sha256, sweeps).

    k is the number of high mantissa bits that decide the output: k_rcp = log2(number of RCPPS table words), k_rsq =
    log2(number of words of one parity of the RSQRTPS table, which holds two). Intel: 11 and 10. AMD (Zen 1 to 5): 12 and 12."""
    rcp, rsq, info, claimed, sweeps = [], [], {}, {}, {}
    rcp_lines = rsq_lines = 0
    with open(path, "r", encoding="ascii") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            m = re.match(r"^(rcp|rsq) ((?:[0-9a-f]{8} ?){%d})$" % WORDS_PER_LINE, line)
            if m:
                words = [int(w, 16) for w in m.group(2).split()]
                if m.group(1) == "rcp":
                    rcp += words
                    rcp_lines += 1
                else:
                    rsq += words
                    rsq_lines += 1
                continue
            if re.match(r"^(rcp|rsq)\s+[0-9a-f]", line):
                fail("%s: malformed table line: %r" % (path, line[:60]))
            key, _, value = line.partition(" ")
            if key in ("vendor", "family", "model", "stepping", "brand"):
                info[key] = value.strip()
            # `model_mismatch_rcp 0 first 00000000`, `model_mismatch_special 0`, `model_mismatch_rcp 0 model_mismatch_rsqrt 0`
            for mm in re.finditer(r"(model_mismatch_\w+) (\d+)", line):
                claimed[mm.group(1)] = int(mm.group(2))
            if key in ("sha256_rcp_table_2048_le", "sha256_rsqrt_table_2x1024_le"):
                claimed[key] = value.strip()
            if key in ("sha256_rcp_exp0_254", "sha256_rsqrt_exp0_254"):
                sweeps[key] = value.strip()
    if rcp_lines == 0 or rsq_lines == 0:
        fail("%s: no table lines" % path)
    k_rcp = log2_exact(len(rcp), "the RCPPS table", path)
    if len(rsq) % 2:
        fail("%s: the RSQRTPS table has an odd number of words" % path)
    k_rsq = log2_exact(len(rsq) // 2, "one parity of the RSQRTPS table", path)
    if not (8 <= k_rcp <= 16 and 8 <= k_rsq <= 16):
        fail("%s: k_rcp %d, k_rsq %d out of the range 8..16" % (path, k_rcp, k_rsq))
    for key in ("model_mismatch_rcp", "model_mismatch_rsqrt"):
        if claimed.get(key) != 0:
            fail("%s: %s is %r, expected 0 (the model did not reproduce the instruction)" % (path, key, claimed.get(key)))
    if claimed.get("model_mismatch_special", 0) != 0:
        fail("%s: model_mismatch_special is not 0" % path)
    rcp_sha, rsq_sha = digest(rcp), digest(rsq)
    for key, got in (("sha256_rcp_table_2048_le", rcp_sha), ("sha256_rsqrt_table_2x1024_le", rsq_sha)):
        if key in claimed and claimed[key] != got:
            fail("%s: the table hash the file prints (%s) is not that of its table" % (path, key))
    for w in rcp:
        if not 0x3e800000 <= w <= 0x3f800000 and w != 0:
            fail("%s: RCPPS table word %08x is not the reciprocal of a number in [1, 2)" % (path, w))
    return info, rcp, rsq, k_rcp, k_rsq, rcp_sha, rsq_sha, sweeps


def words_block(decl, words, per_line=8):
    out = ["const uint32_t %s = {" % decl]
    for i in range(0, len(words), per_line):
        out.append("    " + ", ".join("0x%08x" % w for w in words[i:i + per_line]) + ",")
    out.append("};")
    return "\n".join(out)


def render(intel, amd):
    out = ["""/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Generated by gen_stealth_skia_math.py from stealth_skia_math/model_*.txt; do not
// edit. Included by StealthSkiaMath.cpp inside namespace StealthSkiaMath.
//
// RCPPS: the output bits for 1.m with m = the k_rcp high mantissa bits, exponent 127
// (2^k_rcp words). RSQRTPS: [0] the output bits for exponent 127 (odd), then [1] for
// exponent 128 (even), the k_rsq high mantissa bits (2 x 2^k_rsq words, flat).
// k is a property of the vendor and of the instruction: Intel 11 and 10, AMD 12 and 12.
"""]
    for tag, data in (("Intel", intel), ("Amd", amd)):
        if data is None:
            out.append("// %s: no table yet (stealth_skia_math/model_%s.txt is absent); the mode is refused.\n" % (tag, tag.lower()))
            continue
        info, rcp, rsq, k_rcp, k_rsq, rcp_sha, rsq_sha, sweeps = data
        out.append("// %s: %s family %s model %s stepping %s, %s" % (tag, info.get("vendor"), info.get("family"), info.get("model"), info.get("stepping"), info.get("brand")))
        out.append("// k_rcp %d, k_rsq %d" % (k_rcp, k_rsq))
        out.append("// sha256 of the RCPPS table (%d words, little endian): %s" % (len(rcp), rcp_sha))
        out.append("// sha256 of the RSQRTPS table (2 x %d words, little endian): %s" % (len(rsq) // 2, rsq_sha))
        for key in sorted(sweeps):
            out.append("// the instruction's sweep, %s: %s" % (key, sweeps[key]))
        out.append(words_block("k%sRcp[%d]" % (tag, len(rcp)), rcp))
        out.append(words_block("k%sRsq[%d]" % (tag, len(rsq)), rsq))
        out.append("")
    out.append("const bool kIntelTablePresent = %s;" % ("true" if intel else "false"))
    out.append("const bool kAmdTablePresent = %s;" % ("true" if amd else "false"))
    out.append("// By mode (0 off, 1 Intel, 2 AMD): the tables, null where there is none, and their k (0 where there is none).")
    out.append("const uint32_t* const kRcpByMode[3] = {nullptr, %s, %s};" % ("kIntelRcp" if intel else "nullptr", "kAmdRcp" if amd else "nullptr"))
    out.append("const uint32_t* const kRsqByMode[3] = {nullptr, %s, %s};" % ("kIntelRsq" if intel else "nullptr", "kAmdRsq" if amd else "nullptr"))
    out.append("const int kRcpKByMode[3] = {0, %d, %d};" % (intel[3] if intel else 0, amd[3] if amd else 0))
    out.append("const int kRsqKByMode[3] = {0, %d, %d};" % (intel[4] if intel else 0, amd[4] if amd else 0))
    return "\n".join(out) + "\n"


def summary(intel, amd):
    one = lambda d: "%s k %d/%d" % (d[5][:8], d[3], d[4])
    return "Intel %s, %s" % (one(intel), "AMD " + one(amd) if amd else "no AMD table")


def main(argv):
    check = "--check" in argv
    paths = {"intel": os.path.join(DATA, "model_intel.txt"), "amd": os.path.join(DATA, "model_amd.txt")}
    args = [a for a in argv if a != "--check"]
    while args:
        flag = args.pop(0)
        if flag in ("--intel", "--amd") and args:
            paths[flag[2:]] = args.pop(0)
        else:
            fail("usage: gen_stealth_skia_math.py [--check] [--intel FILE] [--amd FILE]")
    if not os.path.exists(paths["intel"]):
        fail("the Intel table is missing: " + paths["intel"])
    intel = read_model(paths["intel"])
    amd = read_model(paths["amd"]) if os.path.exists(paths["amd"]) else None
    text = render(intel, amd)
    if check:
        with open(INC, "r", encoding="ascii", newline="") as f:
            if f.read() != text:
                fail("StealthSkiaMathTables.inc is not what this script generates from stealth_skia_math/")
        print("StealthSkiaMathTables.inc is up to date (%s)" % summary(intel, amd))
        return
    with open(INC, "w", encoding="ascii", newline="\n") as f:
        f.write(text)
    print("wrote %s: %s" % (INC, summary(intel, amd)))


if __name__ == "__main__":
    main(sys.argv[1:])
