# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

"""The Linux widget does not ask the toolkit for the CSS system fonts.

The system fonts are declared by invisible_core (ui.font.*). A GTK style
context read through Pango names the host, so no pango_* call
and no GetSystemFontInfo may come back in widget/gtk/nsLookAndFeel.cpp; the
engine ships no ui.font.* default either, because a default makes the
declaration gate unable to refuse a launch that lacks them.

Run: python widget/tests/test_stealth_no_host_fonts.py
"""

import os
import re
import sys

TREE = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))


def read(path):
    with open(os.path.join(TREE, path), encoding="utf-8") as f:
        return f.read()


def main():
    bad = []
    lf = read("widget/gtk/nsLookAndFeel.cpp")
    # With the engine on, GetSystemFontInfo returns before it reaches Pango:
    # the hook sits in the function, before its first pango_* call.
    start = lf.index("static void GetSystemFontInfo(")
    end = lf.index("bool nsLookAndFeel::NativeGetFont")
    func = lf[start:end]
    hook = func.find("StealthSystemFontsDeclared()")
    first_pango = func.find("pango_")
    if first_pango < 0:
        bad.append("GetSystemFontInfo has no pango_ call left to guard: the check is stale")
    elif hook < 0 or hook > first_pango:
        bad.append("GetSystemFontInfo reaches pango_* before the engine-on return")
    elif not re.search(r"StealthSystemFontsDeclared\(\)\)\s*(\{[^}]*)?return;", func):
        bad.append("the hook in GetSystemFontInfo does not return")
    # The four callers are Mozilla's own; none may call Pango outside the function.
    if re.search(r"pango_", lf[:start] + lf[end:]):
        bad.append("a pango_* call outside GetSystemFontInfo")
    native = lf[end : end + 400]
    if "StealthNativeSystemFont" not in native:
        bad.append("NativeGetFont no longer goes through StealthNativeSystemFont")
    if re.search(r'^pref\("ui\.font\.', read("modules/libpref/init/StealthPrefs.js"), re.M):
        bad.append("StealthPrefs.js declares a ui.font.* default")
    gate = read("gfx/thebes/StealthDeclarationGate.cpp")
    if "StealthFirstUndeclaredSystemFont" not in gate:
        bad.append("the declaration gate does not check ui.font.*")
    # The pref table must list every FontID pref, in order.
    xp = read("widget/nsXPLookAndFeel.cpp")
    block = xp[xp.index("sFontPrefs[][41]"):]
    theirs = re.findall(r'"(ui\.font\.[^"]+)"', block[: block.index("};")])
    ours_src = read("widget/StealthSystemFonts.cpp")
    ours = re.findall(r'"(ui\.font\.[^"]+)"', ours_src)
    if theirs != ours:
        bad.append("kStealthSystemFontPrefs differs from sFontPrefs: %r vs %r" % (ours, theirs))
    if "StealthFirstUndeclaredSystemColor" not in gate:
        bad.append("the declaration gate does not check the system colours")
    # The verification runs at every entry: the store fill, each NativeGet*,
    # the headless look and feel, and right after user.js is read.
    for path, needle in (
        ("widget/nsXPLookAndFeel.cpp", "StealthSystemThemeFillStores();"),
        ("widget/gtk/nsLookAndFeel.cpp", "StealthVerifySystemTheme();"),
        ("widget/headless/HeadlessLookAndFeelGTK.cpp", "StealthVerifySystemTheme();"),
        ("toolkit/xre/nsXREDirProvider.cpp", "StealthSystemThemeAfterUserPrefs();"),
    ):
        if needle not in read(path):
            bad.append("%s lost the hook %s" % (path, needle))
    if read("widget/headless/HeadlessLookAndFeelGTK.cpp").count("StealthVerifySystemTheme();") < 2:
        bad.append("the headless NativeGetColor and NativeGetFont both need the hook")
    if "ui.-moz-fieldtext" not in read("widget/StealthSystemFonts.cpp"):
        bad.append("Field and FieldText are not in the required colours")
    if '"Segoe UI"' in read("gfx/thebes/StealthBundleFontList.cpp").split("StealthSystemUIFamilies(nsTArray")[1].split("// The DECLARED")[0]:
        bad.append("system-ui has a compiled family: it follows ui.font.menu")
    if "StealthDarkSystemColor" not in read("widget/nsXPLookAndFeel.cpp"):
        bad.append("GetUncachedColor lost the dark HighlightText hook")
    for b in bad:
        print("FAIL:", b)
    if not bad:
        print("ok")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
