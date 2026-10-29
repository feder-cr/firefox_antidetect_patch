/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The DirectWrite half of the uniform bundle-only font list.
//
// gfxDWriteFontList.cpp keeps only hooks (STEALTH-HOOK markers); what they
// decide, and why, is written here, one section per hook, in the order of that
// file. The comments are the ones the hooks carried inline.
//
// ---------------------------------------------------------------------------
// CreateFontInstance: the bundle path travels with the unscaled font.
// ---------------------------------------------------------------------------
// STEALTH: il percorso di bundle viaggia con l'unscaled font. Senza,
// `GetFontDescriptor` fallisce e Gecko manda a WebRender i BYTE del font;
// e non puo' ricavarselo da solo perche' il nostro caricatore non e' un
// `IDWriteLocalFontFileLoader`. Per ogni altro font resta vuoto e il
// comportamento upstream non si muove.
//
// The member it comes from (gfxDWriteFontEntry::mBundleFilePath) was declared
// in gfxDWriteFontList.h with this comment:
//
// STEALTH: il percorso del file di bundle, vuoto per ogni altro font.
//
// Il byte-serving verso WebRender passa da `UnscaledFontDWrite`, che il
// percorso non riesce a ricavarlo da solo: lo prende da un
// `IDWriteLocalFontFileLoader`, e il nostro caricatore presta byte in memoria.
// Dichiararlo qui e' l'unico modo di farlo arrivare la', ed evita che Gecko
// mandi i BYTE del font - 65,9 MB nel padre, misurati su Linux ([B154]).
//
// ---------------------------------------------------------------------------
// ReadCMAP and CreateFontFace.
// ---------------------------------------------------------------------------
// ReadCMAP asks gfxFontEntry::ShouldPublishCharacterMap(), which is true for a
// bundled face although it is built from file data. CreateFontFace passes
// mBundleFaceIndex instead of 0, so one face of a multi-face .ttc file is
// addressed by index; the member is 0 for every other font.
//
// ---------------------------------------------------------------------------
// ReadFaceNames.
// ---------------------------------------------------------------------------
// STEALTH [B317]: the bundle's faces are in no DirectWrite collection (their
// Index() is kNoIndex), so the lookup below can only fail for them.
// The base reads their names from the bundle file, on every platform.
//
// ---------------------------------------------------------------------------
// ReadFaceNamesForFamily.
// ---------------------------------------------------------------------------
// STEALTH [B317]: see ReadFaceNames above. Returning early here, as the
// collection lookup below did, left every bundled family without its
// src:local() names and its alternate family names.
//
// ---------------------------------------------------------------------------
// InitSharedFontListForPlatform: the family list.
// ---------------------------------------------------------------------------
// Uniform bundle: the family list is our manifest, not the DWrite collection
// enumeration (host fonts never enter). The collections above are still set up
// so mGDIInterop etc. stay valid; CreateFontEntry rasterizes each face
// straight from its bundle file. (The bundled collection is not enumerated
// either: see StealthBundleFontList::NeedsPlatformBundledCollection.)
//
// ---------------------------------------------------------------------------
// The substitute-table call sites (InitSharedFontListForPlatform has
// GetDirectWriteSubstitutes and GetFontSubstitutes; InitFontListForPlatform
// has the same two, apart).
// ---------------------------------------------------------------------------
// STEALTH: under bundle-only the aliases come from the manifest and are
// applied for every OS in gfxPlatformFontList::FindAndAddFamiliesLocked.
// These two build the platform's own table instead, and GetFontSubstitutes()
// reads it from the HOST REGISTRY (HKLM\...\FontSubstitutes) - so the set of
// resolvable alias names varied by machine (third-party software adds
// entries) and existed on no other OS. Measured 2026-08-07: "HELV" and
// "Small Fonts" resolved here and nowhere else, giving 8 fonts resolved on
// Windows against 6 on Linux.
//
// Note bundle-only made this WORSE rather than better: the pref
// gfx.windows-font-substitutes.always defaults false, i.e. "substitute only
// when the original family is absent", and with the family list cut to 72
// no alias name is ever present, so every possible substitution registered.
//
// Suppressed only when the manifest actually carries aliases; against a
// v1/v2 manifest the old behaviour stands, because removing the aliases
// outright would suppress a signal a real Windows Firefox does emit.
// (StealthBundleFontList::UsesHostSubstitutes decides it.)
//
// At the call sites in InitFontListForPlatform:
// STEALTH: see the note at the shared-list call site above - under
// bundle-only with a v3 manifest the aliases are ours, not the host's.
//
// ---------------------------------------------------------------------------
// InitFontListForPlatform: no host enumeration.
// ---------------------------------------------------------------------------
// Stealth bundle-only: skip the host system-font enumeration entirely; only
// the bundled collection (above) populates the list, so no host font enters.
//
// if no fonts found, something is out of whack, bail and use GDI backend
// (skip this when stealth bundle-only is active: we intentionally add no
// system fonts, so Count()==kBundledCount is the expected, correct state).

#include "gfxDWriteFontList.h"

#include <string>

#include "FontData.h"
#include "SharedFontList-impl.h"
#include "StealthBundleFontList.h"
#include "gfxDWriteCommon.h"
#include "gfxFontUtils.h"
#include "mozilla/RefPtr.h"
#include "nsString.h"

using namespace mozilla;

// Uniform bundle path: instantiate the face straight from its bundle file (the
// manifest family list drives everything; the DWrite collection is not
// consulted). Like MakePlatformFont, but keeps multi-face .ttc files and
// addresses the wanted face by index (mBundleFaceIndex).
//
// A member and not a free function because it writes protected members of
// gfxDWriteFontEntry (mBundleFaceIndex, mBundleFilePath, mFaceType) and calls
// its protected InitializeFrom, all reachable from gfxDWriteFontList through
// the friend declaration in gfxDWriteFontEntry.
already_AddRefed<gfxFontEntry> gfxDWriteFontList::CreateStealthBundleFontEntry(
    fontlist::Face* aFace, const fontlist::Family* aFamily) {
  auto* bundle = StealthBundleFontList::Get();  // never null; it crashes first
  const nsCString file(aFace->mDescriptor.AsString(SharedFontList()));
  // Stealth: ONE copy of the file - the bundle cache's - lent to every face.
  // This used to be three copies per instantiation (the cache's, a per-call
  // nsTArray, and a malloc+memcpy) of which the DWrite stream then kept a
  // fourth permanently, PER FACE. On the 35.4 MB .ttc with two faces that was
  // 106 MB resident where 35.4 is enough, and the bundle is 218.7 MB over 126
  // files. The borrow is safe because the cache is immortal by construction:
  // StealthBundleFontList is deliberately leaked and never clears it.
  RefPtr<StealthBundleFontList::FileBlob> blob = bundle->GetFaceBlob(file);
  if (!blob || !blob->Length()) {
    return nullptr;
  }
  // The FontData only lends the cache's bytes (it never frees them), and
  // Mozilla's own CreateCustomFontFile makes the stream that holds it; the
  // FreeType path in StealthFcFontList.cpp lends them the same way.
  RefPtr<FontData> lent =
      new FontData(blob->Data(), blob->Length(), FontData::Borrowed::Yes);
  RefPtr<gfxDWriteFontFileStream> stream;
  RefPtr<IDWriteFontFile> fontFile;
  HRESULT hr = gfxDWriteFontFileLoader::CreateCustomFontFile(
      lent, getter_AddRefs(fontFile), getter_AddRefs(stream));
  if (FAILED(hr)) {
    return nullptr;
  }
  nsAutoCString uniqueName;
  if (NS_FAILED(gfxFontUtils::MakeUniqueUserFontName(uniqueName))) {
    return nullptr;
  }
  RefPtr entry = MakeRefPtr<gfxDWriteFontEntry>(
      uniqueName, fontFile, stream, aFace->mWeight, aFace->mWidth,
      aFace->mStyle);
  entry->mBundleFaceIndex = aFace->mIndex;
  // Il percorso va dichiarato insieme ai byte: e' la stessa funzione che
  // risolve il blob qui sopra, quindi non e' una seconda fonte. Se manca si
  // prosegue: il font disegna, si paga solo la copia verso WebRender.
  nsAutoCString path;
  if (StealthBundleFontList::GetFacePath(file, path)) {
    // `get()` torna un `char16ptr_t`, che su Windows converte da solo a
    // `const wchar_t*` (`mfbt/Char16.h:49`): niente reinterpret_cast.
    entry->mBundleFilePath = std::wstring(NS_ConvertUTF8toUTF16(path).get());
  }
  BOOL supported;
  DWRITE_FONT_FILE_TYPE fileType;
  UINT32 numFaces;
  if (FAILED(fontFile->Analyze(&supported, &fileType, &entry->mFaceType,
                               &numFaces)) ||
      !supported || aFace->mIndex >= numFaces) {
    return nullptr;
  }
  // Wire the entry back to its shared-list face. This was the last of the
  // three bundle-only branches still omitting it: Linux was fixed 2026-08-07,
  // macOS already had it, and a comment in gfxFcPlatformFontList.cpp asserted
  // THIS one did too, citing a line number that is in the host-collection
  // branch of gfxDWriteFontList::CreateFontEntry (gfxDWriteFontList.cpp, not
  // this function). The comment was wrong and the consequence was invisible -
  // mShmemFace stayed null for every bundled face on Windows, so
  // GetStealthBundleVMetrics returned false and the manifest's vertical
  // metrics were never applied here at all.
  //
  // Nothing LOOKED broken, because Windows fell back to DirectWrite's own
  // metrics and the manifest was computed to reproduce exactly those: the
  // cross-OS parity gate passed 68/68 on numbers that came from two different
  // sources happening to agree. That is a coincidence held in place by the
  // generator being right, not a mechanism - had DWrite changed how it
  // derives any metric, Windows would have drifted from the file that claims
  // to define it and nothing would have said so.
  entry->InitializeFrom(aFace, aFamily);
  return entry.forget();
}
