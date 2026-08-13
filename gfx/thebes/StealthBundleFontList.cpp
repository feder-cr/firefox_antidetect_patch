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

#include "gfxFontConstants.h"
#include "mozilla/FontPropertyTypes.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "prenv.h"
#include "nsIFile.h"
#include "nsIInputStream.h"
#include "nsNetUtil.h"
#include "nsStreamUtils.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsReadableUtils.h"

using namespace mozilla;
using mozilla::fontlist::Face;
using mozilla::fontlist::Family;

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
        StretchRange(FontStretch::FromFloat(f.mStretchMin),
                     FontStretch::FromFloat(f.mStretchMax)),
        SlantStyleRange(f.mItalic ? FontSlantStyle::ITALIC
                                  : FontSlantStyle::NORMAL),
        nullptr,  // mCharMap
    };
    aFaces.AppendElement(std::move(face));
  }
}

already_AddRefed<StealthBundleFontList::FileBlob>
StealthBundleFontList::GetFaceBlob(const nsACString& aFile) {
  nsCString file(aFile);
  if (auto cached = mFileCache.Lookup(file)) {
    return do_AddRef(cached.Data());
  }
  nsCOMPtr<nsIFile> path;
  if (NS_FAILED(NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(path))) ||
      NS_FAILED(path->Append(u"fonts"_ns)) ||
      NS_FAILED(path->Append(NS_ConvertUTF8toUTF16(file)))) {
    return nullptr;
  }
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

bool StealthBundleFontList::ReadFaceData(const nsACString& aFile,
                                         nsTArray<uint8_t>& aData) {
  // One more copy than GetFaceBlob, on purpose: the FreeType and CoreText
  // paths hand ownership of the bytes to a platform object that frees them.
  RefPtr<FileBlob> blob = GetFaceBlob(aFile);
  if (!blob) {
    return false;
  }
  aData.AppendElements(blob->Data(), blob->Length());
  return true;
}

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
    // is on unconditionally (gfxPlatformFontList.cpp:325) and the browser would
    // keep running and rendering, fingerprinting as whatever machine it is on.
    //
    // Reaching this line means all three sources failed: the
    // STEALTHFOX_FONT_MANIFEST env var, the zoom.stealth.fonts.manifest pref,
    // AND the packaged fonts/bundle-fonts.list under GRE_DIR. That last one is
    // the deliberate floor for a browser launched without invisible_core, and
    // it is present in both objdirs and both release archives, so this is a
    // broken installation and not a configuration we support. A broken install
    // has to stop, not quietly become a different browser.
    MOZ_CRASH(
        "stealth: bundle-fonts.list not loadable from env, pref or GRE_DIR - "
        "refusing to run with host fonts");
  }();
  return sInstance;
}
