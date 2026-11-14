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

#include <memory>
#include <stdint.h>

#include "mozilla/OriginAttributes.h"

class nsIPrincipal;
class nsPIDOMWindowInner;

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

// STEALTH-HOOK(network/proxy-udp): the config of a PeerConnection always exists.
//
// PeerConnectionImpl::GetProxyConfig returns nullptr when the window has no BrowserChild, no document, or when the load info
// cannot be made, and then SetProxyConfig is never called: every socket of the PeerConnection is plain (UDP direct, TCP direct)
// and the names of its ICE servers are looked up in the default context. A page reaches that with a PeerConnection built from
// the window of a removed iframe or of a closed popup (the constructor only asks the window to be the current one of a
// discarded browsing context, nsPIDOMWindowInner::IsCurrentInnerWindow; the docshell is gone, so there is no BrowserChild, and
// the document has lost its ClientInfo, so LoadInfo::Create refuses it).

/**
 * The config of a PeerConnection made from the principal of its window alone: its origin attributes (the user context, so the
 * proxy of the context and the context of the DoH lookups are the ones of the page), the tab id given (0: none; it only feeds
 * the 407 prompt of an HTTP proxy, which then fails closed), and a load info that has no node (so it does not need the
 * ClientInfo a torn-down document has lost). No principal at all: a NullPrincipal and forceProxy, so no UDP socket is made and a
 * TCP socket is made only through a proxy. Null only if the load info cannot be serialised.
 */
std::unique_ptr<NrSocketProxyConfig> StealthProxyConfigOfPrincipal(
    nsIPrincipal* aPrincipal, uint64_t aTabId, bool aForceProxy);

/**
 * The config the PeerConnection gives its ICE context: aUpstream (what GetProxyConfig made) when there is one, else the
 * config of the principal of aWindow (live windows are never here: their config is made by upstream, byte for byte as
 * before). The one exception is upstream's own opt-out, the pref media.peerconnection.disable_http_proxy without
 * aForceProxy: null, no proxy config at all, as upstream (a page cannot set a pref).
 */
std::unique_ptr<NrSocketProxyConfig> StealthProxyConfigOf(
    std::unique_ptr<NrSocketProxyConfig> aUpstream, nsPIDOMWindowInner* aWindow,
    bool aForceProxy);

}  // namespace mozilla

#endif  // mozilla_StealthIceResolverContext_h
