/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthIPv6Literal_h
#define mozilla_net_StealthIPv6Literal_h

#include "mozilla/net/DNS.h"
#include "prerror.h"
#include "prio.h"

namespace mozilla::net {

/**
 * With IPv6 declared off (the disableIPv6 DNS pref, derived from the declared
 * network), a hostname never resolves to an IPv6 address, but a literal does:
 * nsHostResolver::FromIPLiteral gives http://[2001:db8::1]/ straight to the
 * socket, and the host's own IPv6 route decides. The declared network says
 * there is none, so the connection fails the way connect() fails on a Windows
 * machine that has the IPv6 stack and no IPv6 route.
 *
 * StealthIPv6ConnectError is that answer for one destination, as an NSPR
 * error code (0 = connect as usual). It is pure: aIPv6Disabled is the
 * declaration.
 *  - A global, unique-local, site-local or other routable address: no route,
 *    WSAENETUNREACH, PR_NETWORK_UNREACHABLE_ERROR.
 *  - The unspecified address and a multicast address: WSAEADDRNOTAVAIL,
 *    PR_ADDRESS_NOT_AVAILABLE_ERROR, whatever the routes are.
 *  - ::1 connects. A link-local address (fe80::/10) is left to the OS: it
 *    needs an interface, and what comes back is the OS's own. An IPv4-mapped
 *    address is not a route question: StealthConnectError judges it.
 * Both codes become NS_ERROR_CONNECTION_REFUSED in ErrorAccordingToNSPR
 * (NS_ERROR_PROXY_CONNECTION_REFUSED when the socket goes to a proxy).
 */
PRErrorCode StealthIPv6ConnectError(const NetAddr& aAddr, bool aIPv6Disabled);

/**
 * What the address in front of a connect is.
 *  - kDirect: an address this host connects to, chosen by a page or by DNS.
 *  - kBehindSocksProxy: the destination of a connection a SOCKS proxy makes;
 *    the proxy reaches it, not this host, so it is never refused.
 *  - kSocksProxyHop: the SOCKS layer's own connection to its proxy. The
 *    address may be an IPv4-mapped one that the layer built itself (the NSPR
 *    IPv6-to-IPv4 emulation layer needs it), which is not a page's literal.
 */
enum class StealthConnectTarget {
  kDirect,
  kBehindSocksProxy,
  kSocksProxyHop,
};

/**
 * StealthConnectError is the answer for one connect, as an NSPR error code
 * (0 = connect as usual). It is pure: aIPv6Disabled is the declaration.
 *  - kBehindSocksProxy: 0.
 *  - A direct connection to an IPv4-mapped literal (::ffff:0:0/96, not
 *    ::ffff:0:a.b.c.d and not ::a.b.c.d): WSAEADDRNOTAVAIL,
 *    PR_ADDRESS_NOT_AVAILABLE_ERROR, on every OS and whatever the declaration
 *    says. A Firefox-style socket has IPV6_V6ONLY on, and a Windows connect()
 *    to a mapped address fails; a dual-stack Linux socket would connect over
 *    IPv4. The SOCKS hop is exempt (see kSocksProxyHop).
 *  - Otherwise StealthIPv6ConnectError.
 */
PRErrorCode StealthConnectError(const NetAddr& aAddr,
                                StealthConnectTarget aTarget,
                                bool aIPv6Disabled);

/**
 * PR_Connect(aFd, aPrAddr, aTimeout) unless StealthConnectError refuses aAddr
 * (the same address as aPrAddr), in which case nothing is sent: PR_FAILURE and
 * the error, as a failed connect() leaves it.
 */
PRStatus StealthConnect(PRFileDesc* aFd, const NetAddr& aAddr,
                        StealthConnectTarget aTarget, const PRNetAddr* aPrAddr,
                        PRIntervalTime aTimeout);

/**
 * Parent process, once, at the top of nsIOService::Init and before any lookup:
 * IPv6 is one fact of the browser, the declared network's (the environment
 * variable STEALTHFOX_WEBRTC_NETWORK, see nr_stealth_network.h). The DNS IPv6
 * switch takes the value that fact gives, as a default value of its pref.
 * Other processes get the pref from the parent. Nothing in a build without
 * WebRTC.
 */
void StealthApplyDeclaredNetwork();

}  // namespace mozilla::net

#endif  // mozilla_net_StealthIPv6Literal_h
