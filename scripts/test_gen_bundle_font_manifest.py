# scripts/test_gen_bundle_font_manifest.py
"""gen_bundle_font_manifest.py, driven against a built tree.

RUN IT DIRECTLY: `python scripts/test_gen_bundle_font_manifest.py`. This is
NOT a pytest suite - it has no fixtures, so `pytest` collects it by name and
reports whatever it finds as a collection error. That reads exactly like a
broken gate, and on 2026-08-02 it produced one wrong conclusion about this
repository before the files were run the way they are built.

FIXED 2026-08-07: running it directly used to execute NOTHING. The file
defined `test_*` functions and had no `__main__` block, while the docstring
claimed it "asserts at import time" - it does not, and never did. So the
documented invocation exited 0 having checked nothing, which is the
indistinguishable-from-passing shape this project keeps finding. There is a
runner at the bottom now, and it prints what it checked.
"""
import os
import sys

from gen_bundle_font_manifest import build_manifest, emit_manifest

FONTS = os.path.normpath(os.path.join(os.path.dirname(__file__), "..",
                                      "browser", "fonts"))
MANIFEST = os.path.join(FONTS, "bundle-fonts.list")

# Le 68 famiglie del set Windows, i due font di ICONE e quella che Firefox imbarca di suo.
#
# "Twemoji Mozilla" non e' un font di Windows e per un anno e' stata esclusa
# apposta: il predicato del generatore chiedeva "viene da Microsoft?". La
# domanda che decide la realness e' un'altra, "un Firefox vero su Windows ce
# l'ha?", e su questo file le due risposte divergono. Aperti due retail 151
# firmati: la loro cartella fonts/ contiene UN file, ed e' questo.
#
# Misurato il 2026-08-16, un browser per caso, contro un retail certificato: con
# la famiglia non dichiarata una sequenza bandiera misurava 52,599998 (la somma
# di due glifi separati, quindi nessuna legatura), con lei dichiarata misura
# 72,0 come il giudice.
# ⛔ SECONDA COPIA, e sta in un ALTRO REPOSITORY: la stessa lista vive come
# `EXPECTED` in `scripts/ci_font_gate.py` DEL WRAPPER (invisible_playwright),
# non nell'albero Firefox. Il commento che stava qui diceva solo
# "verbatim from ci_font_gate.py" e il 2026-08-17 mi ha fatto concludere che il
# file non esistesse, perche' l'avevo cercato qui.
#
# La deriva che quel commento temeva e' REALE e si e' verificata quel giorno:
# questa lista e' passata a 71 famiglie e l'altra e' rimasta a 68, per mezz'ora,
# perche' stanno in due repository e nessun test le confronta.
EXPECTED_71 = {
    "Arial","Bahnschrift","Calibri","Cambria","Cambria Math","Candara",
    "Comic Sans MS","Consolas","Constantia","Corbel","Courier New","Ebrima","Franklin Gothic",
    "Gabriola","Gadugi","Georgia","Impact","Ink Free","Javanese Text",
    "Leelawadee","Leelawadee UI","Lucida Console","Lucida Sans Unicode","MS Gothic","MS PGothic",
    "MS UI Gothic","MV Boli","Malgun Gothic","Marlett","Microsoft Himalaya","Microsoft JhengHei",
    "Microsoft JhengHei UI","Microsoft New Tai Lue","Microsoft PhagsPa","Microsoft Sans Serif",
    "Microsoft Tai Le","Microsoft Uighur","Microsoft YaHei","Microsoft YaHei UI","Microsoft Yi Baiti",
    "MingLiU-ExtB","Mongolian Baiti","Myanmar Text","NSimSun","Nirmala UI","PMingLiU-ExtB",
    "Palatino Linotype","Segoe Print","Segoe Script",
    "Segoe UI","Segoe UI Emoji","Segoe UI Historic","Segoe UI Symbol","SimSun","SimSun-ExtB",
    "Sitka Small","Sylfaen","Symbol","Tahoma","Times New Roman","Trebuchet MS","Verdana",
    "Segoe Fluent Icons","Segoe MDL2 Assets","Twemoji Mozilla",
    "Webdings","Wingdings","Wingdings 2","Wingdings 3","Yu Gothic","Yu Gothic UI",
}

# v2 record: f|file|index|w_min|w_max|stretch_min|stretch_max|style|psname
#            |upem|ascent|descent|lineGap|xHeight|capHeight
#            |underlineOffset|underlineSize|strikeoutOffset|strikeoutSize
FACE_FIELDS = 19


def test_manifest_family_set_equals_the_71():
    names = {f["name"] for f in build_manifest(FONTS)["families"]}
    assert names == EXPECTED_71, f"missing={EXPECTED_71-names} extra={names-EXPECTED_71}"


def test_every_face_references_an_existing_file():
    for fam in build_manifest(FONTS)["families"]:
        assert fam["faces"], f"{fam['name']} has no faces"
        for fc in fam["faces"]:
            assert os.path.exists(os.path.join(FONTS, fc["file"]))
            assert isinstance(fc["index"], int) and fc["index"] >= 0


def test_checked_in_manifest_matches_the_generator():
    """The shipped file must be exactly what the generator produces.

    It is read at font-list init and its numbers now drive layout, so a hand
    edit or a stale copy after a font file changes would be invisible until it
    showed up as a rendering difference.
    """
    with open(MANIFEST, "r", encoding="ascii", newline="") as f:
        on_disk = f.read()
    regenerated = emit_manifest(build_manifest(FONTS))
    assert on_disk == regenerated, (
        "browser/fonts/bundle-fonts.list is not what the generator emits - "
        "re-run scripts/gen_bundle_font_manifest.py (never hand-edit it)")


def test_every_face_record_has_the_v2_field_count():
    with open(MANIFEST, "r", encoding="ascii") as f:
        for n, line in enumerate(f, 1):
            if line.startswith("f|"):
                got = len(line.rstrip("\n").split("|"))
                assert got == FACE_FIELDS, (
                    f"line {n}: {got} fields, expected {FACE_FIELDS}")


def test_vertical_metrics_are_present_and_sane():
    """Guards the numbers gfxFont::SanitizeMetrics imposes on all three
    backends. A zero upem would make every derived metric garbage; a
    non-positive ascent or a negative descent would mean the usWin*-vs-sTypo*
    resolution picked the wrong sign, which is the one mistake in
    _vertical_metrics() that would still produce plausible-looking output.
    """
    seen = 0
    for fam in build_manifest(FONTS)["families"]:
        for fc in fam["faces"]:
            v = fc["vmetrics"]
            where = f"{fam['name']} / {fc['file']}#{fc['index']}"
            assert 16 <= v["upem"] <= 16384, f"{where}: upem {v['upem']}"
            assert v["ascent"] > 0, f"{where}: ascent {v['ascent']}"
            assert v["descent"] >= 0, (
                f"{where}: descent {v['descent']} - it is stored POSITIVE")
            assert v["lineGap"] >= 0, f"{where}: lineGap {v['lineGap']}"
            # An em box more than 4x the design size means the wrong table.
            assert v["ascent"] + v["descent"] <= 4 * v["upem"], (
                f"{where}: ascent+descent {v['ascent'] + v['descent']} "
                f"vs upem {v['upem']}")
            assert 0 <= v["xHeight"] <= v["upem"], f"{where}: xHeight"
            assert 0 <= v["capHeight"] <= v["upem"], f"{where}: capHeight"
            assert v["underlineSize"] >= 0, f"{where}: underlineSize"
            seen += 1
    assert seen >= 100, f"only {seen} faces carried metrics"


def test_use_typo_metrics_actually_selects_a_different_source():
    """The USE_TYPO_METRICS branch is the whole reason the offline model
    reproduces DirectWrite. If no bundled font sets the bit, the branch has
    never executed and the agreement with Windows would be luck.
    """
    from fontTools.ttLib import TTCollection, TTFont
    from gen_bundle_font_manifest import USE_TYPO_METRICS, _iter_faces

    with_bit = []
    for file_name, index, tt in _iter_faces(FONTS):
        if "OS/2" not in tt:
            continue
        if getattr(tt["OS/2"], "fsSelection", 0) & USE_TYPO_METRICS:
            with_bit.append(f"{file_name}#{index}")
    assert with_bit, (
        "no bundled face sets USE_TYPO_METRICS, so that branch of "
        "_vertical_metrics() is dead code and untested")
    print(f"    USE_TYPO_METRICS set on {len(with_bit)} faces "
          f"(e.g. {with_bit[:3]})")


def test_alias_table_is_present_and_canonical():
    """The v3 alias table, which replaces the host registry on Windows and
    gives the other two platforms an alias set they never had.

    The two names FingerprintJS actually probes must be there: they are why
    this exists (raw_fonts_n was 8 on Windows and 6 on Linux, measured
    2026-08-07 on the shipped firefox-18).
    """
    from gen_bundle_font_manifest import build_aliases

    gfx = os.path.normpath(os.path.join(os.path.dirname(__file__), "..",
                                        "gfx", "thebes"))
    aliases = dict((a.lower(), t) for a, t in build_aliases(gfx))
    assert len(aliases) >= 40, f"solo {len(aliases)} alias"
    assert aliases.get("helv") == "Microsoft Sans Serif"
    assert aliases.get("small fonts") == "Arial"
    # An alias must never point at itself, or lookup would loop on the caller.
    for a, t in aliases.items():
        assert a != t.lower(), f"alias autoreferenziale: {a}"
    # ASCII only: the C++ reader is a getline + split kept deliberately trivial.
    for a, t in aliases.items():
        assert a.isascii() and t.isascii(), f"non-ASCII: {a} -> {t}"


def test_alias_records_round_trip_through_the_manifest():
    with open(MANIFEST, "r", encoding="ascii") as f:
        on_disk = [l.rstrip("\n") for l in f if l.startswith("A|")]
    assert on_disk, "nessun record A| nel manifest"
    for line in on_disk:
        parts = line.split("|")
        assert len(parts) == 3, f"record alias malformato: {line}"
        assert parts[1] and parts[2], f"campo vuoto: {line}"


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = []
    for t in tests:
        try:
            t()
            print(f"  [ok ] {t.__name__}")
        except AssertionError as exc:
            failed.append((t.__name__, exc))
            print(f"  [FAIL] {t.__name__}: {exc}")
        except Exception as exc:  # a broken test is not a pass either
            failed.append((t.__name__, exc))
            print(f"  [ERROR] {t.__name__}: {type(exc).__name__}: {exc}")
    print()
    if failed:
        print(f"{len(failed)}/{len(tests)} FALLITI")
        return 1
    print(f"tutti i {len(tests)} controlli passati")
    return 0


if __name__ == "__main__":
    sys.exit(main())
