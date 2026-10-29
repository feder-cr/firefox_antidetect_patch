/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "StealthOwnBinaries.h"

#include <iterator>
#include <windows.h>

#include "ModuleVersionInfo.h"
#include "mozilla/UntrustedModulesData.h"
#include "nsString.h"
#include "nsUnicharUtils.h"

namespace mozilla {

// STEALTH: whether aDllPath lies in the directory of aExePath (or below),
// case-insensitively. GetVersionAndVendorInfo skips the signature lookup for
// those modules that also carry firefox.exe's version (this build's own;
// the signed C++ runtime beside them keeps it): they carry no Authenticode
// signature, so the lookup always fell through to the catalog search, which
// hashes the whole file and walks every system catalog - 1.26 s of CPU per
// launch in the "Untrusted Modules" thread, on a 2026-10-27 profile, for an
// answer that cannot exist. GetTrust then classifies them as it already did:
// FirefoxDirectory (+AndVersion), from their path and version resource.
// Declared in UntrustedModulesData.h.
/* static */
bool ModuleRecord::IsInDirectoryOf(const nsAString& aDllPath,
                                   const nsAString& aExePath) {
  const int32_t slash = aExePath.RFindChar(u'\\');
  if (slash <= 0) {
    return false;
  }
  const nsDependentSubstring dir = Substring(aExePath, 0, slash + 1);
  return StringBeginsWith(aDllPath, dir, nsCaseInsensitiveStringComparator);
}

namespace {

// STEALTH: one of this build's own binaries - beside firefox.exe and with its
// version - whose signature lookup can only fall through to the catalog
// search (see IsInDirectoryOf). Anything else in the directory, such as the
// signed C++ runtime, keeps the lookup and its classification.
bool IsOwnUnsignedBinary(const nsAString& aPath,
                         const ModuleVersionInfo& aVerInfo) {
  wchar_t exePath[MAX_PATH * 4];
  const DWORD exeLen =
      ::GetModuleFileNameW(nullptr, exePath, std::size(exePath));
  if (exeLen == 0 || exeLen >= std::size(exePath)) {
    return false;
  }
  const nsDependentString exe(exePath, exeLen);
  if (!ModuleRecord::IsInDirectoryOf(aPath, exe)) {
    return false;
  }
  ModuleVersionInfo exeVer;
  return exeVer.GetFromImage(exe) && aVerInfo.mFileVersion.Version64() &&
         exeVer.mFileVersion.Version64() == aVerInfo.mFileVersion.Version64();
}

}  // namespace

bool StealthSkipsSignatureLookup(const nsAString& aPath,
                                 const ModuleVersionInfo& aVerInfo,
                                 bool aHasVersion) {
  return aHasVersion && IsOwnUnsignedBinary(aPath, aVerInfo);
}

}  // namespace mozilla
