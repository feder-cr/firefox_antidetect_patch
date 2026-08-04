# scripts/test_gen_bundle_font_manifest.py
"""gen_bundle_font_manifest.py, driven against a built tree.

RUN IT DIRECTLY: `python scripts/test_gen_bundle_font_manifest.py`. This is
NOT a pytest suite - it has no fixtures and asserts at import time, so `pytest`
collects it by name and reports whatever it finds as a collection error. That
reads exactly like a broken gate, and on 2026-08-02 it produced one wrong
conclusion about this repository before the files were run the way they are
built.
"""
import json, os
from gen_bundle_font_manifest import build_manifest

EXPECTED_72 = {  # verbatim from ci_font_gate.py EXPECTED
    "Arial","Arial Black","Bahnschrift","Calibri","Cambria","Cambria Math","Candara",
    "Comic Sans MS","Consolas","Constantia","Corbel","Courier New","Ebrima","Franklin Gothic",
    "Franklin Gothic Medium","Gabriola","Gadugi","Georgia","Impact","Ink Free","Javanese Text",
    "Leelawadee","Leelawadee UI","Lucida Console","Lucida Sans Unicode","MS Gothic","MS PGothic",
    "MS UI Gothic","MV Boli","Malgun Gothic","Marlett","Microsoft Himalaya","Microsoft JhengHei",
    "Microsoft JhengHei UI","Microsoft New Tai Lue","Microsoft PhagsPa","Microsoft Sans Serif",
    "Microsoft Tai Le","Microsoft Uighur","Microsoft YaHei","Microsoft YaHei UI","Microsoft Yi Baiti",
    "MingLiU-ExtB","Mongolian Baiti","Myanmar Text","NSimSun","Nirmala UI","PMingLiU-ExtB",
    "Palatino Linotype","Segoe Fluent Icons","Segoe MDL2 Assets","Segoe Print","Segoe Script",
    "Segoe UI","Segoe UI Emoji","Segoe UI Historic","Segoe UI Symbol","SimSun","SimSun-ExtB",
    "Sitka Small","Sylfaen","Symbol","Tahoma","Times New Roman","Trebuchet MS","Verdana",
    "Webdings","Wingdings","Wingdings 2","Wingdings 3","Yu Gothic","Yu Gothic UI",
}

def test_manifest_family_set_equals_the_72():
    fonts = os.path.join(os.path.dirname(__file__), "..", "browser", "fonts")
    m = build_manifest(fonts)
    names = {f["name"] for f in m["families"]}
    assert names == EXPECTED_72, f"missing={EXPECTED_72-names} extra={names-EXPECTED_72}"

def test_every_face_references_an_existing_file():
    fonts = os.path.join(os.path.dirname(__file__), "..", "browser", "fonts")
    m = build_manifest(fonts)
    for fam in m["families"]:
        assert fam["faces"], f"{fam['name']} has no faces"
        for fc in fam["faces"]:
            assert os.path.exists(os.path.join(fonts, fc["file"]))
            assert isinstance(fc["index"], int) and fc["index"] >= 0
