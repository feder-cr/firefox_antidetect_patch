/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthFontDescriptorPath.h"

#include <limits.h>
#include <stdlib.h>

#include "ScaledFontFontconfig.h"
#include "mozilla/RefPtr.h"
#include "nsCOMPtr.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsIFile.h"
#include "nsString.h"

namespace mozilla::gfx {

static bool Canonical(const std::string& aPath, std::string& aOut) {
  char resolved[PATH_MAX];
  if (aPath.empty() || !realpath(aPath.c_str(), resolved)) {
    return false;
  }
  aOut = resolved;
  return true;
}

bool StealthHandlesFontDescriptors() { return true; }

bool StealthBundleDescriptorPath(const uint8_t* aData, uint32_t aLen,
                                 const std::string& aBundleDir,
                                 std::string& aOut) {
  if (!aData || aLen == 0 || aBundleDir.empty()) {
    return false;
  }
  std::string dir;
  std::string file;
  if (!Canonical(aBundleDir, dir) ||
      !Canonical(std::string(reinterpret_cast<const char*>(aData), aLen),
                 file)) {
    return false;
  }
  if (dir.back() != '/') {
    dir += '/';
  }
  // Strictly inside: the directory itself is not a font.
  if (file.size() <= dir.size() || file.compare(0, dir.size(), dir) != 0) {
    return false;
  }
  aOut = file;
  return true;
}

already_AddRefed<UnscaledFont> StealthUnscaledFontFromBundleDir(
    const std::string& aBundleDir, const uint8_t* aData, uint32_t aLen,
    uint32_t aIndex) {
  if (aIndex & 0x80000000u) {
    return nullptr;
  }
  std::string path;
  if (!StealthBundleDescriptorPath(aData, aLen, aBundleDir, path)) {
    return nullptr;
  }
  RefPtr<UnscaledFont> font =
      MakeRefPtr<UnscaledFontFontconfig>(path.c_str(), aIndex);
  return font.forget();
}

already_AddRefed<UnscaledFont> StealthUnscaledFontFromDescriptor(
    const uint8_t* aData, uint32_t aLen, uint32_t aIndex) {
  nsCOMPtr<nsIFile> dir;
  nsAutoCString native;
  if (NS_FAILED(NS_GetSpecialDirectory(NS_GRE_DIR, getter_AddRefs(dir))) ||
      NS_FAILED(dir->Append(u"fonts"_ns)) ||
      NS_FAILED(dir->GetNativePath(native))) {
    return nullptr;
  }
  return StealthUnscaledFontFromBundleDir(std::string(native.get()), aData,
                                          aLen, aIndex);
}

}  // namespace mozilla::gfx
