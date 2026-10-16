/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/net/StealthBranding.h"

#include "StealthDeclarationGate.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "nsIURI.h"
#include "nsString.h"

namespace mozilla::net {

namespace {

// The package whose content directory is the branding a page can read.
constexpr auto kBrandingPackage = "branding"_ns;

// Where the declared sets are, relative to that directory: two levels up, out
// of browser/content/ and into chrome/browser/stealth-branding/. The other half
// of this fact is browser/branding/stealth/jar.mn, which packages them there; a
// set that is not where this says shows up as every branding load failing,
// which the page-surface probe reads at once.
constexpr auto kSetsFromBranding = "../../stealth-branding/"_ns;

StaticMutex sMutex MOZ_UNANNOTATED;
StaticAutoPtr<nsCString> sBrandingDir;  // ends with '/'
StaticAutoPtr<nsCString> sSetsDir;      // ends with '/'

}  // namespace

bool StealthBrandingIsSetName(const nsACString& aName) {
  if (aName.IsEmpty()) {
    return false;
  }
  for (char c : aName) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

void StealthBrandingRegisterPackage(const nsACString& aPackage, nsIURI* aBase) {
  if (!aPackage.Equals(kBrandingPackage)) {
    return;
  }
  nsAutoCString branding, sets;
  // A branding package that is not a directory we can rewrite under is a
  // defect of THIS build's manifest, not a runtime condition, and the only
  // quiet way past it would be to stop recognising branding URIs - which with
  // the engine on serves the build's own brand, the compiled fallback rule 7
  // forbids. So it stops here, on every launch, where nobody can miss it.
  MOZ_RELEASE_ASSERT(aBase && NS_SUCCEEDED(aBase->GetSpec(branding)) &&
                         StringEndsWith(branding, "/"_ns) &&
                         NS_SUCCEEDED(aBase->Resolve(kSetsFromBranding, sets)),
                     "stealth: the branding package is not a directory");
  StaticMutexAutoLock lock(sMutex);
  sBrandingDir = new nsCString(branding);
  sSetsDir = new nsCString(sets);
}

nsresult StealthBrandingRedirect(nsACString& aResolvedSpec) {
  if (!gfx::StealthEngineActive()) {
    return NS_OK;
  }

  nsAutoCString setsDir;
  size_t brandingLength;
  {
    StaticMutexAutoLock lock(sMutex);
    if (!sBrandingDir || !StringBeginsWith(aResolvedSpec, *sBrandingDir)) {
      return NS_OK;
    }
    brandingLength = sBrandingDir->Length();
    setsDir = *sSetsDir;
  }

  nsAutoCString declared;
  {
    auto lock = StaticPrefs::zoom_stealth_branding_set();
    declared = *lock;
  }
  if (!StealthBrandingIsSetName(declared)) {
    // In the parent this refuses the launch. In a content process the gate does
    // not run, and the load fails: serving the build's own brand here would be
    // exactly the compiled fallback engine rule 7 forbids.
    gfx::StealthAssertDeclarationsComplete();
    return NS_ERROR_FILE_NOT_FOUND;
  }

  setsDir.Append(declared);
  setsDir.Append('/');
  aResolvedSpec.Replace(0, brandingLength, setsDir);
  return NS_OK;
}

}  // namespace mozilla::net
