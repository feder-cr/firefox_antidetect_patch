/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StealthBundleFontList_h_
#define StealthBundleFontList_h_

#include "SharedFontList.h"
#include "nsTArray.h"
#include "nsTHashMap.h"
#include "nsHashKeys.h"

// One manifest face, stored as trivially-copyable raw values. Both
// fontlist::Face::InitData and the CSS WeightRange/StretchRange/SlantStyleRange
// types are move-only (deleted copy), so we cannot cache them; instead we cache
// these primitives and build a fresh Face::InitData (with fresh ranges) per
// GetFaces call, the way the platform backends build theirs.
/**
 * One face's VERTICAL metrics, in FONT UNITS (scale by size / mUpem).
 *
 * These exist so that NO platform backend computes them. DWrite, FreeType and
 * CoreText each derive ascent/descent from the same file by their own rule and
 * disagree: measured 2026-08-07 on the shipped firefox-18, one build with one
 * set of bundled fonts produced different ascent/descent on 58 of 72 families
 * (up to 27px at a 72px size), different capHeight on 10 and xHeight on 5 -
 * while every advance width was already byte-identical, because shaping is
 * HarfBuzz in-tree. Hence the vertical axis only.
 *
 * The values are the ones a real Windows Firefox produces (the persona this
 * build claims), NOT the font's raw OS/2 numbers: the generator resolves
 * DirectWrite's usWin*-vs-sTypo* rule offline. Validated against the shipped
 * Windows build for 72/72 families - see gen_bundle_font_manifest.py
 * _vertical_metrics().
 *
 * mDescent is POSITIVE (distance below the baseline), matching usWinDescent
 * and gfxFont::Metrics. mUpem == 0 means "not in the manifest", and the caller
 * must then leave the backend's own values alone.
 */
struct StealthBundleVMetrics {
  uint16_t mUpem = 0;
  int32_t mAscent = 0;
  int32_t mDescent = 0;
  int32_t mLineGap = 0;
  int32_t mXHeight = 0;    // 0 = absent from OS/2 (pre-v2 table)
  int32_t mCapHeight = 0;  // 0 = absent from OS/2 (pre-v2 table)
  int32_t mUnderlineOffset = 0;
  int32_t mUnderlineSize = 0;
  int32_t mStrikeoutOffset = 0;
  int32_t mStrikeoutSize = 0;

  bool IsValid() const { return mUpem > 0; }
};

struct StealthBundleFace {
  nsCString mFile;
  nsCString mPsname;  // PostScript name (nameID 6); used to pick the right .ttc face
  uint16_t mIndex = 0;
  int32_t mWeightMin = 400;
  int32_t mWeightMax = 400;
  float mStretchMin = 100.0f;
  float mStretchMax = 100.0f;
  bool mItalic = false;
  StealthBundleVMetrics mVMetrics;
};

/**
 * Reads <GRE>/fonts/bundle-fonts.list (generated offline by
 * scripts/gen_bundle_font_manifest.py) and hands back the canonical family list
 * plus the per-family face init data. This lets InitSharedFontListForPlatform
 * build the shared font list uniformly from the bundle on EVERY OS, instead of
 * enumerating the host font backend (DWrite / fontconfig / CoreText), which
 * diverges per OS. The OS is used only to rasterize a face from its file (see
 * each backend's CreateFontEntry).
 *
 * A Face's mDescriptor holds the bundle file name and mIndex the face index
 * within it (0 for .ttf, 0..N for .ttc); CreateFontEntry loads
 * <GRE>/fonts/<file> at that index. Parsed once, lazily, on first Get().
 */
class StealthBundleFontList final {
 public:
  // Returns the singleton, or nullptr if the manifest is missing/unreadable
  // (caller then falls back to an empty bundle-only list - never to host
  // fonts).
  static StealthBundleFontList* Get();

  // The full family list, ready for SharedFontList()->SetFamilyNames().
  const nsTArray<mozilla::fontlist::Family::InitData>& Families() const {
    return mFamilies;
  }

  // Appends the faces for aFamilyName (its display name) to aFaces. No-op if
  // the family is unknown. Mirrors the platform GetFacesInitDataForFamily
  // contract.
  void GetFaces(const nsACString& aFamilyName,
                nsTArray<mozilla::fontlist::Face::InitData>& aFaces);

  // Reads the raw bytes of a bundle file (aFile is a Face's mDescriptor, i.e.
  // the file name under <GRE>/fonts) into aData. Cached: the same buffer is
  // shared across all faces of a .ttc. Returns false if the file is unreadable.
  bool ReadFaceData(const nsACString& aFile, nsTArray<uint8_t>& aData);

  // Returns the PostScript name of the face at (aFile, aIndex) via aOut, or
  // false if unknown. macOS CreateFontEntry uses this to select the correct
  // face from CTFontManagerCreateFontDescriptorsFromData by PostScript name
  // (kCTFontNameAttribute) rather than trusting the descriptor array order,
  // which Apple does not document as matching the .ttc's face order.
  bool GetPsname(const nsACString& aFile, uint16_t aIndex, nsACString& aOut);

  // Vertical metrics for the face at (aFile, aIndex), i.e. a Face's
  // mDescriptor + mIndex. False if unknown or if the manifest predates v2, in
  // which case the caller must NOT touch the metrics the backend computed.
  // gfxFont::SanitizeMetrics is the single consumer: it is the one function
  // all three backends call as the last step of metric population, so imposing
  // the values there covers DWrite, FreeType and CoreText at once.
  bool GetVMetrics(const nsACString& aFile, uint16_t aIndex,
                   StealthBundleVMetrics& aOut);

  // Canonical Windows font-substitute lookup, applied on EVERY OS.
  //
  // A name that is not a family can still resolve through Windows' substitute
  // table, and a detector reads the difference: FingerprintJS probes "HELV"
  // and "Small Fonts", and on the shipped firefox-18 both resolved on Windows
  // and neither on Linux, because gfxDWriteFontList read them from the HOST
  // REGISTRY with no bundle-only gate while Linux has no such registry.
  //
  // aName is matched case-insensitively; aOut receives the target family name.
  // False when the name is not an alias, which is the common case, so callers
  // must fall through to the ordinary family lookup.
  bool GetAlias(const nsACString& aName, nsACString& aOut);


  // True once the manifest declared at least one alias, i.e. a v3 manifest.
  // Callers use it to decide whether to suppress the platform's own
  // host-derived substitute table: with no aliases of our own, suppressing it
  // would remove a signal rather than make it uniform.
  bool HasAliases() const { return !mAliases.IsEmpty(); }

  // The common-fallback font names, in gfxWindowsPlatform's order, for EVERY
  // host. Appends to aOut:
  //   [prefix if aPrefersColor] + per-script + tail + [symbolish] + tailAfter
  //   + [prefix if !aPrefersColor]
  // aScript is the numeric mozilla::intl::Script value; aSymbolish is the
  // caller's Unicode classification (block + general category), which stays on
  // the C++ side because it is a property of the CHARACTER, not of the host's
  // font engine - only the NAMES were ever platform-dependent.
  //
  // Returns false when the manifest carries no fallback table (a pre-v4 file),
  // so the caller can fall back to the platform list instead of rendering with
  // an empty one.
  bool GetCommonFallback(int16_t aScript, bool aPrefersColor, bool aSymbolish,
                         nsTArray<const char*>& aOut) const;


 private:
  StealthBundleFontList() = default;
  ~StealthBundleFontList() = default;

  // Reads + parses the manifest into mFamilies + mFaces. Returns false on any
  // failure (missing file, malformed line); on failure the instance is left
  // empty and Get() returns nullptr.
  bool Load();

  nsTArray<mozilla::fontlist::Family::InitData> mFamilies;
  // family lookup key (lowercased display name) -> its faces (raw, copyable)
  nsTHashMap<nsCStringHashKey, nsTArray<StealthBundleFace>> mFaces;
  // file name -> its raw bytes, so the N faces of a .ttc share one read
  nsTHashMap<nsCStringHashKey, nsTArray<uint8_t>> mFileCache;
  // lowercased alias name -> target family display name
  nsTHashMap<nsCStringHashKey, nsCString> mAliases;

  // Common-fallback table, parsed from the P/S/T/Y/Z records. The names are
  // owned here and handed out as raw const char*, which is what
  // GetCommonFallbackFonts' callers already expect; the list outlives every
  // caller because this object is a process-lifetime singleton.
  nsTArray<nsCString> mFallbackPrefix;                        // P|
  nsTHashMap<nsUint32HashKey, nsTArray<nsCString>> mFallbackScript;  // S|
  nsTArray<nsCString> mFallbackTail;                          // T|
  nsTArray<nsCString> mFallbackSymbolish;                     // Y|
  nsTArray<nsCString> mFallbackTailAfter;                     // Z|
};

#endif  // StealthBundleFontList_h_
