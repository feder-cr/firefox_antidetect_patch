# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""Every pref of ours is declared once.

Our prefs live in modules/libpref/init/StealthPrefs.js (defaults, included by
greprefs.js) and StealthPrefList.yaml (static prefs, a second input of the
generator). A Mozilla pref whose value we change is changed on Mozilla's own
line, marked STEALTH-HOOK; it is never declared a second time to win by load
order, which hides the real value from whoever reads Mozilla's file.

Run: python modules/libpref/test/test_stealth_prefs_declared_once.py
"""

import os
import re
import sys

TREE = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
OURS_JS = ["modules/libpref/init/StealthPrefs.js"]
OURS_YAML = ["modules/libpref/init/StealthPrefList.yaml"]
MOZILLA_JS = [
    "modules/libpref/init/all.js",
    "browser/app/profile/firefox.js",
    "browser/branding/official/pref/firefox-branding.js",
    "browser/branding/unofficial/pref/firefox-branding.js",
    "browser/branding/nightly/pref/firefox-branding.js",
    "browser/branding/aurora/pref/firefox-branding.js",
]
MOZILLA_YAML = ["modules/libpref/init/StaticPrefList.yaml"]

PREF_JS = re.compile(r'^\s*(?:pref|sticky_pref|lockPref)\(\s*"([^"]+)"', re.M)
PREF_YAML = re.compile(r"^- name:\s*(\S+)", re.M)


def names(rel, pattern):
    with open(os.path.join(TREE, rel), encoding="utf-8") as f:
        return pattern.findall(f.read())


def mozilla_names():
    out = {}
    for rel in MOZILLA_JS:
        if os.path.exists(os.path.join(TREE, rel)):
            for n in names(rel, PREF_JS):
                out.setdefault(n, rel)
    for rel in MOZILLA_YAML:
        for n in names(rel, PREF_YAML):
            out.setdefault(n, rel)
    return out


def ours():
    out = []
    for rel in OURS_JS:
        out += [(n, rel) for n in names(rel, PREF_JS)]
    for rel in OURS_YAML:
        out += [(n, rel) for n in names(rel, PREF_YAML)]
    return out


def problems(our, moz):
    seen = {}; bad = []
    for n, rel in our:
        if n in moz:
            bad.append(f"{n}: declared in {rel} and in Mozilla's {moz[n]}")
        if n in seen:
            bad.append(f"{n}: declared twice ({seen[n]}, {rel})")
        seen[n] = rel
    return bad


def test_parsers_read_the_files():
    # An empty scan would pass vacuously.
    moz = mozilla_names()
    assert len([n for n, r in moz.items() if r.endswith("all.js")]) > 1000, "all.js not read"
    assert len([n for n, r in moz.items() if r.endswith(".yaml")]) > 1000, "StaticPrefList.yaml not read"
    assert len(ours()) >= 50, "our pref files not read"


def test_every_pref_of_ours_is_declared_once():
    assert problems(ours(), mozilla_names()) == []


def test_the_check_catches_both_kinds_of_duplicate():
    moz = {"font.size.monospace.ar": "modules/libpref/init/all.js"}
    assert problems([("font.size.monospace.ar", "StealthPrefs.js")], moz)
    assert problems([("zoom.stealth.x", "a"), ("zoom.stealth.x", "b")], {})
    assert problems([("zoom.stealth.x", "a")], moz) == []


if __name__ == "__main__":
    failed = 0
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn(); print("ok  ", name)
            except AssertionError as e:
                failed += 1; print("FAIL", name, e)
    sys.exit(1 if failed else 0)
