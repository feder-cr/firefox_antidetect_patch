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
 *    address is IPv4's and is left alone too.
 * Both codes become NS_ERROR_CONNECTION_REFUSED in ErrorAccordingToNSPR
 * (NS_ERROR_PROXY_CONNECTION_REFUSED when the socket goes to a proxy).
 */
PRErrorCode StealthIPv6ConnectError(const NetAddr& aAddr, bool aIPv6Disabled);

/**
 * PR_Connect(aFd, aPrAddr, aTimeout) unless the declaration refuses aAddr
 * (the same address as aPrAddr), in which case nothing is sent: PR_FAILURE and
 * the error of StealthIPv6ConnectError, as a failed connect() leaves it.
 * aAddrIsBehindProxy: aAddr is a destination a SOCKS proxy reaches, not an
 * address this host connects to; it is never refused.
 */
PRStatus StealthIPv6Connect(PRFileDesc* aFd, const NetAddr& aAddr,
                            bool aAddrIsBehindProxy, const PRNetAddr* aPrAddr,
                            PRIntervalTime aTimeout);

}  // namespace mozilla::net

#endif  // mozilla_net_StealthIPv6Literal_h
