/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StealthBundleFontList_h_
#define StealthBundleFontList_h_

#include "SharedFontList.h"
#include "mozilla/MemoryReporting.h"
#include "nsTArray.h"
#include "prio.h"
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
  // NEVER RETURNS NULL. If the manifest cannot be loaded from any of its three
  // sources this crashes, and that is the contract every caller depends on.
  //
  // It used to return nullptr, and this comment used to say the caller "then
  // falls back to an empty bundle-only list - never to host fonts". That was
  // the intent and not the behaviour: all thirteen call sites are written
  // `if (auto* bundle = Get())` and simply skip their work, so one load failure
  // silently handed enumeration, vertical metrics, aliases, the per-script
  // fallback table and the generic families back to whatever the machine has,
  // with the browser still running and rendering. The comment described the
  // design; the code did something else, which is why the rule is to read the
  // literal the code compares and never the sentence beside it.
  //
  // Those null checks are gone as of 2026-08-08: eleven of the thirteen were
  // unreachable and were removed, and the two that survive in
  // gfxDWriteFontList test `mStealthBundleOnly ? Get() : nullptr`, where the
  // null comes from the FLAG and not from this function. The invariant lives
  // HERE, at the one place that owns it, so that restoring a nullable Get()
  // cannot quietly reopen them: it has to break this contract first.
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

  // The bytes of one bundle file, read once and SHARED. Refcounted because the
  // readers are more than one - every face of a .ttc, and the platform stream
  // the rasteriser keeps alive for as long as the font exists - and because a
  // refcounted object never moves: the cache below stores RefPtrs, so rehashing
  // it cannot invalidate a pointer someone is already reading through.
  // MAPPED, not read: the pages are file-backed, so the operating system shares
  // them between every process that maps the same file and can evict them under
  // pressure instead of swapping them. Measured with the reporter below on
  // 2026-08-14, before mapping: the same bundle cost 185.5 MB in the content
  // process AND 31.6 MB in the parent, 224.2 MB in all - 20.4% of the browser's
  // whole unique memory, held twice because a heap read is private per process.
  //
  // The heap read stays as a fallback and this is NOT the "second source of
  // truth" rule 7 forbids: that rule is about a declared VALUE having one home.
  // Here both paths deliver the same bytes of the same file, so nothing
  // observable can differ; what differs is where the pages live.
  class FileBlob final {
   public:
    NS_INLINE_DECL_THREADSAFE_REFCOUNTING(FileBlob)

    explicit FileBlob(nsTArray<uint8_t>&& aBytes) : mBytes(std::move(aBytes)) {}
    FileBlob(PRFileMap* aMap, void* aAddr, uint32_t aLength)
        : mMap(aMap), mMapped(static_cast<const uint8_t*>(aAddr)),
          mMappedLength(aLength) {}

    const uint8_t* Data() const { return mMapped ? mMapped : mBytes.Elements(); }
    uint32_t Length() const { return mMapped ? mMappedLength : mBytes.Length(); }

    // Bytes charged to THIS process's heap: zero when mapped, because the pages
    // are the file's and about:memory must not count them as ours.
    size_t HeapBytes() const { return mMapped ? 0 : mBytes.Length(); }
    size_t MappedBytes() const { return mMapped ? mMappedLength : 0; }

   private:
    ~FileBlob();
    const nsTArray<uint8_t> mBytes;
    PRFileMap* mMap = nullptr;
    const uint8_t* mMapped = nullptr;
    uint32_t mMappedLength = 0;
  };

  // How many bytes this object holds ON THE HEAP, for about:memory. Zero for a
  // mapped file: see FileBlob above for why counting those here would be wrong.
  size_t SizeOfIncludingThis(mozilla::MallocSizeOf aMallocSizeOf) const;

  // How many bytes are MAPPED, reported separately and as non-heap, so the two
  // never get added together by accident.
  size_t MappedBytes() const;

  // The absolute path of a bundle file. The ONE place that knows where the
  // bundled fonts live: GetFaceBlob resolves through it too, so a caller that
  // wants the path and one that wants the bytes cannot disagree about the
  // directory. False if the path cannot be built.
  static bool GetFacePath(const nsACString& aFile, nsACString& aOut);

  // The shared bytes of a bundle file (aFile is a Face's mDescriptor, i.e. the
  // file name under <GRE>/fonts), or null if unreadable. Read once and cached
  // for the life of the process.
  //
  // Prefer this to ReadFaceData: it hands out the ONE copy instead of making a
  // new one. The bundle is 218.7 MB across 126 files and the largest is a
  // 35.4 MB .ttc with two faces, so a copy per face is not a rounding error.
  already_AddRefed<FileBlob> GetFaceBlob(const nsACString& aFile);

  // Reads the raw bytes of a bundle file into aData. Kept for the callers that
  // need to OWN their bytes (the FreeType and CoreText paths hand ownership to
  // the platform object); it is a copy of what GetFaceBlob returns.
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

  // Maps aPath read-only, or null if it cannot be mapped. The descriptor is
  // closed straight away: NSPR keeps what the mapping needs.
  static already_AddRefed<FileBlob> MapFile(nsIFile* aPath);

  nsTArray<mozilla::fontlist::Family::InitData> mFamilies;
  // family lookup key (lowercased display name) -> its faces (raw, copyable)
  nsTHashMap<nsCStringHashKey, nsTArray<StealthBundleFace>> mFaces;
  // file name -> its bytes, so the N faces of a .ttc share ONE copy. Never
  // cleared: this object is deliberately leaked and lives for the process, which
  // is what lets a borrowed pointer into a blob stay valid without a keepalive.
  nsTHashMap<nsCStringHashKey, RefPtr<FileBlob>> mFileCache;
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
