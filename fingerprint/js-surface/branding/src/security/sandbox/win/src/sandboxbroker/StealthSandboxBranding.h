/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StealthSandboxBranding_h
#define mozilla_StealthSandboxBranding_h

// Included by sandboxBroker.cpp only (a SOURCES entry, not unified), through
// the STEALTH-HOOK(fingerprint/js-surface/branding) include.

#include "mozilla/net/StealthBranding.h"
#include "nsCOMPtr.h"
#include "nsDebug.h"
#include "nsIFile.h"
#include "nsString.h"
#include "sandbox/win/src/sandbox_policy.h"

namespace mozilla {

/**
 * Gives the content processes READ access to exactly the declared branding
 * archive.
 *
 * The branding a page reads is declared by invisible_core, which keeps the
 * archive wherever it is installed. The content processes load from it, so they
 * get READ access to exactly that file - the same one the declaration gate
 * accepted, through the same function (StealthBrandingDeclaredArchive, see
 * netwerk/protocol/res/StealthBranding.h).
 *
 * Nothing is added when the engine is off or the declaration is not usable
 * (StealthBrandingDeclaredArchive returns null), or when the archive's path
 * cannot be read. A rule that cannot be added is reported with NS_ERROR and is
 * not fatal, as before. Called from
 * SandboxBroker::SetSecurityLevelForContentProcess, in the branch that does not
 * already allow read access to everything, between the chrome and extensions
 * directory rules, so the order of the rules does not change.
 */
inline void AddDeclaredBrandingReadRule(sandbox::TargetConfig* aConfig) {
  if (nsCOMPtr<nsIFile> branding =
          mozilla::net::StealthBrandingDeclaredArchive()) {
    nsAutoString brandingRule;
    if (NS_SUCCEEDED(branding->GetPath(brandingRule))) {
      sandbox::ResultCode result = aConfig->AllowFileAccess(
          sandbox::FileSemantics::kAllowReadonly, brandingRule.get());
      if (sandbox::SBOX_ALL_OK != result) {
        NS_ERROR("Failed to add the declared branding read rule.");
      }
    }
  }
}

}  // namespace mozilla

#endif  // mozilla_StealthSandboxBranding_h
