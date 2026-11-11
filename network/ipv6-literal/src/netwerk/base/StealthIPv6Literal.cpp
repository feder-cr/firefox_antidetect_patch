/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthIPv6Literal.h"

#include "mozilla/StaticPrefs_network.h"
#include "nsXULAppAPI.h"
#include "prerr.h"
#ifdef MOZ_WEBRTC
#  include "transport/nr_stealth_network.h"
#endif

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
  // An IPv4-mapped address is no route question; StealthConnectError has it.
  if (IPv6ADDR_IS_V4MAPPED(&ip)) {
    return 0;
  }
  // The same on every route table: Windows refuses these at connect().
  if (IPv6ADDR_IS_UNSPECIFIED(&ip) || ip.u8[0] == 0xff) {
    return PR_ADDRESS_NOT_AVAILABLE_ERROR;
  }
  return PR_NETWORK_UNREACHABLE_ERROR;
}

PRErrorCode StealthConnectError(const NetAddr& aAddr,
                                StealthConnectTarget aTarget,
                                bool aIPv6Disabled) {
  if (aTarget == StealthConnectTarget::kBehindSocksProxy) {
    return 0;
  }
  if (aTarget == StealthConnectTarget::kDirect &&
      aAddr.raw.family == AF_INET6) {
    const IPv6Addr& ip = aAddr.inet6.ip;
    // Windows: WSAEADDRNOTAVAIL on a V6ONLY socket, declared or not.
    if (IPv6ADDR_IS_V4MAPPED(&ip)) {
      return PR_ADDRESS_NOT_AVAILABLE_ERROR;
    }
  }
  return StealthIPv6ConnectError(aAddr, aIPv6Disabled);
}

PRStatus StealthConnect(PRFileDesc* aFd, const NetAddr& aAddr,
                        StealthConnectTarget aTarget, const PRNetAddr* aPrAddr,
                        PRIntervalTime aTimeout) {
  PRErrorCode error = StealthConnectError(
      aAddr, aTarget, StaticPrefs::network_dns_disableIPv6());
  if (error) {
    PR_SetError(error, 0);
    return PR_FAILURE;
  }
  return PR_Connect(aFd, aPrAddr, aTimeout);
}

void StealthApplyDeclaredNetwork() {
#ifdef MOZ_WEBRTC
  if (XRE_IsParentProcess()) {
    StealthApplyDeclaredIPFamilyFromEnvironment();
  }
#endif
}

}  // namespace mozilla::net
