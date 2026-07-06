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
                nsTArray<mozilla::fontlist::Face::InitData>& aFaces) const;

 private:
  StealthBundleFontList() = default;
  ~StealthBundleFontList() = default;

  // Reads + parses the manifest into mFamilies + mFaces. Returns false on any
  // failure (missing file, malformed line); on failure the instance is left
  // empty and Get() returns nullptr.
  bool Load();

  nsTArray<mozilla::fontlist::Family::InitData> mFamilies;
  // family lookup key (lowercased display name) -> its faces
  nsTHashMap<nsCStringHashKey, nsTArray<mozilla::fontlist::Face::InitData>>
      mFaces;
};

#endif  // StealthBundleFontList_h_
