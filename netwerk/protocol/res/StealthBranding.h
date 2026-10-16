/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthBranding_h
#define mozilla_net_StealthBranding_h

#include "mozilla/AlreadyAddRefed.h"
#include "nsError.h"
#include "nsStringFwd.h"

class nsIFile;
class nsIURI;

namespace mozilla::net {

/**
 * The branding a page can read is DECLARED by invisible_core, files and all.
 *
 * A page reaches the branding package's content directory two ways:
 * chrome://branding/content/ (the package is contentaccessible) and
 * resource://usercontext-content/branding/ (that host maps the whole of
 * browser/content/, contentaccessible too). What it reads there - the about
 * dialog's stylesheet rules, the wordmarks' boxes, the images' sizes - tells a
 * build from the retail one. By engine rule 7 what a page reads lives in
 * invisible_core and nowhere else, so the core ships the files (the retail
 * brand, ours, or a directory of the caller's own) and declares where they
 * are, `zoom.stealth.branding.dir`, a file: URI of a directory. The engine
 * keeps no stealth copy of any of them and serves the declared ones.
 *
 * Both ways end in the same place: a URI under the branding package's content
 * directory. So the redirect is one function called at the two places that
 * resolve those URIs - nsChromeRegistry::ConvertChromeURL and
 * SubstitutingProtocolHandler::ResolveURI - instead of one rule per scheme that
 * could drift apart. A page comparing the two ways gets the same files by
 * construction.
 *
 * Engine off (no seed) is stock Firefox and serves the build's own brand: that
 * is the absence of a declaration, not an incomplete one. Engine on and nothing
 * or something malformed declared refuses: the parent's declaration gate stops
 * the launch, and a load that still gets here fails rather than serving the
 * build's own brand, which would be the compiled fallback rule 7 forbids.
 */

/**
 * Called by the chrome registry, in the parent and in every content process,
 * whenever a package's content directory is registered. Only the branding
 * package is kept. A registry reset re-registers every package, so the last
 * registration wins and nothing needs clearing.
 */
void StealthBrandingRegisterPackage(const nsACString& aPackage, nsIURI* aBase);

/**
 * Rewrites a resolved URI spec that points into the branding package's content
 * directory so that it points into the declared directory instead. Leaves every
 * other spec untouched. Returns NS_ERROR_FILE_NOT_FOUND - a failed load, never
 * the build's own brand - when the engine is on and the declaration is missing
 * or malformed. Safe on any thread: ResolveURI is called off the main thread
 * too.
 */
nsresult StealthBrandingRedirect(nsACString& aResolvedSpec);

/**
 * Whether a declaration is well formed: a file: URI of a directory, i.e.
 * starting with "file:///" and ending with '/'. The redirect checks only this,
 * on any thread; everything else is checked once, in the parent, through
 * StealthBrandingDeclaredDir.
 */
bool StealthBrandingIsDeclaredDir(const nsACString& aSpec);

/**
 * The declared directory, or null when the engine is off or the declaration
 * is not usable: malformed, not an existing directory, or holding anything but
 * regular files. Parent process. Two readers, and they must agree, which is why
 * it is one function: the declaration gate refuses the launch when it is null,
 * and the content sandbox grants READ access to exactly this directory - the
 * files live wherever invisible_core keeps them, which the sandbox would
 * otherwise keep closed to the content processes that load them. "Only regular
 * files, no subdirectory" bounds what that grant opens.
 */
already_AddRefed<nsIFile> StealthBrandingDeclaredDir();

}  // namespace mozilla::net

#endif  // mozilla_net_StealthBranding_h
