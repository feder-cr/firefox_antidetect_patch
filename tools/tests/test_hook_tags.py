"""A hook's STEALTH-HOOK tag names the feature the hook serves, not the folder that
happens to own the Mozilla file.

nsGlobalWindowOuter.cpp belongs to fingerprint/screen-window/geometry, and the two
lines in it that give a new realm its declared language and zone (the include of
StealthSessionDeclaration.h and the call in CreateNativeGlobalForInner) were tagged
with geometry. Someone looking for every place the language or zone is declared
greps the locale-time tags and misses them. These hooks must carry a
fingerprint/locale-time/* tag, wherever the file they sit in is owned.

    python -m pytest tools/tests
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import engine  # noqa: E402

# What a realm's language and zone hook looks like: the call, or the include it needs.
LOCALE_HOOK = re.compile(r"GetDeclaredRealm(Language|Timezone)\b|#\s*include\s*[\"<][\w/]*StealthSessionDeclaration\.h")
TAG = re.compile(r"STEALTH-HOOK\(([^)]+)\)")


def misplaced(patch_text):
    """(tag, line) for every realm language/zone hook whose tag is not locale-time.
    The tag is the one on the line, or the nearest added tag above it in the hunk."""
    bad, tag = [], None
    for line in patch_text.splitlines():
        if line.startswith("@@"):
            tag = None
            continue
        if not line.startswith("+") or line.startswith("+++"):
            continue
        found = TAG.search(line)
        if found:
            tag = found.group(1)
        if LOCALE_HOOK.search(line) and not (tag or "").startswith("fingerprint/locale-time/"):
            bad.append((tag, line))
    return bad


def test_every_realm_language_and_zone_hook_is_tagged_locale_time():
    bad = {p.relative_to(engine.ROOT).as_posix(): misplaced(p.read_text(encoding="utf-8", errors="replace"))
           for p in engine.ROOT.glob("**/hooks.patch")}
    assert {k: v for k, v in bad.items() if v} == {}


def test_the_geometry_tag_it_had_is_caught():
    old = (
        "@@ -35,6 +35,7 @@\n"
        "+#include \"mozilla/dom/StealthSessionDeclaration.h\"  // STEALTH-HOOK(fingerprint/screen-window/geometry)\n"
        "@@ -2095,8 +2097,8 @@ static nsresult CreateNativeGlobalForInner(\n"
        "+      // STEALTH-HOOK(fingerprint/screen-window/geometry)\n"
        "+      GetDeclaredRealmLanguage(aDocument), GetDeclaredRealmTimezone(aDocument));\n"
    )
    assert [t for t, _ in misplaced(old)] == ["fingerprint/screen-window/geometry"] * 2
    assert misplaced(old.replace("fingerprint/screen-window/geometry",
                                 "fingerprint/locale-time/session-declaration")) == []


def test_a_tag_does_not_leak_into_the_next_hunk():
    patch = (
        "@@ -1,1 +1,2 @@\n"
        "+// STEALTH-HOOK(fingerprint/locale-time/timezone)\n"
        "@@ -9,1 +10,2 @@\n"
        "+  GetDeclaredRealmTimezone(doc);\n"
    )
    assert misplaced(patch) == [(None, "+  GetDeclaredRealmTimezone(doc);")]
