/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthSessionResolver_h
#define mozilla_net_StealthSessionResolver_h

#include "nsIDNSService.h"
#include "nsStringFwd.h"

namespace mozilla {
class OriginAttributes;
}

namespace mozilla::net {

/**
 * A lookup is answered by DoH or by the host's resolver according to the user
 * context it belongs to, never according to the global mode.
 *
 * The global mode (network.trr.mode) stays 0, so a lookup that carries no mode
 * of its own is native. Every lookup enters the host resolver through
 * nsHostResolver::ResolveHost or DetachCallback, the two functions that build
 * the record key from the flags, and there its TRR mode bits are set:
 *  - the context of the lookup has a proxy: TRR_ONLY. The lookup is answered by
 *    DoH only, through the proxy of its context, and when DoH fails it fails
 *    (no fallback to the native resolver);
 *  - the context has none: "TRR disabled", the native resolver, as in retail.
 * Every other flag stays. A context without a proxy is therefore not coupled to
 * the proxied ones, which a global mode of 3 would do.
 *
 * Which contexts have a proxy is the transport pref
 * network.trr.stealth.contexts (StealthPrefList.yaml), written by Juggler: "*"
 * when the browser-wide proxy exists (every context without a proxy of its own
 * is proxied) and the decimal ids of the contexts that have their own. The
 * services that stay on as in retail and ask for "TRR disabled" (connectivity
 * checks, captive portal, OCSP) follow the context of their lookup this way
 * without being edited.
 *
 * Not touched:
 *  - the flag RESOLVE_DISABLE_TRR (a different bit): the main-thread
 *    synchronous resolve and the retries that skip a TRR-only attempt keep
 *    their meaning, and fail closed for a proxied lookup;
 *  - the names the TRR service excludes (network.trr.excluded-domains: the
 *    names of the proxies; and the built-in localhost and local): for them
 *    nsHostResolver::ComputeEffectiveTRRMode turns a TRR_ONLY request into a
 *    native one, as it does for any lookup;
 *  - the host name of the DoH server itself: the DoH request is a "TRR
 *    disabled" request on purpose (resolving the server through DoH would wait
 *    for itself), so that name keeps the flags it asked for.
 */

/**
 * True when the context aUserContextId has a proxy according to aList, the
 * value of network.trr.stealth.contexts: comma-separated tokens, spaces around
 * a token ignored. The token "*" matches every context, a token of decimal
 * digits matches the context with that id. Any other token (garbage, a sign, a
 * number with leading zeros such as "06", a number that does not fit 32 bits)
 * is ignored. The default context has the id 0. Pure.
 */
bool StealthContextHasProxy(uint32_t aUserContextId, const nsACString& aList);

/**
 * The flags a lookup enters the host resolver with, pure: its TRR mode bits
 * are TRR_ONLY when aContextHasProxy and "TRR disabled" otherwise, every other
 * flag stays. When aHostIsTRRServer says the name is the DoH server's own, the
 * flags are returned as they are.
 */
nsIDNSService::DNSFlags StealthResolverFlags(nsIDNSService::DNSFlags aFlags,
                                             bool aContextHasProxy,
                                             bool aHostIsTRRServer);

/**
 * The hook of nsHostResolver: reads the pref, asks the TRR service for its
 * server (when there is one) and returns the flags to resolve with. aAttrs are
 * the attributes of the lookup, which carry its user context. Any thread.
 */
nsIDNSService::DNSFlags StealthFlagsFollowSession(
    const nsACString& aHost, const OriginAttributes& aAttrs,
    nsIDNSService::DNSFlags aFlags);

}  // namespace mozilla::net

#endif  // mozilla_net_StealthSessionResolver_h
