/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthDNSDeclaration_h
#define mozilla_net_StealthDNSDeclaration_h

namespace mozilla::net {

/**
 * True when the launcher declares that every connection leaves through a
 * proxy that resolves names itself, so the browser must not ask the local
 * resolver for any name that is not an IP literal.
 *
 * The check in DNSServiceBase::DNSForbiddenByActiveProxy can only see a proxy
 * that lives in the network.proxy.* prefs, and one of our two supported shapes
 * does not. For an HTTP/HTTPS endpoint the routing is done per channel by the
 * automation layer - it is the only side that can answer the proxy's 407 - so
 * network.proxy.type stays PROXYCONFIG_SYSTEM and the upstream guard never
 * arms. Channels are fine either way (the channel filter marks them
 * nsIProxyInfo::TRANSPARENT_PROXY_RESOLVES_HOST, see
 * nsProtocolProxyService.cpp), but some callers are NOT channels and call
 * AsyncResolveNative directly: NetworkConnectivityService, the DNS prefetch
 * and HTTPS-record paths, and the ICE resolver (nriceresolver.cpp). The ICE
 * resolver takes its hostname from the PAGE, via RTCPeerConnection's
 * iceServers, so a site can have the browser look up a name it chose and
 * watch which resolver asks - the user's own, while the HTTP goes out of the
 * proxy.
 *
 * So the fact is declared by the side that knows it, with the pref
 * zoom.stealth.dns.no_local_resolution. It is false by default: a build
 * launched without our launcher behaves exactly as upstream. This approaches
 * the TODO in DNSForbiddenByActiveProxy (Bug 1890542) from the caller's side
 * rather than from nsIProxyInfo.
 *
 * Callable on any thread: the pref is a RelaxedAtomicBool.
 */
bool StealthDNSProxyDeclared();

}  // namespace mozilla::net

#endif  // mozilla_net_StealthDNSDeclaration_h
