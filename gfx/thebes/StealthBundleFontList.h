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
struct StealthBundleFace {
  nsCString mFile;
  uint16_t mIndex = 0;
  int32_t mWeightMin = 400;
  int32_t mWeightMax = 400;
  float mStretchMin = 100.0f;
  float mStretchMax = 100.0f;
  bool mItalic = false;
};

/**
 * Reads <GRE>/fonts/bundle-fonts.manifest (generated offline by
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
};

#endif  // StealthBundleFontList_h_
