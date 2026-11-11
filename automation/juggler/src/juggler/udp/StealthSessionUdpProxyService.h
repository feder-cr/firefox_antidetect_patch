/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef juggler_udp_StealthSessionUdpProxyService_h
#define juggler_udp_StealthSessionUdpProxyService_h

#include "nsIStealthSessionUdpProxy.h"

namespace mozilla {

// A thin door from the Juggler's JavaScript to
// net::StealthSessionUdpProxyPublish (netwerk/socket/nsSOCKSUDPIOLayer.cpp),
// where the decision, the storage and the IPC to the socket process are.
class StealthSessionUdpProxyService final : public nsIStealthSessionUdpProxy {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSISTEALTHSESSIONUDPPROXY

  StealthSessionUdpProxyService() = default;

 private:
  ~StealthSessionUdpProxyService() = default;
};

}  // namespace mozilla

#endif  // juggler_udp_StealthSessionUdpProxyService_h
