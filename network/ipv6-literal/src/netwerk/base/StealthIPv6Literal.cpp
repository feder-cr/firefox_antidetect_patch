/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthIPv6Literal.h"

#include "mozilla/StaticPrefs_network.h"
#include "prerr.h"

namespace mozilla::net {

PRErrorCode StealthIPv6ConnectError(const NetAddr& aAddr, bool aIPv6Disabled) {
  if (!aIPv6Disabled || aAddr.raw.family != AF_INET6) {
    return 0;
  }
  const IPv6Addr& ip = aAddr.inet6.ip;
  if (IPv6ADDR_IS_LOOPBACK(&ip)) {
    return 0;
  }
  // fe80::/10: an interface decides, the OS answers.
  if (ip.u8[0] == 0xfe && (ip.u8[1] & 0xc0) == 0x80) {
    return 0;
  }
  // An IPv4-mapped address is IPv4's, not ours: the SOCKS layer builds one for
  // its IPv4 proxy when the NSPR emulation layer is under the socket.
  if (IPv6ADDR_IS_V4MAPPED(&ip)) {
    return 0;
  }
  // The same on every route table: Windows refuses these at connect().
  if (IPv6ADDR_IS_UNSPECIFIED(&ip) || ip.u8[0] == 0xff) {
    return PR_ADDRESS_NOT_AVAILABLE_ERROR;
  }
  return PR_NETWORK_UNREACHABLE_ERROR;
}

PRStatus StealthIPv6Connect(PRFileDesc* aFd, const NetAddr& aAddr,
                            bool aAddrIsBehindProxy, const PRNetAddr* aPrAddr,
                            PRIntervalTime aTimeout) {
  PRErrorCode error = StealthIPv6ConnectError(
      aAddr, !aAddrIsBehindProxy && StaticPrefs::network_dns_disableIPv6());
  if (error) {
    PR_SetError(error, 0);
    return PR_FAILURE;
  }
  return PR_Connect(aFd, aPrAddr, aTimeout);
}

}  // namespace mozilla::net
