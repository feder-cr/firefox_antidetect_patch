/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// NOTE (2026-07-06): DRAFT - written without a local build (macOS has no dev
// box; a full Firefox build is ~2h). The structure + the SharedFontList /
// WeightRange idioms are taken from confirmed in-tree signatures
// (SharedFontList.h Family/Face::InitData, CoreTextFontList.cpp weight/stretch
// construction). Validate + adjust at the first build: the file-read helper,
// fixed-pitch (manifest does not yet carry it), and the exact FontVisibility.

#include "StealthBundleFontList.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "SharedFontList-impl.h"
#include "gfxFont.h"
#include "gfxFontEntry.h"
#include "gfxPlatform.h"
#include "gfxPlatformFontList.h"
#include "nsMathUtils.h"
#include "gfxFontConstants.h"
#include "gfxFontUtils.h"
#include "mozilla/FontPropertyTypes.h"
#include "mozilla/gfx/StealthRefuse.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "prenv.h"
#include "nsIFile.h"
#include "nsIInputStream.h"
#include "nsIMemoryReporter.h"
#include "nsNetUtil.h"
#include "nsStreamUtils.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsReadableUtils.h"
#include "nsUnicharUtils.h"
#include "nsUnicodeProperties.h"
#include "nsXULAppAPI.h"

#ifdef XP_DARWIN
#  include <CoreFoundation/CoreFoundation.h>
#  include <CoreGraphics/CoreGraphics.h>
#  include <CoreText/CoreText.h>

#  include "AppleUtils.h"
#  include "CoreTextFontList.h"
#endif

// This is our uniform bundle-only font-list builder. It is listed under SOURCES,
// not UNIFIED_SOURCES, in gfx/thebes/moz.build: it defines a few short-named
// file-static helpers that could collide in a unified blob.

using namespace mozilla;
using mozilla::fontlist::Face;
using mozilla::fontlist::Family;

// ---------------------------------------------------------------------------
// .ttc support for gfxFontUtils (feature font-ttc-face-index).
//
// The hook in gfxFontUtils::FindTableDirEntry (gfxFontUtils.cpp) calls this to
// find the sfnt header of face aFaceIndex. Before it, the table lookup read
// only the file header, so only the first face of a collection was readable.
//
// Resolve the sfnt header for a given face within a buffer of font data. For
// single-face sfnt data, returns aFontData (aFaceIndex is ignored). For a .ttc
// collection, reads the TTC header's offset table and returns the pointer to
// the sfnt header of the requested face; returns null if aFaceIndex is out of
// range. Like the other gfxFontUtils table helpers, it assumes aFontData is a
// validated buffer: no length check is made here.
//
// Table offsets inside a face are always relative to the beginning of the
// file, even for faces within a .ttc collection. So GetTableFromFontData adds
// dir->offset to aFontData, never to the face offset returned here.
// ---------------------------------------------------------------------------
/* static */
const mozilla::SFNTHeader* gfxFontUtils::FindSfntOffset(const void* aFontData,
                                                       uint32_t aFaceIndex) {
  const auto* data = reinterpret_cast<const uint8_t*>(aFontData);
  const SFNTHeader* sfnt = reinterpret_cast<const SFNTHeader*>(data);
  if (sfnt->sfntVersion == 0x74746366u) {  // 'ttcf'
    const TTCHeader* ttc = reinterpret_cast<const TTCHeader*>(data);
    if (aFaceIndex >= uint32_t(ttc->numFonts)) {
      return nullptr;
    }
    // The offset table immediately follows the TTCHeader (12 bytes).
    const AutoSwap_PRUint32* offsets =
        reinterpret_cast<const AutoSwap_PRUint32*>(data + sizeof(TTCHeader));
    uint32_t faceOffset = offsets[aFaceIndex];
    return reinterpret_cast<const SFNTHeader*>(data + faceOffset);
  }
  return sfnt;
}

// Split aLine on '|' into aOut, keeping empty fields (a face's psname may be
// empty). Returns the field count.
static uint32_t SplitPipe(const nsACString& aLine,
                          nsTArray<nsCString>& aOut) {
  aOut.Clear();
  int32_t start = 0;
  const nsCString line(aLine);
  while (true) {
    int32_t bar = line.FindChar('|', start);
    if (bar < 0) {
      aOut.AppendElement(Substring(line, start));
      break;
    }
    aOut.AppendElement(Substring(line, start, bar - start));
    start = bar + 1;
  }
  return aOut.Length();
}

static int32_t ParseInt(const nsACString& aStr, int32_t aDefault) {
  nsresult rv;
  int32_t v = nsCString(aStr).ToInteger(&rv);
  return NS_FAILED(rv) ? aDefault : v;
}

static float ParseFloat(const nsACString& aStr, float aDefault) {
  nsresult rv;
  float v = nsCString(aStr).ToFloat(&rv);
  return NS_FAILED(rv) ? aDefault : v;
}

bool StealthBundleFontList::Load() {
  nsCString data;

  // Sources in order: env, then pref, then the packaged file.
  //
  // ENV FIRST, and not as a belt-and-braces - it is the only one that can
  // arrive in time. This runs inside InitSharedFontListForPlatform during
  // app startup, and on the Juggler path the caller's prefs are never written
  // into the profile at all: they travel over the wire and are applied by JS
  // once the browser has finished starting. So the pref below is always empty
  // here, which is exactly what was measured - removing a family from the
  // core's copy changed nothing because the packaged file kept winning. An
  // environment variable is set at process creation and inherited by every
  // content process, which is the same reason nr_stealth_bridge.cpp uses one.
  //
  // The env var carries a PATH, not the text: the manifest is ~20 KB and an
  // environment block is not the place for it.
  //
  // The packaged file stays as the FLOOR, deliberately. A browser launched
  // WITHOUT invisible_core - a direct run, an older core - has to render, and
  // the failure mode of an empty font list is not a subtle drift, it is a
  // browser with no fonts. This is a preference order, not a dependency.
  if (const char* envPath = PR_GetEnv("STEALTHFOX_FONT_MANIFEST")) {
    if (envPath[0]) {
      nsCOMPtr<nsIFile> envFile;
      if (NS_SUCCEEDED(NS_NewNativeLocalFile(nsDependentCString(envPath),
                                             getter_AddRefs(envFile)))) {
        nsCOMPtr<nsIInputStream> envStream;
        if (NS_SUCCEEDED(NS_NewLocalFileInputStream(getter_AddRefs(envStream),
                                                    envFile))) {
          nsCString envData;
          if (NS_SUCCEEDED(NS_ConsumeStream(envStream, UINT32_MAX, envData)) &&
              !envData.IsEmpty()) {
            data = envData;
          }
        }
      }
    }
  }

  if (data.IsEmpty()) {
    auto lock = mozilla::StaticPrefs::zoom_stealth_fonts_manifest();
    data = *lock;
  }

  if (data.IsEmpty()) {
    nsCOMPtr<nsIFile> file;
    if (NS_FAILED(NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(file)))) {
      return false;
    }
    if (NS_FAILED(file->Append(u"fonts"_ns)) ||
        NS_FAILED(file->Append(u"bundle-fonts.list"_ns))) {
      return false;
    }
    nsCOMPtr<nsIInputStream> stream;
    if (NS_FAILED(NS_NewLocalFileInputStream(getter_AddRefs(stream), file))) {
      return false;
    }
    if (NS_FAILED(NS_ConsumeStream(stream, UINT32_MAX, data))) {
      return false;
    }
  }

  nsCString currentKey;  // lowercased display name of the family being filled
  nsTArray<nsCString> fields;

  for (const auto& lineRange : data.Split('\n')) {
    nsDependentCSubstring line(lineRange);
    if (line.IsEmpty() || line[0] == '#') {
      continue;
    }
    if (StringBeginsWith(line, "F|"_ns)) {
      nsCString name(Substring(line, 2));
      nsAutoCString key(name);
      ToLowerCase(key);
      currentKey = key;
      mFamilies.AppendElement(Family::InitData(key, name, Family::kNoIndex,
                                               FontVisibility::Base));
      continue;
    }
    // NOTE: the L| coverage ladder used to be parsed here. It moved to
    // invisible_core and arrives as zoom.stealth.canvas.alpha_ladder, because
    // it is not a property of any font FILE - it is what the rasteriser does to
    // an edge. Nothing read mCoverageLadder after that move, so the member, the
    // accessor and this branch were dead, and the record in the manifest was a
    // second copy of a value the core already declares. Removed 2026-08-08.
    // P|T|Y|Z - the four fixed common-fallback lists (v4). Each is one line,
    // in gfxWindowsPlatform's order; see GetCommonFallback for how they compose.
    {
      nsTArray<nsCString>* dest = nullptr;
      if (StringBeginsWith(line, "P|"_ns)) {
        dest = &mFallbackPrefix;
      } else if (StringBeginsWith(line, "T|"_ns)) {
        dest = &mFallbackTail;
      } else if (StringBeginsWith(line, "Y|"_ns)) {
        dest = &mFallbackSymbolish;
      } else if (StringBeginsWith(line, "Z|"_ns)) {
        dest = &mFallbackTailAfter;
      }
      if (dest) {
        const uint32_t n = SplitPipe(line, fields);
        dest->Clear();
        for (uint32_t i = 1; i < n; ++i) {
          if (!fields[i].IsEmpty()) {
            dest->AppendElement(fields[i]);
          }
        }
        continue;
      }
    }
    if (StringBeginsWith(line, "S|"_ns)) {
      // S|<Script>|<font>|... - per-script common fallback (v4), keyed by the
      // NUMERIC mozilla::intl::Script value so no name table is needed on
      // either side. A script with no entry is normal: LATIN and COMMON carry
      // none, they are served by T|/Y|/Z| alone.
      const uint32_t n = SplitPipe(line, fields);
      if (n < 3) {
        continue;
      }
      const int32_t script = ParseInt(fields[1], -1);
      if (script < 0) {
        continue;
      }
      nsTArray<nsCString> fonts;
      for (uint32_t i = 2; i < n; ++i) {
        if (!fields[i].IsEmpty()) {
          fonts.AppendElement(fields[i]);
        }
      }
      if (!fonts.IsEmpty()) {
        mFallbackScript.InsertOrUpdate(uint32_t(script), std::move(fonts));
      }
      continue;
    }
    if (StringBeginsWith(line, "A|"_ns)) {
      // A|<alias>|<target family> - canonical Windows substitute table (v3).
      // Absent from a v1/v2 manifest, in which case HasAliases() is false and
      // the platform keeps whatever it did before.
      if (SplitPipe(line, fields) < 3) {
        continue;
      }
      nsAutoCString key(fields[1]);
      ToLowerCase(key);
      if (!key.IsEmpty() && !fields[2].IsEmpty()) {
        mAliases.InsertOrUpdate(key, fields[2]);
      }
      continue;
    }
    if (StringBeginsWith(line, "f|"_ns)) {
      // v1: f|<file>|<index>|<w_min>|<w_max>|<stretch_min>|<stretch_max>|<style>|<psname>
      // v2 appends the vertical metrics, in font units:
      //     |<upem>|<ascent>|<descent>|<lineGap>|<xHeight>|<capHeight>
      //     |<underlineOffset>|<underlineSize>|<strikeoutOffset>|<strikeoutSize>
      // A v1 manifest still parses: the metrics stay zeroed, IsValid() is
      // false, and gfxFont::SanitizeMetrics then leaves the backend's own
      // values alone rather than imposing zeros.
      const uint32_t nFields = SplitPipe(line, fields);
      if (nFields < 9 || currentKey.IsEmpty()) {
        continue;  // malformed / orphan face - skip, never crash
      }
      StealthBundleFace face;
      face.mFile = fields[1];
      face.mIndex = uint16_t(ParseInt(fields[2], 0));
      face.mWeightMin = ParseInt(fields[3], 400);
      face.mWeightMax = ParseInt(fields[4], face.mWeightMin);
      face.mStretchMin = ParseFloat(fields[5], 100.0f);
      face.mStretchMax = ParseFloat(fields[6], face.mStretchMin);
      face.mItalic = fields[7].EqualsLiteral("italic");
      face.mPsname = fields[8];
      if (nFields >= 19) {
        const int32_t upem = ParseInt(fields[9], 0);
        // A non-positive or oversized upem would make every derived metric
        // garbage; treat it as "no metrics" so the backend keeps its own.
        if (upem > 0 && upem <= UINT16_MAX) {
          face.mVMetrics.mUpem = uint16_t(upem);
          face.mVMetrics.mAscent = ParseInt(fields[10], 0);
          face.mVMetrics.mDescent = ParseInt(fields[11], 0);
          face.mVMetrics.mLineGap = ParseInt(fields[12], 0);
          face.mVMetrics.mXHeight = ParseInt(fields[13], 0);
          face.mVMetrics.mCapHeight = ParseInt(fields[14], 0);
          face.mVMetrics.mUnderlineOffset = ParseInt(fields[15], 0);
          face.mVMetrics.mUnderlineSize = ParseInt(fields[16], 0);
          face.mVMetrics.mStrikeoutOffset = ParseInt(fields[17], 0);
          face.mVMetrics.mStrikeoutSize = ParseInt(fields[18], 0);
        }
      }
      mFaces.LookupOrInsert(currentKey).AppendElement(std::move(face));
    }
  }

  return !mFamilies.IsEmpty();
}

void StealthBundleFontList::GetFaces(const nsACString& aFamilyName,
                                     nsTArray<Face::InitData>& aFaces) {
  nsAutoCString key(aFamilyName);
  ToLowerCase(key);
  auto entry = mFaces.Lookup(key);
  if (!entry) {
    return;
  }
  for (const auto& f : entry.Data()) {
    // Build a fresh (move-only) Face::InitData per call from the cached raw
    // primitives; aggregate-init every member in declaration order, with fresh
    // (move-only) range values.
    Face::InitData face = {
        f.mFile,   // mDescriptor = bundle file name
        f.mIndex,  // mIndex
#ifdef MOZ_WIDGET_GTK
        0,  // mSize (0 = scalable)
#endif
        false,  // mFixedPitch (manifest does not carry it yet)
        WeightRange(FontWeight::FromInt(f.mWeightMin),
                    FontWeight::FromInt(f.mWeightMax)),
        WidthRange(FontWidth::FromFloat(f.mStretchMin),
                   FontWidth::FromFloat(f.mStretchMax)),
        SlantStyleRange(f.mItalic ? FontSlantStyle::ITALIC
                                  : FontSlantStyle::NORMAL),
        nullptr,  // mCharMap
    };
    aFaces.AppendElement(std::move(face));
  }
}

StealthBundleFontList::FileBlob::~FileBlob() {
  if (mMap) {
    if (mMapped) {
      PR_MemUnmap(const_cast<uint8_t*>(mMapped), mMappedLength);
    }
    PR_CloseFileMap(mMap);
  }
}

size_t StealthBundleFontList::SizeOfIncludingThis(
    mozilla::MallocSizeOf aMallocSizeOf) const {
  size_t n = aMallocSizeOf(this);
  // Only what this process's HEAP holds. A mapped blob reports zero on purpose:
  // its pages belong to the file and are shared with every other process that
  // mapped it, so charging them here would count the same physical memory once
  // per process - which is exactly the inflation `resident` already has and USS
  // exists to avoid.
  for (const auto& blob : mFileCache.Values()) {
    if (blob) {
      n += sizeof(FileBlob) + blob->HeapBytes();
    }
  }
  return n;
}

size_t StealthBundleFontList::MappedBytes() const {
  size_t n = 0;
  for (const auto& blob : mFileCache.Values()) {
    if (blob) {
      n += blob->MappedBytes();
    }
  }
  return n;
}

// La sola funzione che sa dove stanno i file imbarcati. Prima la directory era
// ricostruita a mano in ogni chiamante che ne aveva bisogno; adesso chi vuole i
// byte e chi vuole il percorso passano di qui, quindi non possono divergere.
static already_AddRefed<nsIFile> PercorsoDelFile(const nsACString& aFile) {
  nsCOMPtr<nsIFile> path;
  if (NS_FAILED(NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(path))) ||
      NS_FAILED(path->Append(u"fonts"_ns)) ||
      NS_FAILED(path->Append(NS_ConvertUTF8toUTF16(nsCString(aFile))))) {
    return nullptr;
  }
  return path.forget();
}

bool StealthBundleFontList::GetFacePath(const nsACString& aFile,
                                        nsACString& aOut) {
  nsCOMPtr<nsIFile> path = PercorsoDelFile(aFile);
  if (!path) {
    return false;
  }
  // GetNativePath is XP_UNIX only - on Windows a native path is UTF-16 and the
  // method does not exist at all, so this does not compile without the branch.
  // The only caller today is the FreeType path, which is not built on Windows;
  // the function still has to COMPILE there, and a caller that appears later
  // must get a real answer rather than a stub.
#ifdef XP_WIN
  nsAutoString largo;
  if (NS_FAILED(path->GetPath(largo))) {
    return false;
  }
  CopyUTF16toUTF8(largo, aOut);
#else
  if (NS_FAILED(path->GetNativePath(aOut))) {
    return false;
  }
#endif
  return !aOut.IsEmpty();
}

already_AddRefed<StealthBundleFontList::FileBlob>
StealthBundleFontList::GetFaceBlob(const nsACString& aFile) {
  nsCString file(aFile);
  if (auto cached = mFileCache.Lookup(file)) {
    return do_AddRef(cached.Data());
  }
  nsCOMPtr<nsIFile> path = PercorsoDelFile(aFile);
  if (!path) {
    return nullptr;
  }
  // Map it read-only first. The pages are then the FILE's: shared by the OS
  // with every other process that maps it, and evictable instead of swappable.
  if (RefPtr<FileBlob> mapped = MapFile(path)) {
    mFileCache.InsertOrUpdate(file, mapped);
    return mapped.forget();
  }

  // Fallback: read it onto the heap. Same bytes, private pages. Reached only if
  // the mapping fails, which on a local install directory means something is
  // wrong with the machine rather than with the font.
  nsCOMPtr<nsIInputStream> stream;
  if (NS_FAILED(NS_NewLocalFileInputStream(getter_AddRefs(stream), path))) {
    return nullptr;
  }
  nsCString bytes;
  if (NS_FAILED(NS_ConsumeStream(stream, UINT32_MAX, bytes))) {
    return nullptr;
  }
  nsTArray<uint8_t> buf;
  buf.AppendElements(reinterpret_cast<const uint8_t*>(bytes.get()),
                     bytes.Length());
  RefPtr<FileBlob> blob = new FileBlob(std::move(buf));
  mFileCache.InsertOrUpdate(file, blob);
  return blob.forget();
}

already_AddRefed<StealthBundleFontList::FileBlob>
StealthBundleFontList::MapFile(nsIFile* aPath) {
  PRFileDesc* fd = nullptr;
  if (NS_FAILED(aPath->OpenNSPRFileDesc(PR_RDONLY, 0, &fd)) || !fd) {
    return nullptr;
  }
  PRFileInfo64 info;
  if (PR_GetOpenFileInfo64(fd, &info) != PR_SUCCESS || info.size <= 0 ||
      info.size > int64_t(UINT32_MAX)) {
    PR_Close(fd);
    return nullptr;
  }
  const uint32_t len = uint32_t(info.size);
  PRFileMap* map = PR_CreateFileMap(fd, info.size, PR_PROT_READONLY);
  if (!map) {
    PR_Close(fd);
    return nullptr;
  }
  void* addr = PR_MemMap(map, 0, len);
  // NSPR keeps what it needs from the descriptor; the mapping stays valid after
  // the file is closed, which is what lets the whole thing be fire-and-forget.
  PR_Close(fd);
  if (!addr) {
    PR_CloseFileMap(map);
    return nullptr;
  }
  return MakeAndAddRef<FileBlob>(map, addr, len);
}

// ⛔ `ReadFaceData` STAVA QUI e non c'e' piu', dal 2026-08-17. Copiava i byte del
// blob nell'array del chiamante, "on purpose: the caller hands ownership of the
// bytes to a platform object that frees them". Quella frase e' morta due volte:
// il ramo FreeType passo' a `GetFaceBlob` prestando, e il 2026-08-17 anche
// CoreText - dove peraltro non era mai stata vera, perche' `CFDataCreate` COPIA e
// l'array moriva a fine scope senza che nessuno ne prendesse possesso.
//
// Rimossa invece di lasciata senza chiamanti: era la seconda via per gli stessi
// byte, quella che copia, tenuta viva da una giustificazione scaduta. Lasciarla
// li' significa che il prossimo la ritrova e reintroduce la copia senza
// accorgersene, che e' esattamente cio' che la regola 16 vieta. Chi ha bisogno
// dei byte chiama `GetFaceBlob` e li presta; chi ha bisogno di POSSEDERLI ne
// faccia una copia sul posto e scriva perche'.

bool StealthBundleFontList::GetPsname(const nsACString& aFile, uint16_t aIndex,
                                      nsACString& aOut) {
  for (const auto& faces : mFaces.Values()) {
    for (const auto& f : faces) {
      if (f.mIndex == aIndex && f.mFile.Equals(aFile)) {
        aOut = f.mPsname;
        return !aOut.IsEmpty();
      }
    }
  }
  return false;
}

hb_blob_t* StealthBundleFontList::GetFaceNameTable(const nsACString& aFile,
                                                   uint16_t aIndex) {
  RefPtr<FileBlob> blob = GetFaceBlob(aFile);
  if (!blob || !blob->Length()) {
    return nullptr;
  }
  // The table points into the cached bytes, which outlive every caller: the
  // cache is never cleared (see the comment on GetFaceBlob's borrowers).
  return gfxFontUtils::GetTableFromFontData(
      blob->Data(), TRUETYPE_TAG('n', 'a', 'm', 'e'), aIndex);
}

bool StealthBundleFontList::GetVMetrics(const nsACString& aFile,
                                        uint16_t aIndex,
                                        StealthBundleVMetrics& aOut) {
  // Same linear walk as GetPsname above: 133 faces, and the caller only
  // reaches this for a bundled face (a web font has no shmem face), so the
  // miss case never pays for it.
  for (const auto& faces : mFaces.Values()) {
    for (const auto& f : faces) {
      if (f.mIndex == aIndex && f.mFile.Equals(aFile)) {
        if (!f.mVMetrics.IsValid()) {
          return false;  // v1 manifest: leave the backend's values alone
        }
        aOut = f.mVMetrics;
        return true;
      }
    }
  }
  return false;
}

// ---- Vertical metrics hooked from gfxFont (fingerprint/fonts/metrics) ----

// STEALTH: host-independent vertical metrics straight from the font's own OS/2
// sfnt table (FUnits->px), bypassing the platform backend (DWrite/FreeType/CoreText)
// whose computed asc/desc differ per OS. For a given bundled font + size the values
// are byte-identical on every host -> canvas TextMetrics vertical fields become
// host-independent (see CanvasRenderingContext2D::DrawOrMeasureText). emAscent/Descent
// from sTypoAscender/sTypoDescender, maxAscent/Descent from usWinAscent/usWinDescent
// (the Windows convention we always claim). Returns false if no usable OS/2 table.
bool gfxFont::GetStealthHostIndepVMetrics(gfxFloat& aEmAscent, gfxFloat& aEmDescent,
                                          gfxFloat& aMaxAscent,
                                          gfxFloat& aMaxDescent) {
  if (mFUnitsConvFactor < 0.0) {
    uint16_t unitsPerEm =
        GetFontEntry() ? GetFontEntry()->UnitsPerEm() : gfxFontEntry::kInvalidUPEM;
    if (unitsPerEm == gfxFontEntry::kInvalidUPEM || unitsPerEm == 0) {
      return false;
    }
    mFUnitsConvFactor = GetAdjustedSize() / unitsPerEm;
  }
  if (mFUnitsConvFactor <= 0.0) {
    return false;
  }
  gfxFontEntry::AutoTable os2Table(mFontEntry, TRUETYPE_TAG('O', 'S', '/', '2'));
  if (!os2Table) {
    return false;
  }
  uint32_t len;
  const OS2Table* os2 =
      reinterpret_cast<const OS2Table*>(hb_blob_get_data(os2Table, &len));
  if (len < offsetof(OS2Table, usWinDescent) + sizeof(uint16_t)) {
    return false;
  }
  aEmAscent = int16_t(os2->sTypoAscender) * mFUnitsConvFactor;
  aEmDescent = -int16_t(os2->sTypoDescender) * mFUnitsConvFactor;  // sTypoDescender < 0
  aMaxAscent = uint16_t(os2->usWinAscent) * mFUnitsConvFactor;
  aMaxDescent = uint16_t(os2->usWinDescent) * mFUnitsConvFactor;
  return true;
}

// STEALTH: resolve this font entry's manifest vertical metrics, if it is one
// of the bundled faces. False for everything else - a web/user font, a font
// outside the shared list, or a v1 manifest with no metrics - and in that case
// the backend's own values must stand untouched.
static bool GetStealthBundleVMetrics(gfxFontEntry* aFontEntry,
                                     StealthBundleVMetrics& aOut) {
  // mShmemFace is what identifies a bundled face, so it is also the reason
  // this returns false for a web font. Note it is only non-null because
  // every backend's CreateFontEntry calls InitializeFrom - the Linux
  // bundle-only branch did NOT until 2026-08-07, and while that was true this
  // whole function silently no-opped on Linux, which is precisely the platform
  // the metrics were meant to correct.
  if (!aFontEntry) {
    return false;
  }
  // Deliberately NOT gated on IsUserFont(). On Linux the bundle-only branch of
  // gfxFcPlatformFontList::CreateFontEntry loads each face from an in-memory
  // buffer through FTUserFontData - the same machinery a web font uses - so
  // every bundled face reports IsUserFont() == true. An earlier version of this
  // guard rejected all 72 of them for that reason, on the one platform the
  // metrics exist to correct.
  //
  // mShmemFace alone is the right discriminator: it means "this face came from
  // the shared font list", and a genuine web font never has one - gfxUserFontSet
  // explicitly clears it (gfxUserFontSet.cpp:1117). Even if one slipped through,
  // the manifest lookup below is keyed on (bundle file, face index) and would
  // simply miss.
  if (!aFontEntry->mShmemFace) {
    return false;
  }
  auto* pfl = gfxPlatformFontList::PlatformFontList(false);
  if (!pfl) {
    return false;
  }
  mozilla::fontlist::FontList* sharedList = pfl->SharedFontList();
  if (!sharedList) {
    return false;
  }
  auto* bundle = StealthBundleFontList::Get();  // never null; it crashes first
  // A Face's mDescriptor is the bundle file name and mIndex the face within
  // it - the same pair every backend's CreateFontEntry uses to load the file.
  const nsCString file =
      aFontEntry->mShmemFace->mDescriptor.AsString(sharedList);
  return bundle->GetVMetrics(file, aFontEntry->mShmemFace->mIndex, aOut);
}

// STEALTH: impose the manifest's vertical metrics, so that no platform
// backend decides them.
//
// This is the single choke point where DWrite, FreeType and CoreText meet:
// all three call gfxFont::SanitizeMetrics as the last step of populating their own
// mMetrics (gfxDWriteFonts.cpp, gfxFT2FontBase.cpp, gfxMacFont.cpp), and it
// runs inside font init, so it is upstream of both gfxFontCache and the lazy
// mVerticalMetrics cache - no per-query cost, no const-ness problem.
//
// Why: each backend derived ascent/descent from the SAME bundled file by its
// own rule, and they disagreed. Measured 2026-08-07 on the shipped
// firefox-18, Windows vs Linux, same seed and same build: ascent/descent
// differed on 58 of 72 families (up to 27px at a 72px size), line-box height
// on 61, capHeight on 10, xHeight on 5 - while every advance width already
// matched, because shaping is HarfBuzz in-tree. That divergence is a tell:
// it is what makes a Linux-hosted session's font metrics disagree with the
// Windows persona this build claims everywhere else.
//
// The values come from bundle-fonts.list v2, resolved offline to what a real
// Windows Firefox produces (DirectWrite's usWin*-vs-sTypo* rule) and
// validated against the shipped Windows build for 72/72 families. The
// arithmetic mirrors gfxDWriteFonts.cpp:396-421 on purpose, so Windows keeps
// the numbers it already had and the other two are brought TO it, instead of
// all three moving somewhere new.
//
// Called from gfxFont::SanitizeMetrics, right after its zero-size early
// return, with that font's entry and adjusted size.
void StealthImposeBundleVMetrics(gfxFontEntry* aFontEntry,
                                 gfxFloat aAdjustedSize,
                                 gfxFont::Metrics* aMetrics) {
  MOZ_ASSERT(aMetrics);
  StealthBundleVMetrics vm;
  if (GetStealthBundleVMetrics(aFontEntry, vm)) {
    const gfxFloat size = aAdjustedSize;
    const gfxFloat conv = size / gfxFloat(vm.mUpem);

    aMetrics->maxAscent = NS_round(vm.mAscent * conv);
    aMetrics->maxDescent = NS_round(vm.mDescent * conv);
    aMetrics->maxHeight = aMetrics->maxAscent + aMetrics->maxDescent;
    aMetrics->emHeight = size;
    // Keep emAscent + emDescent == emHeight: gfxHarfBuzzShaper reads both to
    // place the vertical VOrg and the glyph y_bearing, so breaking that
    // invariant would move glyph bounding boxes, not just line boxes.
    aMetrics->emAscent =
        aMetrics->maxHeight > 0.0
            ? aMetrics->emHeight * aMetrics->maxAscent / aMetrics->maxHeight
            : aMetrics->emHeight;
    aMetrics->emDescent = aMetrics->emHeight - aMetrics->emAscent;
    aMetrics->internalLeading =
        std::max(aMetrics->maxHeight - aMetrics->emHeight, 0.0);
    aMetrics->externalLeading = std::ceil(vm.mLineGap * conv);

    // xHeight / capHeight are absent from a pre-v2 OS/2 table; when the
    // manifest carries 0 keep whatever the backend derived, rather than
    // zeroing a real value.
    if (vm.mXHeight) {
      aMetrics->xHeight = vm.mXHeight * conv;
    }
    if (vm.mCapHeight) {
      aMetrics->capHeight = vm.mCapHeight * conv;
    }
    if (vm.mUnderlineSize) {
      aMetrics->underlineOffset = vm.mUnderlineOffset * conv;
      aMetrics->underlineSize = vm.mUnderlineSize * conv;
    }
    if (vm.mStrikeoutSize) {
      aMetrics->strikeoutOffset = vm.mStrikeoutOffset * conv;
      aMetrics->strikeoutSize = vm.mStrikeoutSize * conv;
    }
    // The clamps in gfxFont::SanitizeMetrics after the call site
    // (underlineSize >= 1, underlineOffset <= -1, maxAscent >= 1, ...) still
    // run, exactly as they do for backend values.
  }
}

bool StealthBundleFontList::GetAlias(const nsACString& aName,
                                     nsACString& aOut) {
  nsAutoCString key(aName);
  ToLowerCase(key);
  if (auto entry = mAliases.Lookup(key)) {
    aOut = entry.Data();
    return true;
  }
  return false;
}

bool StealthBundleFontList::GetCommonFallback(
    int16_t aScript, bool aPrefersColor, bool aSymbolish,
    nsTArray<const char*>& aOut) const {
  if (mFallbackTail.IsEmpty()) {
    return false;  // pre-v4 manifest: let the caller keep the platform list
  }
  auto append = [&aOut](const nsTArray<nsCString>& aList) {
    for (const nsCString& name : aList) {
      aOut.AppendElement(name.get());
    }
  };
  // gfxWindowsPlatform's exact order. The colour-emoji fonts lead when the
  // character asks for colour and trail when it does not - upstream's comment
  // is that trailing keeps them preferred over user-installed broken fonts in
  // the global path, and reproducing the ORDER is the point: a detector reads
  // which font won, not which list we consulted.
  if (aPrefersColor) {
    append(mFallbackPrefix);
  }
  if (auto entry = mFallbackScript.Lookup(uint32_t(aScript))) {
    append(entry.Data());
  }
  append(mFallbackTail);
  if (aSymbolish) {
    append(mFallbackSymbolish);
  }
  append(mFallbackTailAfter);
  if (!aPrefersColor) {
    append(mFallbackPrefix);
  }
  return true;
}

// Called by gfxPlatformGtk::GetCommonFallbackFonts before it appends the
// upstream Linux names; "below" in the comment means "after these names".
void StealthAppendWindowsFallbackNames(bool aPrefersColor,
                                       nsTArray<const char*>& aFontList) {
  // Stealth: the bundled Windows faces come FIRST, mirroring the order
  // gfxWindowsPlatform::GetCommonFallbackFonts uses, so a codepoint no
  // requested family covers resolves to the same face on every host.
  //
  // Every name in the upstream Linux list below - Twemoji Mozilla, DejaVu,
  // FreeSerif/Sans, Symbola, Noto Sans Symbols 1/2 - is absent from
  // bundle-fonts.list, so under bundle-only this whole function was a
  // guaranteed miss for EVERY codepoint, on every script, not just emoji.
  // (Twemoji Mozilla ships in browser/fonts but is excluded from the manifest
  // by the Microsoft-attribution filter and is registered as a family on
  // neither OS, so naming it here has never resolved.) The upstream names are
  // kept after ours: they cost nothing when absent and keep the non-bundle
  // build working.
  // The ORDER mirrors the leading names of
  // gfxWindowsPlatform::GetCommonFallbackFonts -
  // Segoe UI Emoji, then Arial, Segoe UI, Segoe UI Symbol, Cambria Math -
  // because the order decides WHICH face covers a codepoint, and a different
  // face means a different advance. Measured 2026-08-07: listing Segoe UI
  // Symbol before Arial (an earlier version of this block) left five emoji
  // widths apart from Windows even though every family already resolved.
  if (aPrefersColor) {
    aFontList.AppendElement("Segoe UI Emoji");
  }
  aFontList.AppendElement("Arial");
  aFontList.AppendElement("Segoe UI");
  aFontList.AppendElement("Segoe UI Symbol");
  aFontList.AppendElement("Cambria Math");
}

/* static */
StealthBundleFontList* StealthBundleFontList::Get() {
  static StealthBundleFontList* sInstance = []() -> StealthBundleFontList* {
    auto* list = new StealthBundleFontList();
    if (list->Load()) {
      return list;  // leaked on purpose: lives for the process
    }
    delete list;
    // NO SILENT FALLBACK. This used to return nullptr, and every call site is
    // written `if (auto* bundle = Get())` - so one failure here made the WHOLE
    // font declaration revert to the host's own fonts without a word:
    // enumeration, vertical metrics, aliases, the per-script fallback table and
    // the generic families, twelve sites across the three platform back ends.
    // That is the worst failure mode this build has, because mStealthBundleOnly
    // is on unconditionally (the gfxPlatformFontList constructor) and the
    // browser would keep running and rendering, fingerprinting as whatever
    // machine it is on.
    //
    // Reaching this line means all three sources failed: the
    // STEALTHFOX_FONT_MANIFEST env var, the zoom.stealth.fonts.manifest pref,
    // AND the packaged fonts/bundle-fonts.list under GRE_DIR. That last one is
    // the deliberate floor for a browser launched without invisible_core, and
    // it is present in both objdirs and both release archives, so this is a
    // broken installation and not a configuration we support. A broken install
    // has to stop, not quietly become a different browser.
    mozilla::gfx::StealthRefuse(
        "stealth: bundle-fonts.list not loadable from env, pref or GRE_DIR - "
        "refusing to run with host fonts");
  }();
  return sInstance;
}

// ---------------------------------------------------------------------------
// gfxFontEntry::ShouldPublishCharacterMap (hook in gfxFontEntry.cpp).
// ---------------------------------------------------------------------------

bool StealthBundleFontList::ShouldPublishCharacterMap(
    const gfxFontEntry& aEntry) {
  if (!aEntry.mShmemFace) {
    // Not a face from the shared list: there is no slot to publish into. This
    // alone excludes every downloaded @font-face, which is never built from a
    // fontlist::Face and so never has an mShmemFace.
    return false;
  }
  if (aEntry.mIsLocalUserFont) {
    // A `src: local()` reference borrows the list's face but carries the
    // @font-face rule's own weight/stretch/style ranges; its cmap must not
    // overwrite the slot the list's own face owns.
    return false;
  }
  if (!aEntry.mIsDataUserFont) {
    // Upstream case: an installed system face, exactly what the old
    // `!IsUserFont()` test meant to allow.
    return true;
  }
  // Bundle-only: every face in the list is instantiated from its bundle file, so
  // mIsDataUserFont is true for all of them and the old test rejected the entire
  // font list. Publishing is what makes the family character map a real union
  // instead of the deliberate empty "unusable family" map, and therefore what
  // lets IsFullyInitialized() ever become true.
  return gfxPlatformFontList::PlatformFontList()->StealthBundleOnly();
}

// ---------------------------------------------------------------------------
// Hooks called from gfxPlatformFontList.cpp (series folder
// fingerprint/fonts/font-list). Declared in StealthBundleFontList.h.
// ---------------------------------------------------------------------------

// Reports one number to about:memory. A free function cannot use
// MOZ_COLLECT_REPORT: that macro names KIND_HEAP / UNITS_BYTES unqualified and
// only compiles inside a class deriving from nsIMemoryReporter, so this spells
// the same call the macro expands to (xpcom/base/nsIMemoryReporter.idl:608-611),
// with the constants qualified.
static void ReportBundleBytes(nsIHandleReportCallback* aHandleReport,
                              nsISupports* aData, const nsACString& aPath,
                              int32_t aKind, size_t aBytes,
                              const nsACString& aDescription) {
  (void)aHandleReport->Callback(""_ns, aPath, aKind,
                                nsIMemoryReporter::UNITS_BYTES, int64_t(aBytes),
                                aDescription, aData);
}

void StealthReportBundleFiles(mozilla::MallocSizeOf aMallocSizeOf,
                              nsIHandleReportCallback* aHandleReport,
                              nsISupports* aData) {
  // The bundled font FILES, which are the largest single allocation this build
  // makes and had no reporter at all - so they landed in heap-unclassified and
  // about:memory could not see them. The bundle is 218.7 MB over 126 files, the
  // cache is per PROCESS (every process that rasterises a face reads its file),
  // and it is never cleared. Not being able to measure the biggest thing you
  // allocate is how an optimisation gets planned on a guess.
  //
  // Get() never returns null (it crashes first, see its contract), so the null
  // test the inline code had is gone.
  auto* bundle = StealthBundleFontList::Get();
  size_t bundleHeap = bundle->SizeOfIncludingThis(aMallocSizeOf);
  ReportBundleBytes(
      aHandleReport, aData, "explicit/gfx/font-bundle-files"_ns,
      nsIMemoryReporter::KIND_HEAP, bundleHeap,
      "Bundled font files held on this process's HEAP. Zero once they are "
      "mapped instead of read; one entry per FILE, never per face."_ns);

  // Reported apart and as NONHEAP, because these pages are the file's: the
  // operating system shares them with every process that maps the same file,
  // so adding them to the heap figure would count one physical page N times.
  size_t bundleMapped = bundle->MappedBytes();
  if (bundleMapped) {
    ReportBundleBytes(
        aHandleReport, aData, "font-bundle-files-mapped"_ns,
        nsIMemoryReporter::KIND_NONHEAP, bundleMapped,
        "Bundled font files MAPPED read-only. Shared with every other "
        "process that mapped them, and evictable by the OS."_ns);
  }
}

// gfxPlatformFontList::mStealthBundleOnly (declared in gfxPlatformFontList.h):
// Stealth bundle-only: always true in this patched build. The platform font
// list is built from the bundled fonts ALONE; the host font enumeration is
// skipped entirely (see the InitFontList backends), so host system fonts
// never enter it. The exposed set is exactly the bundle - no external
// allow-list. Set in the constructor.

// Stealth: true in this patched build. Set in the constructor and never
// mutated, so it is safe to read without holding mLock. Needed outside this
// class because "is this face one of ours" cannot be answered by IsUserFont():
// every bundled face is constructed from file data and so reports true.
bool gfxPlatformFontList::StealthBundleOnly() const {
  return mStealthBundleOnly;
}

// The three gfxPlatformFontList members below are STEALTH [B317]: they give
// the bundle's faces the alias, PostScript and full names a platform gives its
// installed fonts. Defined here and not in gfxPlatformFontList.cpp so that the
// Mozilla file carries no logic; a member definition in another .cpp has the
// same access as one in the class's own .cpp, so the protected members they use
// need no friend and no header change.

// STEALTH [B317]: record what one face's 'name' table says - its alternate
// family names in mAliasTable, its PostScript and full names in the local
// names table that @font-face src:local() searches. It reads the bundle's
// faces (from the bundle file) through ReadFaceNamesForFamily below; the
// DirectWrite back end keeps its own upstream loop for installed fonts and
// does not call it. Before it the bundle's tables were never read on any
// platform, so src:local() found no bundled face and no localized family name
// resolved.
void gfxPlatformFontList::RecordFaceNamesFromTable(
    fontlist::Family* aFamily, uint32_t aFaceIndex, const nsACString& aKey,
    const nsCString& aFamilyName, const char* aNameTable, uint32_t aLength) {
  // The same reading gfxDWriteFontList::ReadFaceNamesForFamily does for an
  // installed font (that loop keeps its upstream text), so that the bundle's
  // faces get exactly what DirectWrite gives an installed font, and nothing
  // that could drift from it.
  const fontlist::Pointer* facePtrs = aFamily->Faces(SharedFontList());
  AutoTArray<nsCString, 4> otherFamilyNames;
  gfxFontUtils::ReadOtherFamilyNamesForFace(aFamilyName, aNameTable, aLength,
                                            otherFamilyNames, false);
  for (const auto& alias : otherFamilyNames) {
    nsAutoCString key(alias);
    ToLowerCase(key);
    auto aliasData = mAliasTable.GetOrInsertNew(key);
    aliasData->InitFromFamily(aFamily, aFamilyName);
    aliasData->mFaces.AppendElement(facePtrs[aFaceIndex]);
  }

  nsAutoCString psname, fullname;
  // Bug 1854090: don't load PSname if the family name ends with ".tmp",
  // as some PDF-related software appears to pollute the font collection
  // with spurious re-encoded versions of standard fonts like Arial, fails
  // to alter the PSname, and thus can result in garbled rendering because
  // the wrong resource may be found via src:local(...).
  if (!StringEndsWith(aKey, ".tmp"_ns)) {
    if (NS_SUCCEEDED(gfxFontUtils::ReadCanonicalName(
            aNameTable, aLength, gfxFontUtils::NAME_ID_POSTSCRIPT, psname))) {
      ToLowerCase(psname);
      MaybeAddToLocalNameTable(psname,
                               fontlist::LocalFaceRec::InitData(aKey, aFaceIndex));
    }
  }
  if (NS_SUCCEEDED(gfxFontUtils::ReadCanonicalName(
          aNameTable, aLength, gfxFontUtils::NAME_ID_FULL, fullname))) {
    ToLowerCase(fullname);
    if (fullname != psname) {
      MaybeAddToLocalNameTable(fullname,
                               fontlist::LocalFaceRec::InitData(aKey, aFaceIndex));
    }
  }
}

void gfxPlatformFontList::ReadFaceNamesForFamily(
    fontlist::Family* aFamily, bool aNeedFullnamePostscriptNames) {
  // STEALTH [B317]: a platform that reads names while enumerating (fontconfig)
  // has nothing to do here for its own fonts; the bundle's faces were never
  // enumerated by any platform, so their names are read here, on every one.
  if (!mStealthBundleOnly) {
    return;
  }
  if (!aFamily->IsInitialized() && !InitializeFamily(aFamily)) {
    return;
  }
  fontlist::FontList* list = SharedFontList();
  const fontlist::Pointer* facePtrs = aFamily->Faces(list);
  nsAutoCString familyName(aFamily->DisplayName().AsString(list));
  nsAutoCString key(aFamily->Key().AsString(list));
  auto* bundle = StealthBundleFontList::Get();
  for (uint32_t i = 0; i < aFamily->NumFaces(); ++i) {
    auto* face = facePtrs[i].ToPtr<fontlist::Face>(list);
    if (!face) {
      continue;
    }
    gfxFontUtils::AutoHBBlob names(bundle->GetFaceNameTable(
        nsCString(face->mDescriptor.AsString(list)), face->mIndex));
    if (!names) {
      continue;
    }
    unsigned int length = 0;
    const char* data = hb_blob_get_data(names, &length);
    RecordFaceNamesFromTable(aFamily, i, key, familyName, data, length);
  }
}

// STEALTH [B317]: implemented here for the bundle's faces, on every platform,
// from the 'name' table of the bundle file; a platform's override delegates
// here when the font list is bundle-only.
bool gfxPlatformFontList::ReadFaceNames(const fontlist::Family* aFamily,
                                        const fontlist::Face* aFace,
                                        nsCString& aPSName,
                                        nsCString& aFullName) {
  // STEALTH [B317]: the bundle's faces, on every platform; the platform's own
  // faces are its override's business.
  if (!mStealthBundleOnly) {
    return false;
  }
  gfxFontUtils::AutoHBBlob names(StealthBundleFontList::Get()->GetFaceNameTable(
      nsCString(aFace->mDescriptor.AsString(SharedFontList())), aFace->mIndex));
  if (!names) {
    return false;
  }
  (void)gfxFontUtils::ReadCanonicalName(names, gfxFontUtils::NAME_ID_POSTSCRIPT,
                                        aPSName);
  (void)gfxFontUtils::ReadCanonicalName(names, gfxFontUtils::NAME_ID_FULL,
                                        aFullName);
  return !aPSName.IsEmpty() || !aFullName.IsEmpty();
}

void StealthGetCommonFallbackFonts(bool aBundleOnly, uint32_t aCh,
                                   mozilla::intl::Script aRunScript,
                                   FontPresentation aPresentation,
                                   nsTArray<const char*>& aOut) {
  // This is the list that decides which font renders a character its own font
  // lacks, and it was the last per-platform font decision left.
  // gfxWindowsPlatform answers with a 93-case script switch plus a
  // character-dependent tail; gfxPlatformGtk answered with a flat list that
  // merely began with four Windows names.
  //
  // CORRECTED 2026-08-08 by an audit, because the original justification here
  // was WRONG and would have been the reason a future reader kept 139 records
  // without re-examining them. This block used to cite a bare U+2764 measuring
  // 31.77 on Windows against 40.03 on Linux. Neither number is a fallback
  // advance: both are Arial's OWN aveCharWidth, i.e. the missing-glyph hex box
  // (gfxFont.cpp:858-865, the advance is max(aveCharWidth, hex-box minimum)).
  // DWrite reads OS/2 xAvgCharWidth, 904/2048 x 72 = 31.78; FreeType takes
  // max(that, the '0' advance), 1139/2048 x 72 = 40.04. No fallback font was
  // chosen on EITHER host, so no list could have caused it - the empty family
  // charmap did, see StealthLoadMissingCharacterMaps below. For U+2764 both
  // lists in fact answer Segoe UI Symbol, identically.
  //
  // What the table really buys is CJK. For U+4E00 twelve bundled families
  // carry the codepoint, so GlobalFontFallback's whole-list scan lands on MS
  // Gothic, while S|17|SimSun|SimSun-ExtB gives what Windows gives. That is
  // the evidence it survives on.
  //
  // The NAMES and their order come from the manifest on every host. The
  // classification below stays here on purpose - the colour-presentation test
  // and the symbol/punctuation test are properties of the CHARACTER, defined
  // by Unicode, and were never the platform's font engine. Copied from
  // gfxWindowsPlatform::GetCommonFallbackFonts so the condition matches the
  // list it selects.
  //
  // The platform call is the upstream one: it runs when not bundle-only, and
  // when the manifest has no fallback table (a pre-v4 file; GetCommonFallback
  // returns false before appending anything).
  if (aBundleOnly) {
    auto* bundle = StealthBundleFontList::Get();  // never null; it crashes first
    const uint32_t b = aCh >> 8;
    // mozilla::unicode:: is spelled out: gfxPlatformFontList.cpp only compiled
    // the unqualified call because a unified-build neighbour (gfxFont.cpp:63,
    // gfxFontEntry.cpp:32 ...) has 'using namespace mozilla::unicode'; this
    // file is not unified.
    const bool symbolish =
        aRunScript == mozilla::intl::Script::COMMON ||  // stray COMMON chars
        (b >= 0x20 && b <= 0x2b) || b == 0x2e ||        // BMP symbols/punct.
        mozilla::unicode::GetGenCategory(aCh) == nsUGenCategory::kSymbol ||
        mozilla::unicode::GetGenCategory(aCh) == nsUGenCategory::kPunctuation;
    if (bundle->GetCommonFallback(int16_t(aRunScript), PrefersColor(aPresentation),
                                  symbolish, aOut)) {
      return;
    }
  }
  gfxPlatform::GetPlatform()->GetCommonFallbackFonts(aCh, aRunScript,
                                                     aPresentation, aOut);
}

// Stealth: the ONE place that decides whether a fallback search may skip a
// family whose character maps are not loaded yet, kicking off an async load
// and moving on. Two sites ask (GlobalFontFallback and
// gfxFontGroup::FindFallbackFaceForChar) and the answer has to be the same in
// both: skipping makes the result depend on what this process happens to have
// touched already, and that is observable in measureText widths. The condition
// was written out by hand at the first site and was simply MISSING at the
// second, so every text run took the deferring path while the code read as if
// bundle-only had been handled. One predicate, two readers.
bool gfxPlatformFontList::MayDeferCmapLoading() const {
  // Never under bundle-only. Deferring means answering a fallback search from a
  // partially loaded list, so the answer depends on what this process happens to
  // have touched already - and that is observable: measured 2026-08-07, the
  // regional-indicator pair through "Arial" gave 52.63 on BOTH platforms when it
  // was the first thing a fresh page measured, and 63.53 (Windows) vs 80.07
  // (Linux) once other families had been measured first. Same binary, same page;
  // the divergence was the load order. 68 bundled families is a small enough list
  // to search synchronously, and a deterministic answer is the whole point of
  // moving fallback into the manifest.
  //
  // The condition used to be spelled out in GlobalFontFallback, and the same
  // condition was MISSING at the other site that asks it
  // (gfxFontGroup::FindFallbackFaceForChar, in gfxTextRun.cpp). It lives in one
  // place - here - which carries the measurement and the reason.
  //
  // Stealth: gfxFontGroup::FindFallbackFaceForChar (gfxTextRun.cpp) is the site
  // the bundle-only condition was missing from. The twin site in
  // GlobalFontFallback spelled it out by hand, this one (FindFallbackFaceForChar)
  // did not, and under bundle-only NO family is ever fully initialized - so every
  // text run that needed a fallback face within a family took this early return
  // in the content process. MayDeferCmapLoading is now the single answer for
  // both.
  return !mStealthBundleOnly &&
         StaticPrefs::gfx_font_rendering_fallback_async() &&
         !XRE_IsParentProcess();
}

void StealthApplyAlias(bool aBundleOnly, nsACString& aKey) {
  // Applies the manifest's canonical Windows substitute table, on every OS,
  // before the family list is consulted.
  //
  // A name that is not a family can still resolve through Windows' substitute
  // table, and a detector reads the difference. Measured 2026-08-07 on the
  // shipped firefox-18: a commercial fingerprinting service probes "HELV" and
  // "Small Fonts", both resolved on Windows and neither on Linux (8 fonts
  // resolved against 6), because gfxDWriteFontList reads them from the HOST
  // REGISTRY with no bundle-only gate and Linux has no such registry. That made
  // the answer depend on the OS and, on Windows, on the individual machine -
  // third-party software adds registry entries.
  //
  // This is the single place all three backends pass through: the DWrite
  // override calls up into this base after applying its own table, and the
  // fontconfig one after its -moz-sentinel pass, so one hook covers them all.
  // The platform tables are suppressed separately (see gfxDWriteFontList's
  // GetFontSubstitutes call site) so this is the ONLY source of aliases.
  if (!aBundleOnly) {
    return;
  }
  nsAutoCString target;
  if (StealthBundleFontList::Get()->GetAlias(aKey, target)) {
    // What gfxPlatformFontList::GenerateFontListKey(target, key) does; that
    // member is protected, so its two lines are repeated here (same bytes).
    aKey = target;
    ToLowerCase(aKey);
  }
}

// InitializeFamily's eager character-map loader, run after BOTH branches of
// `if (!aFamily->IsInitialized())`.
//
// It used to be the `else` of that branch, i.e. it ran ONLY for a family whose
// face list already existed, on the assumption that a backend asked with
// aLoadCmaps=true fills Face::InitData::mCharMap while it enumerates. Our bundle
// path cannot do that without a SECOND cmap reader of its own, so under
// bundle-only a family initialized with aLoadCmaps=true came out with every face
// map null and stayed that way forever - the eager loader existed and was
// unreachable on the first call, which is the only call most families ever get.
//
// Running it after both branches costs upstream nothing: the loop skips any face
// that already has a map, so a backend that filled InitData still does the work
// exactly once. It is used when a child process is doing SearchAllFontsForChar,
// to have the parent load all the cmaps at once and reduce IPC traffic (and
// content-process file access overhead, which is crippling for DirectWrite on
// Windows).
//
// The SetupFamilyCharMap call that follows in InitializeFamily used to carry
// `&& !mStealthBundleOnly`, and that deroga was CORRECT for as long as no bundled
// face had a character map: with every face map null, SetupFamilyCharMap takes
// its "unusable family" branch and stores the EMPTY map deliberately so it will
// not retry, and from there SearchAllFontsForChar returns on its second line for
// every codepoint, both CommonFontFallback and GlobalFontFallback funnel through
// it, SystemFindFontForChar returns null, and every character needing system
// fallback rendered as a hex box. Measured 2026-08-08: U+2764 through "Arial"
// gave 31.77 on Windows and 40.03 on Linux, and neither is a font - both are the
// hex box, whose advance is max(aveCharWidth, 13px) of the REQUESTED font, where
// retail Firefox gives 69.97 (Segoe UI Symbol, a family we bundle and could not
// reach).
//
// The deroga is gone because its PREMISE is gone:
// gfxFontEntry::ShouldPublishCharacterMap now lets a bundled face publish into
// the shared list, so the union computed there is a real union and not an empty
// map. What the deroga cost while it stood is the whole reason this changed:
// with no family map, IsFullyInitialized() is false for all 68 families forever,
// so SearchAllFontsForChar could never reject a family cheaply AND every search
// re-ran InitializeFamily through IPC because nothing was ever cached. Measured
// 2026-08-13 on the cost bench: 176.9 ms per fallback lookup, and 206.7 ms on
// the SECOND page of the same session - it did not warm up, because there was
// nothing to warm.
//
// CreateFontEntry is a protected virtual of gfxPlatformFontList, so the caller
// hands it in bound to the family instead of this function needing a friend or a
// new member declaration in gfxPlatformFontList.h (whose hook budget can only
// go down).
void StealthLoadMissingCharacterMaps(
    mozilla::fontlist::FontList* aList, mozilla::fontlist::Family* aFamily,
    bool aLoadCmaps,
    mozilla::FunctionRef<already_AddRefed<gfxFontEntry>(
        mozilla::fontlist::Face*)>
        aCreateFontEntry) {
  if (!aLoadCmaps) {
    return;
  }
  if (auto* faces = aFamily->Faces(aList)) {
    for (size_t i = 0; i < aFamily->NumFaces(); i++) {
      auto* face = faces[i].ToPtr<mozilla::fontlist::Face>(aList);
      if (face && face->mCharacterMap.IsNull()) {
        // We don't want to cache this font entry, as the parent will most
        // likely never use it again; it's just to populate the charmap for
        // the benefit of the child process.
        RefPtr<gfxFontEntry> fe = aCreateFontEntry(face);
        if (fe) {
          fe->ReadCMAP();
        }
      }
    }
  }
}

void StealthSystemUIFamilies(nsTArray<nsCString>& aFamilies) {
  // Our persona is always Windows, so `font-family: system-ui` must resolve to
  // "Segoe UI" on every host - never the host's real menu font (Cantarell on
  // Linux, San Francisco on macOS). Hardcoded, no external config.
  aFamilies.AppendElement("Segoe UI"_ns);
}

// The DECLARED generic table, from zoom.stealth.fonts.generics, as
// "generic|lang|family" records separated by newlines. It is the ONLY source:
// an empty pref returns false here and no generic is prepended. The compiled
// table that used to sit under it as a floor was deleted on 2026-08-09 (engine
// rule 7), and a process never gets here with the pref empty because
// StealthDeclarationGate refuses to launch (StealthDeclarationGate.cpp:96-99).
// [The old comment said an empty pref "falls through to the compiled table
// below"; that table no longer exists.]
//
// It is a pref rather than ten rows of C++ because the domain is finite and
// perfectly known (four generics x five langgroups), which is the condition
// under which a value belongs to the core rather than to the engine. Changing
// which face answers `font-family: serif` should not cost a browser rebuild.
//
// Maps the standard CSS generics to the bundled Windows fonts, per language
// group, matching a real Windows install. Returns false for generics we don't
// force (natural resolution against the bundle) and for system-ui, which
// StealthSystemUIFamilies handles separately.
//
// `cursive` joined the list on 2026-08-07. It had been grouped with the
// "non-text generics" and left out, and that made it a cross-OS tell: Windows
// resolves it from its own pref (font.name-list.cursive.x-western = "Comic Sans
// MS" in the Windows block of all.js, which IS bundled) while Linux keeps the
// literal pref value "cursive" (the Linux block of all.js), matching no bundled
// family and collapsing to sans-serif. Measured on the shipped firefox-18:
// 2191.95px wide on Windows against 2128.37 on Linux, the latter being exactly
// the sans-serif width.
//
// `fantasy` is deliberately still excluded: it already agrees cross-OS (both
// land on the sans-serif value), so forcing it would change behaviour without
// closing a gap.
bool StealthDeclaredGenericFont(StyleGenericFontFamily aGeneric,
                                const char* aLang, nsACString& aOut) {
  const char* generic = nullptr;
  switch (aGeneric) {
    case StyleGenericFontFamily::Serif:
      generic = "serif";
      break;
    case StyleGenericFontFamily::SansSerif:
      generic = "sans-serif";
      break;
    case StyleGenericFontFamily::Monospace:
      generic = "monospace";
      break;
    case StyleGenericFontFamily::Cursive:
      generic = "cursive";
      break;
    default:
      return false;
  }
  nsAutoCString spec;
  {
    auto lock = mozilla::StaticPrefs::zoom_stealth_fonts_generics();
    spec = *lock;
  }
  if (spec.IsEmpty()) {
    return false;
  }
  const nsDependentCString wantGeneric(generic);
  const nsDependentCString wantLang(aLang ? aLang : "");
  // Two passes: an exact langgroup match wins over the default row, so the
  // order of the records in the pref cannot change the answer.
  for (int pass = 0; pass < 2; ++pass) {
    for (const nsACString& entry : spec.Split('\n')) {
      nsCString rec(entry);
      const int32_t bar1 = rec.FindChar('|');
      if (bar1 <= 0) continue;
      const int32_t bar2 = rec.FindChar('|', bar1 + 1);
      if (bar2 <= bar1) continue;
      if (!wantGeneric.Equals(Substring(rec, 0, bar1))) continue;
      const nsDependentCSubstring lang(
          Substring(rec, bar1 + 1, bar2 - bar1 - 1));
      const bool isDefault = lang.IsEmpty();
      if (pass == 0 ? (isDefault || !wantLang.Equals(lang)) : !isDefault) {
        continue;
      }
      aOut.Assign(Substring(rec, bar2 + 1));
      return !aOut.IsEmpty();
    }
  }
  return false;
}

void StealthPrependDeclaredGeneric(bool aBundleOnly,
                                   StyleGenericFontFamily aGeneric,
                                   const char* aLang,
                                   nsTArray<nsCString>& aFamilies) {
  // Forces the standard generics to the bundled Windows fonts on every host
  // (prepend = highest priority), so they never resolve to a dropped host font.
  // No external font.name-list.* needed.
  if (!aBundleOnly) {
    return;
  }
  // ONE source: the declaration, and nothing under it. The compiled table that
  // used to sit here as a floor was deleted on 2026-08-09 with engine rule 7 -
  // two tables for one fact are two things that can disagree, which is how the
  // taskbar height came to be written in four places. If the declaration is
  // missing the process has already refused to start (StealthDeclarationGate),
  // so reaching here means it is present.
  nsAutoCString declared;
  if (StealthDeclaredGenericFont(aGeneric, aLang, declared)) {
    aFamilies.InsertElementAt(0, declared);
  }
}

void StealthPrependEmojiFamily(bool aBundleOnly,
                               nsTArray<nsCString>& aFamilies) {
  // Same prepend StealthPrependDeclaredGeneric does for the other generics, for
  // the same reason. ResolveEmojiFontNames was the one sibling that never got
  // it, and that asymmetry WAS the emoji bug: font.name-list.emoji is a
  // build-time #ifdef, "Segoe UI Emoji, Twemoji Mozilla" in the Windows block of
  // all.js but "Noto Color Emoji, Twemoji Mozilla" in the Linux one. Only the
  // Windows list names a bundled family, so on Linux this list resolved to
  // nothing, emoji fell through to gfxPlatformGtk::GetCommonFallbackFonts -
  // whose eight names are ALL absent from the bundle - and rendered from
  // whatever the alphabetical cmap scan happened to reach.
  //
  // Measured on the shipped firefox-18: an emoji run under font-family:serif
  // was 340.58px wide on Windows and 252.00 on Linux, while asking for
  // "Segoe UI Emoji" explicitly gave the identical 340.58 on both - i.e. the
  // bundled font was always there and simply never selected.
  if (aBundleOnly) {
    aFamilies.InsertElementAt(0, "Segoe UI Emoji"_ns);
  }
}

// ---------------------------------------------------------------------------
// CoreText back end hooks (series folder fingerprint/fonts/font-list), called
// from CoreTextFontList.cpp.
// ---------------------------------------------------------------------------

bool StealthDropFamily(bool aBundleOnly,
                       const nsTHashSet<nsCStringHashKey>& aBundled,
                       const nsACString& aFamilyName) {
  return aBundleOnly && !aBundled.Contains(aFamilyName);
}

bool StealthSetBundledFamilyNames(bool aBundleOnly,
                                  mozilla::fontlist::FontList* aList) {
  if (!aBundleOnly || !XRE_IsParentProcess()) {
    return false;
  }
  // Uniform bundle: build the shared family list from our manifest (skipping
  // the CoreText enumeration; host fonts never enter). Faces are provided
  // lazily by GetFacesInitDataForFamily.
  auto* bundle = StealthBundleFontList::Get();  // never null; it crashes first
  if (!aList) {
    return false;
  }
  nsTArray<Family::InitData> families(bundle->Families().Clone());
  aList->SetFamilyNames(families);
  return true;
}

#ifdef XP_DARWIN
static void StealthCFStringToUTF16(CFStringRef aSrc, nsAString& aDest) {
  auto len = CFStringGetLength(aSrc);
  aDest.SetLength(len);
  CFStringGetCharacters(aSrc, CFRangeMake(0, len),
                        reinterpret_cast<UniChar*>(aDest.BeginWriting()));
}

RefPtr<CTFontEntry> StealthBundledCTFontEntry(
    Face* aFace, const Family* aFamily, mozilla::fontlist::FontList* aList) {
  // Instantiate the face straight from its bundle file:
  // CTFontManagerCreateFontDescriptorsFromData yields one descriptor per face
  // of a .ttc, so pick the wanted index and wrap its CGFont in a CTFontEntry.
  auto* bundle = StealthBundleFontList::Get();  // never null; it crashes first
  nsCString file(aFace->mDescriptor.AsString(aList));
  // Borrow the ONE cached copy of the file instead of making two more per
  // face - the same correction §5.2l applied to the DirectWrite path, here on
  // CoreText. What was here copied in `ReadFaceData` and then copied AGAIN
  // inside `CFDataCreate`, both per face: `mingliub.ttc` is 35.4 MB with two
  // faces, so instantiating both cost ~106 MB where 35.4 are enough.
  //
  // The header's note that the CoreText path "hands ownership to the platform
  // object" did not hold for this call site: `CFDataCreate` copies, so the
  // nsTArray died at scope exit and nobody had taken ownership of anything.
  //
  // `kCFAllocatorNull` is the deallocator for the same reason the DirectWrite
  // borrow is safe, and it is not an optimistic assumption: the blob lives in
  // `mFileCache`, on a `StealthBundleFontList` that is leaked on purpose
  // ("lives for the process") and never emptied. The bytes therefore outlive
  // any CGFont CoreText derives from them, and CFData must NOT free them: they
  // are not ours, and on a mapped blob they are the file's pages.
  // `NativeFontResourceMac.cpp` builds a real deallocator instead, correctly -
  // there the caller owns the bytes.
  RefPtr<StealthBundleFontList::FileBlob> blob = bundle->GetFaceBlob(file);
  if (!blob || !blob->Length()) {
    return nullptr;
  }
  AutoCFTypeRef<CFDataRef> cfData(CFDataCreateWithBytesNoCopy(
      kCFAllocatorDefault, blob->Data(), blob->Length(), kCFAllocatorNull));
  if (!cfData) {
    return nullptr;
  }
  AutoCFTypeRef<CFArrayRef> descriptors(
      CTFontManagerCreateFontDescriptorsFromData(cfData));
  if (!descriptors) {
    return nullptr;
  }
  CFIndex descCount = CFArrayGetCount(descriptors);
  // Pick the wanted face by PostScript name, NOT by raw array index: Apple
  // does not document CTFontManagerCreateFontDescriptorsFromData as returning
  // a .ttc's faces in the collection's file order, so indexing could select
  // the wrong CJK face. Match kCTFontNameAttribute (the PostScript name)
  // against the manifest's psname; fall back to the index only if that fails.
  CTFontDescriptorRef desc = nullptr;
  nsAutoCString wantPsname;
  if (bundle->GetPsname(file, aFace->mIndex, wantPsname)) {
    for (CFIndex i = 0; i < descCount; i++) {
      auto* d = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descriptors, i);
      AutoCFTypeRef<CFStringRef> ps(
          (CFStringRef)CTFontDescriptorCopyAttribute(d, kCTFontNameAttribute));
      if (!ps) {
        continue;
      }
      nsAutoString ps16;
      StealthCFStringToUTF16(ps, ps16);
      if (NS_ConvertUTF16toUTF8(ps16).Equals(wantPsname)) {
        desc = d;
        break;
      }
    }
  }
  if (!desc) {
    if (aFace->mIndex >= descCount) {
      return nullptr;
    }
    desc = (CTFontDescriptorRef)CFArrayGetValueAtIndex(descriptors,
                                                       aFace->mIndex);
  }
  AutoCFTypeRef<CTFontRef> ctFont(
      CTFontCreateWithFontDescriptor(desc, 0.0, nullptr));
  if (!ctFont) {
    return nullptr;
  }
  AutoCFTypeRef<CGFontRef> cgFont(CTFontCopyGraphicsFont(ctFont, nullptr));
  if (!cgFont) {
    return nullptr;
  }
  nsAutoCString name(aFamily->DisplayName().AsString(aList));
  return MakeRefPtr<CTFontEntry>(name, cgFont, aFace->mWeight, aFace->mWidth,
                                 aFace->mStyle, true, false);
}
#endif  // XP_DARWIN

void StealthFacesForFamily(const Family* aFamily,
                           mozilla::fontlist::FontList* aList,
                           nsTArray<Face::InitData>& aFaces) {
  StealthBundleFontList::Get()->GetFaces(aFamily->DisplayName().AsString(aList),
                                         aFaces);
}
