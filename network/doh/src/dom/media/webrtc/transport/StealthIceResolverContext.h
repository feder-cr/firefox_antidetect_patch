/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/doh)
//
// The name of an ICE server (stun:..., turn:...) is looked up in the browser context of
// the page that made the PeerConnection. NrIceResolver gives the DNS service the
// attributes it was given, so the DNS-over-HTTPS request of the lookup leaves by the
// proxy of that context (the proxy filter answers per userContextId of the request)
// and not in the default one.

#ifndef mozilla_StealthIceResolverContext_h
#define mozilla_StealthIceResolverContext_h

#include "mozilla/OriginAttributes.h"

namespace mozilla {

class NrSocketProxyConfig;

/**
 * The attributes a lookup of an ICE server name carries, from the attributes of the
 * PeerConnection: the user context alone. The context is what the proxy of the lookup
 * depends on; the rest (first party, private browsing, partition) would change the
 * identity of the cached record, which upstream's default attributes do not. The
 * default context stays the default attributes.
 */
OriginAttributes StealthIceResolverAttributes(
    const OriginAttributes& aPeerConnection);

/**
 * The same, from the proxy config the PeerConnection gives its ICE context: the
 * attributes of the load info it carries (they are sent to the process that gathers
 * before the gather starts).
 */
OriginAttributes StealthIceResolverAttributes(
    const NrSocketProxyConfig& aConfig);

}  // namespace mozilla

#endif  // mozilla_StealthIceResolverContext_h
