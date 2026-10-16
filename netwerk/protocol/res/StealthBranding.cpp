/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/net/StealthBranding.h"

#include "StealthDeclarationGate.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "nsIDirectoryEnumerator.h"
#include "nsIFile.h"
#include "nsIURI.h"
#include "nsNetUtil.h"
#include "nsString.h"

namespace mozilla::net {

namespace {

// The package whose content directory is the branding a page can read.
constexpr auto kBrandingPackage = "branding"_ns;

StaticMutex sMutex MOZ_UNANNOTATED;
StaticAutoPtr<nsCString> sBrandingDir;  // ends with '/'

}  // namespace

bool StealthBrandingIsDeclaredDir(const nsACString& aSpec) {
  return StringBeginsWith(aSpec, "file:///"_ns) &&
         StringEndsWith(aSpec, "/"_ns) && aSpec.FindChar('\\') == kNotFound;
}

already_AddRefed<nsIFile> StealthBrandingDeclaredDir() {
  if (!gfx::StealthEngineActive()) {
    return nullptr;
  }
  nsAutoCString declared;
  {
    auto lock = StaticPrefs::zoom_stealth_branding_dir();
    declared = *lock;
  }
  if (!StealthBrandingIsDeclaredDir(declared)) {
    return nullptr;
  }
  nsCOMPtr<nsIFile> dir;
  bool isDir = false;
  if (NS_FAILED(NS_GetFileFromURLSpec(declared, getter_AddRefs(dir))) ||
      NS_FAILED(dir->IsDirectory(&isDir)) || !isDir) {
    return nullptr;
  }
  nsCOMPtr<nsIDirectoryEnumerator> entries;
  if (NS_FAILED(dir->GetDirectoryEntries(getter_AddRefs(entries)))) {
    return nullptr;
  }
  nsCOMPtr<nsIFile> entry;
  while (NS_SUCCEEDED(entries->GetNextFile(getter_AddRefs(entry))) && entry) {
    bool isFile = false, isLink = false;
    if (NS_FAILED(entry->IsFile(&isFile)) || !isFile ||
        NS_FAILED(entry->IsSymlink(&isLink)) || isLink) {
      return nullptr;
    }
  }
  return dir.forget();
}

void StealthBrandingRegisterPackage(const nsACString& aPackage, nsIURI* aBase) {
  if (!aPackage.Equals(kBrandingPackage)) {
    return;
  }
  nsAutoCString branding;
  // A branding package that is not a directory we can rewrite under is a
  // defect of THIS build's manifest, not a runtime condition, and the only
  // quiet way past it would be to stop recognising branding URIs - which with
  // the engine on serves the build's own brand, the compiled fallback rule 7
  // forbids. So it stops here, on every launch, where nobody can miss it.
  MOZ_RELEASE_ASSERT(aBase && NS_SUCCEEDED(aBase->GetSpec(branding)) &&
                         StringEndsWith(branding, "/"_ns),
                     "stealth: the branding package is not a directory");
  StaticMutexAutoLock lock(sMutex);
  sBrandingDir = new nsCString(branding);
}

nsresult StealthBrandingRedirect(nsACString& aResolvedSpec) {
  if (!gfx::StealthEngineActive()) {
    return NS_OK;
  }

  size_t brandingLength;
  {
    StaticMutexAutoLock lock(sMutex);
    if (!sBrandingDir || !StringBeginsWith(aResolvedSpec, *sBrandingDir)) {
      return NS_OK;
    }
    brandingLength = sBrandingDir->Length();
  }

  nsAutoCString declared;
  {
    auto lock = StaticPrefs::zoom_stealth_branding_dir();
    declared = *lock;
  }
  if (!StealthBrandingIsDeclaredDir(declared)) {
    // In the parent this refuses the launch. In a content process the gate does
    // not run, and the load fails: serving the build's own brand here would be
    // exactly the compiled fallback engine rule 7 forbids.
    gfx::StealthAssertDeclarationsComplete();
    return NS_ERROR_FILE_NOT_FOUND;
  }

  // The rest of the path becomes a path on disk, under a directory a web page
  // can reach. Inside omni.ja a "../" could only wander through the archive;
  // under a file: base it could leave the declared directory and read the
  // machine. The URL parsers above coalesce dot segments already, so none
  // should arrive here - which is exactly why a stray one is refused rather
  // than trusted: this is the boundary, and it does not rely on its callers.
  const nsDependentCSubstring rest(aResolvedSpec, brandingLength);
  nsAutoCString lowered(rest);
  ToLowerCase(lowered);
  if (lowered.Find("..") != kNotFound || lowered.Find("%2e") != kNotFound ||
      lowered.Find("%5c") != kNotFound || lowered.Find("%2f") != kNotFound ||
      lowered.FindChar('\\') != kNotFound) {
    return NS_ERROR_FILE_NOT_FOUND;
  }

  aResolvedSpec.Replace(0, brandingLength, declared);
  return NS_OK;
}

}  // namespace mozilla::net
