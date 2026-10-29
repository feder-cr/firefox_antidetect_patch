/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StealthOwnBinaries_h
#define mozilla_StealthOwnBinaries_h

#include "nsStringFwd.h"

namespace mozilla {

class ModuleVersionInfo;

// STEALTH: whether aDllPath lies in the directory of aExePath (or below),
// case-insensitively. GetVersionAndVendorInfo skips the signature lookup for
// those modules that also carry firefox.exe's version (this build's own;
// the signed C++ runtime beside them keeps it): they carry no Authenticode
// signature, so the lookup always fell through to the catalog search, which
// hashes the whole file and walks every system catalog - 1.26 s of CPU per
// launch in the "Untrusted Modules" thread, on a 2026-10-27 profile, for an
// answer that cannot exist. GetTrust then classifies them as it already did:
// FirefoxDirectory (+AndVersion), from their path and version resource.
//
// ModuleRecord::IsInDirectoryOf (declared in UntrustedModulesData.h, defined in
// StealthOwnBinaries.cpp) is the path test; this is the whole decision.
//
// True when the signature lookup of the module at aPath must be skipped: it
// has a version resource (aHasVersion, the result of aVerInfo.GetFromImage)
// and is one of this build's own binaries.
bool StealthSkipsSignatureLookup(const nsAString& aPath,
                                 const ModuleVersionInfo& aVerInfo,
                                 bool aHasVersion);

}  // namespace mozilla

#endif  // mozilla_StealthOwnBinaries_h
