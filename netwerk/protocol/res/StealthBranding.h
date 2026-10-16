/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthBranding_h
#define mozilla_net_StealthBranding_h

#include "nsError.h"
#include "nsStringFwd.h"

class nsIURI;

namespace mozilla::net {

/**
 * The branding a page can read is DECLARED by invisible_core.
 *
 * A page reaches the branding package's content directory two ways:
 * chrome://branding/content/ (the package is contentaccessible) and
 * resource://usercontext-content/branding/ (that host maps the whole of
 * browser/content/, contentaccessible too). What it reads there - the about
 * dialog's stylesheet rules, the wordmarks' boxes, the images' sizes - tells a
 * build from the retail one. The set of answers is finite and known (the brand
 * directories this build ships), so by engine rule 7 which one a page sees is a
 * declaration of invisible_core, `zoom.stealth.branding.set`, and the engine
 * serves it.
 *
 * Both ways end in the same place: a URI under the branding package's content
 * directory. So the redirect is one function called at the two places that
 * resolve those URIs - nsChromeRegistry::ConvertChromeURL and
 * SubstitutingProtocolHandler::ResolveURI - instead of one rule per scheme that
 * could drift apart. A page comparing the two ways gets the same set by
 * construction.
 *
 * The declared sets live OUTSIDE browser/content/, in
 * chrome/browser/stealth-branding/<set>/ (browser/branding/stealth/jar.mn),
 * because everything under browser/content/ is readable by a page through
 * resource://usercontext-content/: a second set there would be a file a page
 * can load, and a file's mere existence is visible (load against error).
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
 * package is kept: its directory, and the sets directory derived from it. A
 * registry reset re-registers every package, so the last registration wins and
 * nothing needs clearing.
 */
void StealthBrandingRegisterPackage(const nsACString& aPackage, nsIURI* aBase);

/**
 * Rewrites a resolved URI spec that points into the branding package's content
 * directory so that it points into the declared set instead. Leaves every other
 * spec untouched. Returns NS_ERROR_FILE_NOT_FOUND - a failed load, never the
 * build's own brand - when the engine is on and the declaration is missing or
 * malformed. Safe on any thread: ResolveURI is called off the main thread too.
 */
nsresult StealthBrandingRedirect(nsACString& aResolvedSpec);

/**
 * Whether a declared set name is well formed: lowercase letters, digits and
 * underscores, at least one. It becomes a path segment, so anything else - a
 * slash, a dot - is refused rather than resolved. The declaration gate uses this
 * same predicate, so "declared" means the same thing in both places.
 */
bool StealthBrandingIsSetName(const nsACString& aName);

}  // namespace mozilla::net

#endif  // mozilla_net_StealthBranding_h
