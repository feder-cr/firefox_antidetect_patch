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
#include "nsIFile.h"
#include "nsIInputStream.h"
#include "nsNetUtil.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsReadableUtils.h"
#include "mozilla/StaticPtr.h"

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

static WeightRange ParseWeight(const nsACString& aMin, const nsACString& aMax) {
  nsresult rv1, rv2;
  int32_t lo = nsCString(aMin).ToInteger(&rv1);
  int32_t hi = nsCString(aMax).ToInteger(&rv2);
  if (NS_FAILED(rv1)) lo = 400;
  if (NS_FAILED(rv2)) hi = lo;
  return WeightRange(FontWeight::FromInt(lo), FontWeight::FromInt(hi));
}

static StretchRange ParseStretch(const nsACString& aMin,
                                 const nsACString& aMax) {
  nsresult rv1, rv2;
  float lo = nsCString(aMin).ToFloat(&rv1);
  float hi = nsCString(aMax).ToFloat(&rv2);
  if (NS_FAILED(rv1)) lo = 100.0f;
  if (NS_FAILED(rv2)) hi = lo;
  return StretchRange(FontStretch::FromFloat(lo), FontStretch::FromFloat(hi));
}

static SlantStyleRange ParseStyle(const nsACString& aStyle) {
  return aStyle.EqualsLiteral("italic")
             ? SlantStyleRange(FontSlantStyle::ITALIC)
             : SlantStyleRange(FontSlantStyle::NORMAL);
}

bool StealthBundleFontList::Load() {
  nsCOMPtr<nsIFile> file;
  if (NS_FAILED(NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(file)))) {
    return false;
  }
  if (NS_FAILED(file->Append(u"fonts"_ns)) ||
      NS_FAILED(file->Append(u"bundle-fonts.manifest"_ns))) {
    return false;
  }
  nsCOMPtr<nsIInputStream> stream;
  if (NS_FAILED(NS_NewLocalFileInputStream(getter_AddRefs(stream), file))) {
    return false;
  }
  nsCString data;
  if (NS_FAILED(NS_ConsumeStream(stream, UINT32_MAX, data))) {
    return false;
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
      mFaces.GetOrInsertNew(key);
      continue;
    }
    if (StringBeginsWith(line, "f|"_ns)) {
      // f|<file>|<index>|<w_min>|<w_max>|<stretch_min>|<stretch_max>|<style>|<psname>
      if (SplitPipe(line, fields) < 9 || currentKey.IsEmpty()) {
        continue;  // malformed / orphan face - skip, never crash
      }
      Face::InitData face;
      face.mDescriptor = fields[1];  // bundle file name
      nsresult rv;
      face.mIndex = uint16_t(nsCString(fields[2]).ToInteger(&rv));
      if (NS_FAILED(rv)) face.mIndex = 0;
#ifdef MOZ_WIDGET_GTK
      face.mSize = 0;  // scalable
#endif
      face.mFixedPitch = false;  // manifest does not carry it yet
      face.mWeight = ParseWeight(fields[3], fields[4]);
      face.mStretch = ParseStretch(fields[5], fields[6]);
      face.mStyle = ParseStyle(fields[7]);
      face.mCharMap = nullptr;
      if (auto* faces = mFaces.GetValue(currentKey)) {
        faces->AppendElement(std::move(face));
      }
    }
  }

  return !mFamilies.IsEmpty();
}

void StealthBundleFontList::GetFaces(
    const nsACString& aFamilyName,
    nsTArray<Face::InitData>& aFaces) const {
  nsAutoCString key(aFamilyName);
  ToLowerCase(key);
  if (const auto* faces = mFaces.GetValue(key)) {
    aFaces.AppendElements(*faces);
  }
}

/* static */
StealthBundleFontList* StealthBundleFontList::Get() {
  static StaticAutoPtr<StealthBundleFontList> sInstance;
  static bool sTried = false;
  if (!sTried) {
    sTried = true;
    auto* list = new StealthBundleFontList();
    if (list->Load()) {
      sInstance = list;
    } else {
      delete list;
    }
  }
  return sInstance.get();
}
