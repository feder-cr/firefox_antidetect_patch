#!/usr/bin/env python3
"""Offline generator for browser/fonts/bundle-fonts.list.

Reads every bundled font file (.ttf / .ttc) under browser/fonts/ with
fontTools, expands .ttc collections into their individual faces, groups the
faces into families the same way the canonical Windows-11 font list does, and
emits a line-based manifest consumed at runtime by the (later, C++) uniform
font list builder. No OS font enumeration is involved - everything here is
derived straight from the font files themselves.

Manifest format (pipe-delimited, one record per line, ASCII, LF-terminated) -
chosen so the C++ reader is a trivial getline + split, safe at font-list-init:
  # <comment header>
  F|<family name>
  f|<file>|<index>|<w_min>|<w_max>|<stretch_min>|<stretch_max>|<style>|<psname>
    |<upem>|<ascent>|<descent>|<lineGap>|<xHeight>|<capHeight>
    |<underlineOffset>|<underlineSize>|<strikeoutOffset>|<strikeoutSize>
(the second line is a continuation of the same record - one physical line)
An `f` record belongs to the most recent `F` family. Family/PostScript names
never contain `|`, so the split is unambiguous.

Usage: python gen_bundle_font_manifest.py
Writes: browser/fonts/bundle-fonts.list
"""
from __future__ import annotations

import os
import re

from fontTools.ttLib import TTCollection, TTFont

# OS/2 usWidthClass (1-9) -> CSS font-stretch percentage.
WIDTH_CLASS_TO_PERCENT = {
    1: 50.0,
    2: 62.5,
    3: 75.0,
    4: 87.5,
    5: 100.0,
    6: 112.5,
    7: 125.0,
    8: 150.0,
    9: 200.0,
}

# Standard OpenType OS/2.usWeightClass names (the spec's own weight-class
# table). Used only to recognize a compound family name (nameID 1) that
# encodes its weight as a trailing word - e.g. "Franklin Gothic Medium" - so a
# WWS base family ("Franklin Gothic") can be derived for a face whose file
# carries no typographic-family (nameID 16) or WWS-family (nameID 21) record
# of its own.
WEIGHT_WORD_TO_CLASS = {
    "Thin": 100,
    "ExtraLight": 200,
    "UltraLight": 200,
    "Light": 300,
    "Medium": 500,
    "SemiBold": 600,
    "DemiBold": 600,
    "Demi": 600,
    "Bold": 700,
    "ExtraBold": 800,
    "UltraBold": 800,
    "Black": 900,
    "Heavy": 900,
}

# Legacy GDI-era compound family names that predate the WWS/typographic-family
# model. These files also carry a typographic-family (nameID 16) record
# pointing at a shorter base (e.g. "Arial Black" -> nameID16 "Arial").
#
# EMPTY since 2026-08-07, and the reason is worth keeping. It used to hold
# "Arial Black", on the stated premise that "the classic/CSS-visible Windows
# font list keeps the compound name as its own family". That premise was never
# checked and is FALSE for the thing that matters, which is not the Windows
# font list but what Firefox exposes to a page on Windows. Driven directly -
# retail Firefox 152.0.6, throwaway profile, no Playwright and no build of ours
# in the loop - "Arial Black" does NOT resolve as a family name, while "Arial"
# does. DWrite groups it as the 900 weight of Arial, so folding it into the
# typographic family is what reproduces a real browser.
#
# The test that found it needs TWO different fallbacks (`"X", monospace` and
# `"X", cursive`): a single monospace fallback calls Consolas unresolved,
# because Consolas IS the monospace fallback.
# [PROVATO E RITIRATO IL 2026-08-09.] Per qualche ora questo insieme ha
# contenuto "Arial Black", per emettere quella famiglia separatamente invece di
# ripiegarla. La misura che lo giustificava era presa contro
# C:/tmp/ff151-win2, che riporta 151.0 ma NON e' un retail: non firmato,
# ProductName "Nightly", IsPrivateBuild true. Contro un 151.0 scaricato da
# archive.mozilla.org e verificato Authenticode come firmato Mozilla, la
# risposta e' l'opposto: `font-weight:900` su Arial misura 277.5833, cioe' il
# retail USA Arial Black come faccia a peso 900 - che e' esattamente cio' che il
# ripiegamento riproduce, e che questa build faceva gia'. La riga sopra aveva
# ragione; il "difetto" era la build di confronto.
KEEP_GDI_NAME = set()

# nameIDs that carry copyright / trademark / manufacturer / license text -
# every genuine Windows-supplied font attributes itself to Microsoft in at
# least one of these (even third-party-designed ones, e.g. Lucida's copyright
# is Bigelow & Holmes but its license text says "Microsoft supplied font").
# Used to exclude non-Windows fonts Firefox bundles for its own purposes (e.g.
# the Mozilla-authored Twemoji Mozilla fallback emoji font), which carry none
# of this attribution at all.
_ATTRIBUTION_NAME_IDS = (0, 7, 8, 9, 13, 14)


def _is_microsoft_supplied(name_table) -> bool:
    for name_id in _ATTRIBUTION_NAME_IDS:
        value = name_table.getDebugName(name_id)
        if value and "Microsoft" in value:
            return True
    return False


def _iter_faces(fonts_dir):
    """Yield (file_name, face_index, TTFont) for every face of every bundled
    .ttf/.ttc under fonts_dir, expanding .ttc collections face by face."""
    for file_name in sorted(os.listdir(fonts_dir)):
        low = file_name.lower()
        path = os.path.join(fonts_dir, file_name)
        if low.endswith(".ttc"):
            coll = TTCollection(path, lazy=True)
            for index in range(len(coll.fonts)):
                yield file_name, index, TTFont(path, fontNumber=index, lazy=True)
        elif low.endswith(".ttf") or low.endswith(".otf"):
            yield file_name, 0, TTFont(path, lazy=True)


def _face_style(os2, head):
    italic = bool(os2.fsSelection & 0x0001) or bool(head.macStyle & 0x0002)
    return "italic" if italic else "normal"


def _weight_range(tt, os2):
    if "fvar" in tt:
        for axis in tt["fvar"].axes:
            if axis.axisTag == "wght":
                return [int(round(axis.minValue)), int(round(axis.maxValue))]
    w = int(os2.usWeightClass)
    return [w, w]


def _stretch_range(tt, os2):
    if "fvar" in tt:
        for axis in tt["fvar"].axes:
            if axis.axisTag == "wdth":
                return [float(axis.minValue), float(axis.maxValue)]
    pct = WIDTH_CLASS_TO_PERCENT.get(int(os2.usWidthClass), 100.0)
    return [pct, pct]


# OS/2 fsSelection bit 7: "use the sTypo* fields as the default line spacing".
USE_TYPO_METRICS = 1 << 7

# DirectWrite's grayscale glyph-coverage ladder - the alpha values a Windows
# Firefox actually produces when it rasterises text.
#
# WHY THIS EXISTS. The coverage of a glyph's antialiased edge is computed by the
# platform rasteriser, and the two disagree STRUCTURALLY, not marginally.
# Measured 2026-08-07 over 48 combinations (4 families x 6 sizes x 2 weights),
# black text on transparent so the alpha channel IS the coverage:
#
#     Windows / DirectWrite :   9-19 distinct alpha levels per render
#     Linux   / FreeType    : 193-256 distinct levels; 16 of the 48 used all 256
#
# So a detector does not need to compare a hash to tell the two apart - it can
# COUNT the levels. That is a far more robust tell than a hash mismatch, because
# two genuine Windows machines produce different hashes anyway (the value varies
# with the Windows build and the ClearType settings) while both produce a level
# COUNT in the low tens.
#
# THE VALUES. Union over the 48 Windows cases was 32 levels, but they are not 32
# independent ladders: 9 of these appear in all 48 renders and 8 more in about
# half, while the remaining 15 show up in 1-8 cases each. The 17 below are the
# ones present in >=10 of 48 - i.e. the ladder itself, with the rest being
# occasional extras. The step sizes narrow towards the top
# (18,17,18,17,17,17,17,17,15,16,16,15,15,15,13,12), which is the signature of
# gamma-corrected quantisation.
#
# This is MEASURED data, not derived from the font files, so unlike everything
# else in this generator it is a constant here rather than something read out of
# the fonts. It is shipped in the manifest so the runtime has one canonical copy
# and no platform computes it.
DWRITE_COVERAGE_LADDER = [
    0, 18, 35, 53, 70, 87, 104, 121, 138, 153, 169, 185, 200, 215, 230, 243, 255
]


def _vertical_metrics(tt, os2, head):
    """Vertical metrics in FONT UNITS, resolved the way DirectWrite resolves
    them - because the persona this build claims is Windows, so the numbers a
    real Windows Firefox produces are the correct target, not the font's raw
    tables.

    DWRITE_FONT_METRICS uses OS/2 usWinAscent/usWinDescent + hhea lineGap,
    except when fsSelection bit 7 (USE_TYPO_METRICS) is set, where it switches
    to the sTypo* trio. That rule was validated offline against the shipped
    Windows build with the canvas metrics override disabled: it reproduces
    DWrite's own output for 72/72 bundled families, including the four
    (Cambria Math, Gabriola, Sitka Small, Bahnschrift) where the raw usWin*
    numbers differ wildly - those are exactly the USE_TYPO_METRICS ones.

    Descent is stored POSITIVE (distance below the baseline), matching both
    usWinDescent and gfxFont::Metrics.

    Only the VERTICAL axis is carried. Measured 2026-08-07 cross-OS on the
    shipped binary: every width (advance, spaceWidth, zeroWidth) is already
    byte-identical because shaping is HarfBuzz in-tree, while ascent/descent
    diverged on 58 of 72 families, capHeight on 10 and xHeight on 5. Width
    fields here would be data nobody needs.
    """
    if getattr(os2, "fsSelection", 0) & USE_TYPO_METRICS:
        ascent = int(os2.sTypoAscender)
        descent = int(-os2.sTypoDescender)
    else:
        ascent = int(os2.usWinAscent)
        descent = int(os2.usWinDescent)

    # The LINE GAP is the "absorbed" one, not hhea.lineGap raw.
    #
    # Ascent/descent above were validated against the shipped Windows build for
    # 72/72 families; the gap never was, and taking hhea.lineGap directly was
    # simply wrong. It shows up in the LINE BOX rather than in ascent+descent,
    # so an inline-span probe cannot see it - which is why the parity gate
    # reported these families as matching while a position:absolute box did not.
    #
    # Measured 2026-08-07: line-height:normal per family on the real Windows
    # build, against every candidate rule. The classic "the win metrics already
    # absorbed the gap" formula wins:
    #
    #     hhea.lineGap                                      58/69
    #     OS/2.sTypoLineGap                                 20/69
    #     0                                                 52/69
    #     max(0, hhea.lineGap - (winAsc+winDesc            *63/69*
    #                            - (hheaAsc - hheaDesc)))
    #
    # A variant taking ascent/descent always from usWin* scores 64/69 on line
    # height but is rejected: it wrecks ascent/descent, e.g. Cambria Math 401px
    # against the 85px Windows actually produces. The pair below is the one that
    # holds BOTH axes.
    #
    # Six families still miss - Bahnschrift, Cambria Math, Gabriola, Segoe
    # Fluent Icons, Segoe MDL2 Assets, Sitka Small - all symbol/math/decorative
    # faces where our internal and external leading both come out 0, so layout
    # takes the emHeight*1.2 branch (ReflowInput's kNormalLineHeightFactor)
    # while Windows produces something else. Two of them land on exactly
    # 72*1.35 = 97.2, a non-integer that em+IL+EL cannot produce, so Windows is
    # on a path not yet identified. Documented rather than guessed at.
    hhea = tt["hhea"] if "hhea" in tt else None
    if getattr(os2, "fsSelection", 0) & USE_TYPO_METRICS:
        # The font asked for its typo metrics to be the line spacing, and
        # DirectWrite takes the trio WHOLE - gap included, not absorbed.
        # Measured on the four bundled faces that set the bit: the leading
        # Windows produces is exactly sTypoLineGap scaled, 15/13/51/19 px at
        # 72px for Bahnschrift, Cambria Math, Gabriola and Sitka Small, where
        # the absorbed formula gives 0 and layout then falls into the
        # emHeight*1.2 branch at 86.4px.
        line_gap = int(os2.sTypoLineGap)
    elif hhea is not None:
        absorbed = (int(os2.usWinAscent) + int(os2.usWinDescent)) - (
            int(hhea.ascender) - int(hhea.descender))
        line_gap = max(0, int(hhea.lineGap) - absorbed)
    else:
        line_gap = 0

    # sxHeight / sCapHeight exist only from OS/2 version 2. 0 means "absent",
    # and the C++ side derives a fallback the way the backends already do.
    has_v2 = int(getattr(os2, "version", 0)) >= 2
    x_height = int(getattr(os2, "sxHeight", 0) or 0) if has_v2 else 0
    cap_height = int(getattr(os2, "sCapHeight", 0) or 0) if has_v2 else 0

    post = tt["post"] if "post" in tt else None

    return {
        "upem": int(head.unitsPerEm),
        "ascent": ascent,
        "descent": descent,
        "lineGap": line_gap,
        "xHeight": x_height,
        "capHeight": cap_height,
        "underlineOffset": int(post.underlinePosition) if post else 0,
        "underlineSize": int(post.underlineThickness) if post else 0,
        "strikeoutOffset": int(getattr(os2, "yStrikeoutPosition", 0) or 0),
        "strikeoutSize": int(getattr(os2, "yStrikeoutSize", 0) or 0),
    }


def _parse_substitute_table(path: str, table_name: str) -> list[tuple[str, str]]:
    """Extract the {"alias", "target"} pairs of one C table, in file order.

    Parsed out of the C source rather than transcribed, because a second copy
    of a list is a copy that drifts - this repository already carries that
    exact defect in ci_font_gate.py's hand-copied EXPECTED_72, and its own docs
    flag it as a risk.
    """
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    start = src.index(table_name)
    end = src.index("};", start)
    body = src[start:end]
    pairs = re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', body)
    if not pairs:
        raise SystemExit(f"nessuna coppia estratta da {table_name} in {path}")
    return [(a, b) for a, b in pairs]


def build_aliases(gfx_dir: str) -> list[tuple[str, str]]:
    """The canonical Windows font-substitute table, for every OS.

    WHY THIS IS DATA AND NOT A LOOKUP. A font name that is not a family can
    still resolve, through Windows' substitute table, and a detector sees the
    difference: FingerprintJS probes `HELV` and `Small Fonts`, and on the
    shipped firefox-18 both resolved on Windows and neither on Linux, because
    gfxDWriteFontList reads them from the HOST REGISTRY
    (HKLM\\...\\FontSubstitutes) with no bundle-only gate, and Linux has no
    such registry. Measured 2026-08-07: `raw_fonts_n` 8 on Windows, 6 on Linux.

    Two things are wrong with the host registry as a source: it differs per OS
    (so the answer is not f(seed)), and it differs per WINDOWS INSTALL, since
    third-party software adds entries. Note also that bundle-only made this
    WORSE, not better: `gfx.windows-font-substitutes.always` defaults false,
    meaning "substitute only when the original family is absent", and with the
    family list cut to 72 no alias name is ever present, so every possible
    substitution registers.

    The sources here are Mozilla's own, both already in the tree:
      * kFontSubstitutes in StandardFonts-win10.inc - the curated
        standard-Windows-10 table Gecko already uses for resistFingerprinting,
        i.e. built for exactly this purpose;
      * sDirectWriteSubs in gfxDWriteFontList.cpp - the six Gecko applies
        unconditionally on Windows.

    Not filtered against the 72 bundled families on purpose: an alias whose
    target we do not bundle simply fails to resolve, and it fails the same way
    on every OS. Determinism is the property being bought, and filtering here
    would only move the decision somewhere less visible.
    """
    inc = os.path.join(gfx_dir, "StandardFonts-win10.inc")
    dwrite = os.path.join(gfx_dir, "gfxDWriteFontList.cpp")
    pairs = _parse_substitute_table(inc, "kFontSubstitutes[]")
    pairs += _parse_substitute_table(dwrite, "sDirectWriteSubs[]")

    seen, out = set(), []
    for alias, target in pairs:
        key = alias.lower()
        if key in seen or alias.lower() == target.lower():
            continue
        # The manifest is ASCII (the C++ reader is a getline + split on '|',
        # deliberately kept trivial because it runs at font-list init). The
        # only non-ASCII entries are the legacy Japanese GDI names, and every
        # one of them targets a CJK family this build does not bundle
        # ("MS Mincho"/"MS Gothic" in their full-width Japanese spelling), so
        # they could never resolve here. Dropping them changes no observable
        # behaviour; carrying them would mean widening the format for data
        # that resolves to nothing.
        if not alias.isascii() or not target.isascii():
            continue
        seen.add(key)
        out.append((alias, target))
    return sorted(out, key=lambda p: p[0].lower())


def _script_enum_values(intl_dir: str) -> dict[str, int]:
    """NAME -> numeric value of mozilla::intl::Script, read from the tree.

    The numeric value is the manifest key for the fallback table, because it is
    what the C++ side already has in hand (`int16_t(aRunScript)`) and needs no
    name lookup at all. UnicodeScriptCodes.h declares every value explicitly
    (`HAN = 17`), so this is a read, not an assumption about ordering.
    """
    path = os.path.join(intl_dir, "UnicodeScriptCodes.h")
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()
    start = src.index("enum class Script : int16_t {")
    end = src.index("};", start)
    out = {}
    for name, val in re.findall(r"(\w+)\s*=\s*(-?\d+)", src[start:end]):
        out[name] = int(val)
    if "HAN" not in out or "LATIN" not in out:
        raise SystemExit("UnicodeScriptCodes.h: enum Script non parsato")
    return out


def build_script_fallback(gfx_dir: str, intl_dir: str) -> dict:
    """The Windows common-fallback font lists, for every OS.

    WHY THIS IS DATA. `gfxPlatform::GetCommonFallbackFonts` is per-platform and
    the two implementations are not the same shape: Windows is a 93-case switch
    on the script plus a character-dependent tail, while the GTK one was a flat
    list with four Windows names prepended - described in its own patch as
    "the same order as gfxWindowsPlatform", which it never was. That is what
    differ. CORRECTED 2026-08-08: this used to cite a bare U+2764 measuring
    31.77 against 40.03 as the motive. Those are Arial's own aveCharWidth - the
    missing-glyph hex box - and both lists actually resolve U+2764 to Segoe UI
    Symbol. The table's real evidence is CJK: U+4E00 is covered by twelve
    bundled families, so a whole-list scan lands on MS Gothic while S|17 gives
    SimSun, as Windows does. Moving the NAMES here makes the choice a
    property of the file; the character CLASSIFICATION (colour presentation,
    Unicode block, general category) stays in shared C++, because it is Unicode
    and identical on every host - it was never the platform's font engine.

    Parsed from gfxWindowsPlatform.cpp, not transcribed, for the same reason as
    build_aliases(): a second copy of a 54-entry list is a copy that drifts.

    CONDITIONALS ARE FLATTENED. A few cases add a font under an `if` on the
    character (`if (aCh > 0xFFFF) SimSun-ExtB`). They are emitted
    unconditionally, which is safe HERE because CommonFontFallback only accepts
    a family that actually has a cmap entry for the character - a font offered
    to a character it does not cover is a no-op. It is not safe in general, so
    the count of flattened conditionals is returned and printed: if it grows,
    re-check that assumption instead of trusting this comment.
    """
    path = os.path.join(gfx_dir, "gfxWindowsPlatform.cpp")
    with open(path, "r", encoding="utf-8") as f:
        src = f.read()

    sig = "void gfxWindowsPlatform::GetCommonFallbackFonts("
    start = src.index(sig)
    # The function ends at the first closing brace in column 0 after it.
    end = src.index("\n}\n", start)
    body = src[start:end]

    sw = body.index("switch (aRunScript) {")
    head, rest = body[:sw], body[sw:]
    # Matching close of the switch: scan braces from the switch keyword.
    depth, cut = 0, None
    for i, ch in enumerate(rest):
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                cut = i
                break
    if cut is None:
        raise SystemExit("switch (aRunScript) non chiuso: parser da rivedere")
    switch_body, tail = rest[:cut], rest[cut + 1:]

    def _strip_comments(s: str) -> str:
        """Remove C comments BEFORE looking for AppendElement calls.

        The first version of this parser did not, and reported LATIN and COMMON
        as offering "Arial" because gfxWindowsPlatform.cpp carries

            // We always append Arial below, so no need to add it here.
            // aFontList.AppendElement("Arial");

        inside those cases. It is the defect CLAUDE.md names as the most
        repeated one in this project - a check written against the comment
        instead of the code - and it produced a table that looked plausible.
        """
        s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
        return "\n".join(ln for ln in s.splitlines()
                         if not ln.lstrip().startswith("//"))

    appends = lambda s: re.findall(r'AppendElement\("([^"]+)"\)',
                                   _strip_comments(s))

    # --- prefix: the colour-presentation emoji fonts -----------------------
    prefix = appends(head)
    if not prefix:
        raise SystemExit("prefisso PrefersColor non trovato")

    # --- per-script lists -------------------------------------------------
    values = _script_enum_values(intl_dir)
    per_script: dict[int, list[str]] = {}
    pending: list[str] = []
    fonts: list[str] = []
    flattened = 0
    for line in switch_body.splitlines():
        m = re.match(r"\s*case Script::(\w+):", line)
        if m:
            if fonts:  # a new label group after a block with no break
                pending = []
                fonts = []
            pending.append(m.group(1))
            continue
        got = appends(line)
        if got:
            fonts.extend(got)
        if re.search(r"\bif\s*\(", line):
            flattened += 1
        if "break;" in line:
            for name in pending:
                if name in values and fonts:
                    per_script[values[name]] = list(fonts)
            pending, fonts = [], []

    if len(per_script) < 30:
        raise SystemExit(
            f"solo {len(per_script)} script estratti: il parser non regge piu")

    # --- tail: Arial, then the symbol/punctuation block, then the rest -----
    # Order in the function is: Arial | if(symbolish){...} | Arial Unicode MS |
    # if(!PrefersColor){emoji}. Split on the conditional block so the C++ side
    # can reproduce the same order under the same condition.
    sym = re.search(r"if \(aRunScript == Script::COMMON.*?\{(.*?)\n  \}",
                    tail, re.S)
    if not sym:
        raise SystemExit("blocco simboli/punteggiatura non trovato nella coda")
    before = tail[:sym.start()]
    after = tail[sym.end():]
    return {
        "prefix": prefix,                       # P|
        "per_script": per_script,               # S|
        "tail_before": appends(before),         # T|
        "symbolish": appends(sym.group(1)),     # Y|
        "tail_after": [f for f in appends(after) if f not in prefix],  # Z|
        "flattened_conditionals": flattened,
    }


def build_manifest(fonts_dir: str, gfx_dir: str | None = None) -> dict:
    # The alias table belongs to the manifest, so ONE function produces the
    # whole file: building it separately in main() meant build_manifest() and
    # the file on disk disagreed, which the round-trip test caught.
    if gfx_dir is None:
        gfx_dir = os.path.normpath(
            os.path.join(fonts_dir, "..", "..", "gfx", "thebes"))
    families: dict[str, list[dict]] = {}
    # Legacy styled-face names: the GDI family name of a face that we file under
    # a DIFFERENT family. Gecko exposes exactly these as aliases - families whose
    # Face entries are already part of another family - and the shipped manifest
    # had none of them, so every one fell to the generic fallback.
    #
    # Measured 2026-08-16, one name per BROWSER against a certified retail 151.0
    # (one page cannot answer this: within a family only the FIRST name asked
    # resolves, because alias population is lazy, and three earlier measurements
    # were contaminated by exactly that). The retail resolves every legacy name
    # tried and lands on the BASE family's regular face, so the alias costs no
    # font file: `Arial Black` measures 231.3167, which is Arial; `Segoe UI
    # Light`, `Semilight`, `Semibold` and `Black` all measure 227.4833, which is
    # Segoe UI; `Franklin Gothic Medium` measures 223.2833, which is Franklin
    # Gothic. Only `Arial Bold` falls back on the retail too, and it is not a
    # Windows family name - bold is a face inside Arial.
    #
    # An alias resolves WITHOUT enumerating, which is the distinction the
    # earlier removal was missing: dropping the compound from the family list
    # was right (the family count is a tell), dropping it from resolution was
    # not.
    aliases_da_facce: dict[str, str] = {}

    def add_face(name, face):
        families.setdefault(name, []).append(face)

    for file_name, index, tt in _iter_faces(fonts_dir):
        if "name" not in tt or "OS/2" not in tt or "head" not in tt:
            continue

        name_table = tt["name"]
        gdi_name = name_table.getDebugName(1)
        if not gdi_name:
            continue
        if not _is_microsoft_supplied(name_table):
            continue

        # Regional-variant aliases (e.g. "MingLiU_HKSCS-ExtB") are internal
        # CJK supplementary-character-set faces Windows does not expose as
        # their own selectable family; no real Windows family name contains
        # an underscore.
        if "_" in gdi_name:
            continue
        # A "<Family> Variable" file is a Microsoft-internal build source for
        # already-bundled static weight instances of <Family> (e.g. "Segoe UI
        # Variable" backs the separately-bundled Segoe UI/Light/Semilight
        # static faces) - not itself a selectable Windows font family.
        if gdi_name.endswith(" Variable"):
            continue
        # System ICON fonts. Measured 2026-08-07 against retail Firefox
        # 152.0.6 on Windows: neither resolves as a family name for a page,
        # so exposing them answered "present" where a real browser answers
        # "absent". They carry glyphs for the shell UI, not text.
        if gdi_name in ("Segoe Fluent Icons", "Segoe MDL2 Assets"):
            continue

        typo_name = name_table.getDebugName(16)
        wws_name = name_table.getDebugName(21)
        psname = name_table.getDebugName(6) or ""

        os2 = tt["OS/2"]
        head = tt["head"]
        face = {
            "file": file_name,
            "index": index,
            "weight": _weight_range(tt, os2),
            "stretch": _stretch_range(tt, os2),
            "style": _face_style(os2, head),
            "psname": psname,
            "vmetrics": _vertical_metrics(tt, os2, head),
        }

        family = gdi_name
        if wws_name and wws_name != gdi_name:
            family = wws_name
        elif typo_name and typo_name != gdi_name and gdi_name not in KEEP_GDI_NAME:
            family = typo_name

        add_face(family, face)
        if family != gdi_name:
            aliases_da_facce[gdi_name] = family

        # WWS-base derivation: a compound name with no typographic-family record
        # of its own (Franklin Gothic Medium has neither nameID 16 nor 21) is
        # exposed under its weight-word-stripped base family INSTEAD of under
        # the compound.
        #
        # It used to be exposed under BOTH, on the claim that this matched the
        # real Windows font list. Measured 2026-08-07 against retail Firefox
        # 152.0.6 on Windows: "Franklin Gothic" resolves, "Franklin Gothic
        # Medium" does NOT. A page sees the base only, so exposing the compound
        # as well answered "present" where a real browser answers "absent" -
        # and font-name probing is exactly what FingerprintJS does.
        if family == gdi_name and not typo_name and not wws_name:
            words = gdi_name.split(" ")
            if (
                len(words) > 1
                and words[-1] in WEIGHT_WORD_TO_CLASS
                and gdi_name not in KEEP_GDI_NAME
            ):
                base = " ".join(words[:-1])
                if base:
                    families.pop(family, None)
                    add_face(base, dict(face))
                    aliases_da_facce[gdi_name] = base

    result_families = []
    for name in sorted(families):
        faces = sorted(families[name], key=lambda f: (f["file"].lower(), f["index"]))
        result_families.append({"name": name, "faces": faces})

    intl_dir = os.path.normpath(
        os.path.join(gfx_dir, "..", "..", "intl", "components", "src"))
    # Le due sorgenti si fondono qui, e la tabella di Mozilla VINCE su un nome
    # derivato dai file: se `kFontSubstitutes` dice gia' dove va un nome, quella
    # e' la risposta di Windows e la nostra derivazione e' solo un'inferenza.
    aliases = dict(aliases_da_facce)
    for alias, target in build_aliases(gfx_dir):
        aliases[alias] = target
    # ⛔ NON si filtrano gli alias il cui bersaglio non e' una famiglia che
    # dichiariamo. Nove ce ne sono (David, FangSong, Miriam, Fixedsys, KaiTi,
    # Rod, Mistral, System e Miriam Fixed) e sono INERTI: `GetAlias` restituisce
    # il nome bersaglio, il chiamante lo cerca fra le famiglie, non lo trova, e
    # cade sul ripiego - esattamente come se l'alias non ci fosse. Toglierli
    # sarebbe un cambiamento che non sposta nessuna misura, ed e' la categoria
    # che questo progetto rifiuta per regola. Restano, e il fatto che siano
    # inerti sta scritto in [B159].
    return {"families": result_families,
            "aliases": [(a, t) for a, t in sorted(aliases.items())],
            "script_fallback": build_script_fallback(gfx_dir, intl_dir)}


def emit_manifest(manifest: dict) -> str:
    """Serialize the manifest dict to the pipe-delimited line format the C++
    reader consumes. See the module docstring for the grammar."""
    lines = [
        "# bundle-fonts.list v3 - generated by scripts/gen_bundle_font_manifest.py; do not edit by hand",
        "# NOTE: the L| coverage ladder was removed 2026-08-08. It is not a property",
        "#   of any font FILE - it is what the rasteriser does to a glyph edge - so it",
        "#   is declared by invisible_core and delivered as a pref. Keeping it here as",
        "#   well would have been a second copy of a value with one owner.",
        "# A|alias|target  - canonical Windows font-substitute table, applied on EVERY OS.",
        "#   Replaces the per-host lookup: Windows read these from the registry",
        "#   (HKLM\\...\\FontSubstitutes) with no bundle-only gate, so the set varied by",
        "#   machine, and Linux/macOS had none at all. Source: Mozilla's own",
        "#   kFontSubstitutes (StandardFonts-win10.inc) + sDirectWriteSubs, parsed from",
        "#   the tree so there is no second copy to drift.",
        "# f|file|index|w_min|w_max|stretch_min|stretch_max|style|psname"
        "|upem|ascent|descent|lineGap|xHeight|capHeight"
        "|underlineOffset|underlineSize|strikeoutOffset|strikeoutSize",
        "# v2 adds the vertical metrics, in FONT UNITS (scale by size/upem).",
        "# They exist so no platform backend (DWrite/FreeType/CoreText) computes",
        "# them: each derived its own from the same file and they disagreed on 58",
        "# of 72 families. Descent is positive. See _vertical_metrics().",
    ]
    lines += [
        "# P|font|...        - common-fallback prefix used when the character asks for",
        "#   COLOUR presentation; the same list is appended LAST when it asks for",
        "#   monochrome, which is what gfxWindowsPlatform does.",
        "# S|<Script>|font|...- per-script common-fallback list, keyed by the NUMERIC",
        "#   value of mozilla::intl::Script (HAN=17, LATIN=25, ...), read from",
        "#   UnicodeScriptCodes.h so the key cannot drift from the enum.",
        "# T|font|...        - appended after the per-script list, always.",
        "# Y|font|...        - appended only for symbols/punctuation (the character",
        "#   test stays in shared C++: it is Unicode, not a host font engine).",
        "# Z|font|...        - appended after Y.",
        "#   All five come from gfxWindowsPlatform::GetCommonFallbackFonts, PARSED",
        "#   from the tree. They exist because that function is per-platform and the",
        "#   GTK one was a flat list with four Windows names in front - which is why",
        "#   U+4E00 resolves to MS Gothic without it and SimSun with it.",
    ]
    sf = manifest.get("script_fallback") or {}
    if sf:
        lines.append("P|" + "|".join(sf["prefix"]))
        lines.append("T|" + "|".join(sf["tail_before"]))
        lines.append("Y|" + "|".join(sf["symbolish"]))
        lines.append("Z|" + "|".join(sf["tail_after"]))
        for value in sorted(sf["per_script"]):
            lines.append("S|%d|%s" % (value, "|".join(sf["per_script"][value])))
    for alias, target in manifest.get("aliases", []):
        lines.append("A|%s|%s" % (alias, target))
    for fam in manifest["families"]:
        lines.append("F|" + fam["name"])
        for fc in fam["faces"]:
            w = fc["weight"]
            s = fc["stretch"]
            v = fc["vmetrics"]
            lines.append(
                "f|%s|%d|%d|%d|%g|%g|%s|%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d"
                % (fc["file"], fc["index"], w[0], w[1], s[0], s[1],
                   fc["style"], fc["psname"],
                   v["upem"], v["ascent"], v["descent"], v["lineGap"],
                   v["xHeight"], v["capHeight"],
                   v["underlineOffset"], v["underlineSize"],
                   v["strikeoutOffset"], v["strikeoutSize"])
            )
    return "\n".join(lines) + "\n"


def main():
    scripts_dir = os.path.dirname(os.path.abspath(__file__))
    fonts_dir = os.path.normpath(os.path.join(scripts_dir, "..", "browser", "fonts"))
    manifest = build_manifest(fonts_dir)
    out_path = os.path.join(fonts_dir, "bundle-fonts.list")
    with open(out_path, "w", encoding="ascii", newline="\n") as f:
        f.write(emit_manifest(manifest))
    print(f"wrote {out_path} ({len(manifest['families'])} families)")


if __name__ == "__main__":
    main()
