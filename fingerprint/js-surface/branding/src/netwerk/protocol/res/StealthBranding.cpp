/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/net/StealthBranding.h"

#include "StealthDeclarationGate.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/UniquePtr.h"
#include "nsIFile.h"
#include "nsIURI.h"
#include "nsNetUtil.h"
#include "nsString.h"
#include "nsZipArchive.h"

namespace mozilla::net {

namespace {

// The package whose content directory is the branding a page can read.
constexpr auto kBrandingPackage = "branding"_ns;

StaticMutex sMutex MOZ_UNANNOTATED;
StaticAutoPtr<nsCString> sBrandingDir;  // ends with '/'

}  // namespace

bool StealthBrandingIsDeclaredArchive(const nsACString& aSpec) {
  return StringBeginsWith(aSpec, "file:///"_ns) &&
         !StringEndsWith(aSpec, "/"_ns) && aSpec.FindChar('\\') == kNotFound &&
         aSpec.FindChar('!') == kNotFound;
}

already_AddRefed<nsIFile> StealthBrandingDeclaredArchive() {
  if (!gfx::StealthEngineActive()) {
    return nullptr;
  }
  nsAutoCString declared;
  {
    auto lock = StaticPrefs::zoom_stealth_branding_archive();
    declared = *lock;
  }
  if (!StealthBrandingIsDeclaredArchive(declared)) {
    return nullptr;
  }
  nsCOMPtr<nsIFile> archive;
  bool isFile = false, isLink = false;
  if (NS_FAILED(NS_GetFileFromURLSpec(declared, getter_AddRefs(archive))) ||
      NS_FAILED(archive->IsFile(&isFile)) || !isFile ||
      NS_FAILED(archive->IsSymlink(&isLink)) || isLink) {
    return nullptr;
  }
  RefPtr<nsZipArchive> zip = nsZipArchive::OpenArchive(archive);
  if (!zip) {
    return nullptr;
  }
  nsZipFind* rawFind = nullptr;
  if (NS_FAILED(zip->FindInit(nullptr, &rawFind))) {
    return nullptr;
  }
  UniquePtr<nsZipFind> find(rawFind);
  const char* name = nullptr;
  uint16_t nameLength = 0;
  uint32_t entries = 0;
  while (NS_SUCCEEDED(find->FindNext(&name, &nameLength))) {
    // Only files at the root: a directory entry or a nested path is something
    // the declaration does not describe, and a page could load under it.
    if (!nameLength || nsDependentCSubstring(name, nameLength).FindChar('/') !=
                           kNotFound) {
      return nullptr;
    }
    ++entries;
  }
  if (!entries) {
    return nullptr;
  }
  return archive.forget();
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
    auto lock = StaticPrefs::zoom_stealth_branding_archive();
    declared = *lock;
  }
  if (!StealthBrandingIsDeclaredArchive(declared)) {
    // In the parent this refuses the launch. In a content process the gate does
    // not run, and the load fails: serving the build's own brand here would be
    // exactly the compiled fallback engine rule 7 forbids.
    gfx::StealthAssertDeclarationsComplete();
    return NS_ERROR_FILE_NOT_FOUND;
  }

  // The rest of the path names an entry of the declared archive. Inside a jar
  // a "../" can only wander through the archive, never out of it, but the URL
  // parsers above coalesce dot segments already, so none should arrive here -
  // and a stray one is refused rather than interpreted: this is the boundary,
  // and it does not rely on its callers.
  const nsDependentCSubstring rest(aResolvedSpec, brandingLength);
  nsAutoCString lowered(rest);
  ToLowerCase(lowered);
  if (lowered.Find("..") != kNotFound || lowered.Find("%2e") != kNotFound ||
      lowered.Find("%5c") != kNotFound || lowered.Find("%2f") != kNotFound ||
      lowered.FindChar('\\') != kNotFound || lowered.FindChar('/') != kNotFound) {
    return NS_ERROR_FILE_NOT_FOUND;
  }

  // Served as jar:, the channel class the retail serves its own branding with
  // out of omni.ja. A file: channel is a different class: it keeps the type a
  // loader suggests before opening (the CSS loader's text/css, the script
  // loader's), where a jar channel replaces it with the entry's own type on
  // open - so a page loading a PNG as a script or a stylesheet saw it load on
  // a file: branding and fail on the retail (measured on 636cea1: 62 loads,
  // every file, both ways in).
  aResolvedSpec.Replace(0, brandingLength,
                        "jar:"_ns + declared + "!/"_ns);
  return NS_OK;
}

}  // namespace mozilla::net
