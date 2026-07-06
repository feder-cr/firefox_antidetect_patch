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
      continue;
    }
    if (StringBeginsWith(line, "f|"_ns)) {
      // f|<file>|<index>|<w_min>|<w_max>|<stretch_min>|<stretch_max>|<style>|<psname>
      if (SplitPipe(line, fields) < 9 || currentKey.IsEmpty()) {
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

bool StealthBundleFontList::ReadFaceData(const nsACString& aFile,
                                         nsTArray<uint8_t>& aData) {
  nsCString file(aFile);
  if (auto cached = mFileCache.Lookup(file)) {
    aData.AppendElements(cached.Data());
    return true;
  }
  nsCOMPtr<nsIFile> path;
  if (NS_FAILED(NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(path))) ||
      NS_FAILED(path->Append(u"fonts"_ns)) ||
      NS_FAILED(path->Append(NS_ConvertUTF8toUTF16(file)))) {
    return false;
  }
  nsCOMPtr<nsIInputStream> stream;
  if (NS_FAILED(NS_NewLocalFileInputStream(getter_AddRefs(stream), path))) {
    return false;
  }
  nsCString bytes;
  if (NS_FAILED(NS_ConsumeStream(stream, UINT32_MAX, bytes))) {
    return false;
  }
  nsTArray<uint8_t> buf;
  buf.AppendElements(reinterpret_cast<const uint8_t*>(bytes.get()),
                     bytes.Length());
  aData.AppendElements(buf);
  mFileCache.InsertOrUpdate(file, std::move(buf));
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
    return nullptr;
  }();
  return sInstance;
}
