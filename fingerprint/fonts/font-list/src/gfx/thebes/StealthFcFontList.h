/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTHFCFONTLIST_H
#define GFX_STEALTHFCFONTLIST_H

// The fontconfig (Linux) half of the bundle-only font list. Each function is
// the body of one STEALTH-HOOK in gfxFcPlatformFontList.cpp, moved out of that
// file unchanged. The bundle itself (manifest, mapped files) is
// StealthBundleFontList.
//
// Hooks that stay inline in gfxFcPlatformFontList.cpp, because each is a
// condition on a Mozilla member and there is nothing to move. Their comments,
// which used to sit in that file, are here:
//
//  - AddPatternToFontList: `if (!aFontFamily) return;`
//
//    Stealth bundle-only: drop every pattern that arrives while aFontFamily is
//    still null. aFontFamily is null only while the first patterns of an
//    AddFontSetFamilies call carry an empty canonical family name:
//    lastFamilyName starts as "", so the block that assigns the family is
//    skipped (and so are faces 2..N of such a family, which take the
//    FcStrCmp==0 path and never re-enter that block). Upstream would go on to
//    AddOtherFamilyNames / MOZ_ASSERT(aFontFamily) and dereference null in
//    AddFontPattern.
//
//    The comment this guard used to carry said "a skipped family (above) left
//    aFontFamily null". That was false: nothing in AddPatternToFontList skips a
//    family (fontFamily starts null and is assigned whenever the canonical name
//    differs from lastFamilyName), so the sentence is replaced by the one
//    above.
//
//  - InitFontListForPlatform: `if (mStealthBundleOnly) return NS_OK;`
//
//    Stealth bundle-only: skip the host system-font enumeration entirely; only
//    the app-bundled set (processed just before the hook) populates the list,
//    so no host font enters.
//
//  - FindAndAddFamiliesLocked: `if (mStealthBundleOnly)` and the call to the
//    base class.
//
//    STEALTH: no host fontconfig in name resolution, at all.
//
//    Everything below the hook asks the HOST's fontconfig what a name means -
//    the "sans"/"mono" legacy aliases just after it, which exist on no Windows
//    machine, and then the whole -moz-sentinel FcConfigSubstitute machinery
//    further down, which resolves arbitrary names against /etc/fonts and
//    ~/.config/fontconfig. Information goes into the host config and comes back
//    out, which is exactly the shape this build exists to remove.
//
//    It is not a theoretical leak. A public fingerprinting test page reads the
//    operating system off the font system, and measured 2026-08-07 on the
//    shipped firefox-18 it reported `system_fonts: Segoe UI:Windows` on Windows
//    and `system_fonts: Sans:Linux` on Linux - it named the platform outright.
//    Probing the names directly: "Sans", "Serif", "Monospace" and "sans-serif"
//    all resolved as families on Linux and none of them on Windows. No host
//    font FILE leaked (DejaVu, Liberation, Ubuntu, Cantarell, Noto, FreeSans
//    all failed to resolve, so the bundle-only family list itself is sealed) -
//    what leaked was fontconfig's own alias NAMES, which no Windows install
//    has.
//
//    Delegating to the base class gives Windows' behaviour by construction: the
//    manifest's canonical substitute table (see
//    StealthBundleFontList::GetAlias, applied in
//    gfxPlatformFontList::FindAndAddFamiliesLocked) and then the bundle family
//    list, and nothing else. CSS generics do not depend on this path -
//    AddGenericFonts already forces the base class under bundle-only, so
//    serif/sans-serif/monospace/cursive resolve through
//    StealthDeclaredGenericFont (StealthBundleFontList.h, reached through
//    StealthPrependDeclaredGeneric in gfxPlatformFontList::
//    ResolveGenericFontNames).
//
//    (The text said `StealthGenericWindowsFont`, a name that exists nowhere in
//    the tree; StealthDeclaredGenericFont is the function that does what the
//    sentence describes.)
//
//  - AddGenericFonts: `usePrefFontList || mStealthBundleOnly`
//
//    when pref fonts exist, use standard pref font lookup. Stealth: also force
//    this path when bundle-only, so the CSS generics resolve via the base-class
//    ResolveGenericFontNames (StealthDeclaredGenericFont -> Windows fonts,
//    through StealthPrependDeclaredGeneric) instead of fontconfig, which would
//    map them to a host font we have dropped (making serif/monospace collapse
//    to Arial). (Same correction as above: the name used to be
//    `StealthGenericWindowsFont`.)

#include <stdint.h>

#include <string>

#include "SharedFontList.h"
#include "gfxFcPlatformFontList.h"
#include "mozilla/AlreadyAddRefed.h"

// Puts the file of a data-font face back into its pattern. Called by
// StealthCreateBundleFontEntry right after it builds the entry with the
// data-font constructor of gfxFontconfigFontEntry, which stays Mozilla's.
//
// STEALTH: `aFile`/`aIndex` sono il file da cui quella faccia viene, quando
// ce n'e' uno. Per un font scaricato da una pagina non c'e' e restano vuoti,
// che e' il comportamento upstream; per le facce del nostro bundle c'e', e
// dichiararlo evita che WebRender riceva i BYTE al posto del percorso. Vedi
// `70-known-bugs.md` [B154]: erano 65,9 MB nel processo padre.
void StealthDeclareFontFile(FcPattern* aPattern, const char* aFile,
                            int aIndex);

// The .ttc face index of aFace (face_index & 0x7FFF), 0 when there is no face.
uint32_t StealthTableFaceIndex(mozilla::gfx::SharedFTFace* aFace);

// gfxFontEntry::GetHBFace() (the base implementation, not the override) as a
// referenced hb_face_t.
hb_face_t* StealthBaseHBFaceRef(gfxFontEntry* aEntry);

// hb_face_reference_table on aEntry->GetHBFace() (virtual). Returns the table
// blob, possibly the empty blob, exactly as hb_face_reference_table does.
hb_blob_t* StealthHBFaceTable(gfxFontEntry* aEntry, uint32_t aTableTag);

// Writes zoom.stealth.text.freetype_hintstyle (FC_HINT_STYLE + FC_HINTING) and
// zoom.stealth.text.freetype_antialias (FC_ANTIALIAS) into the render pattern.
// Must run before PrepareFontOptions.
void StealthDeclareTextOptions(FcPattern* aPattern);

// Called in gfxFontconfigFontEntry::CreateFontInstance right after Mozilla's
// `if (!face->GetData())` block. When that block left aFile empty (a face made
// from data) and the PATTERN names a file (FC_FILE and FC_INDEX), sets aFile
// and aIndex from it. Otherwise touches nothing: a face from data with no file
// keeps the empty name, as upstream.
void StealthDeclaredFontFile(FcPattern* aPattern, std::string& aFile,
                             int& aIndex);

// Called in the same function right after mUnscaledFontCache.Add. When Mozilla
// built the path-less UnscaledFont (it chooses on GetData()) but aFile names
// the file, replaces aFont with one built on the path and adds that to the
// entry's cache. aFile is moved from then.
void StealthPreferFileUnscaledFont(
    gfxFontconfigFontEntry* aEntry,
    RefPtr<mozilla::gfx::UnscaledFontFontconfig>& aFont, std::string& aFile,
    int aIndex, mozilla::gfx::SharedFTFace* aFace);

// Bundle-only: builds the shared family list and, eagerly (Linux builds faces
// eagerly), the faces of every family from the manifest, so no fontconfig
// enumeration and no host font enters. Returns false when aList is null (the
// caller then continues as upstream). Calls StealthBundleFontList::Get() before
// the null test, as the code it replaces did.
bool StealthPopulateSharedList(mozilla::fontlist::FontList* aList);

// Bundle-only CreateFontEntry: instantiates the face from its bundle file via
// FreeType and wires it back to its shared-list face. Null when the file cannot
// be read or the face cannot be opened.
already_AddRefed<gfxFontEntry> StealthCreateBundleFontEntry(
    mozilla::fontlist::Face* aFace, const mozilla::fontlist::Family* aFamily,
    mozilla::fontlist::FontList* aList);

#endif  // GFX_STEALTHFCFONTLIST_H
