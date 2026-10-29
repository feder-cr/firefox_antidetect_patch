/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthSocksUdp.h"

#include <inttypes.h>
#include <stdint.h>

#include "mozilla/Assertions.h"
#include "mozilla/Preferences.h"
#include "nsError.h"
#include "nsSOCKSUDPIOLayer.h"
#include "nsSocketTransportService2.h"  // UDPSOCKET_LOG, gUDPSocketLog
#include "nsString.h"

namespace mozilla::net {

// Stealthfox: if a SOCKS5 proxy is configured AND socks_remote_udp is
// enabled, push the SOCKS UDP layer onto the socket so that all
// sendto() / recvfrom() are tunneled via the proxy's UDP ASSOCIATE
// endpoint. Without this, QUIC / HTTP/3 traffic bypasses the proxy and
// leaks the host's real IP.
// Stealthfox: when SOCKS5 proxy is configured AND
// network.proxy.socks_remote_udp is true, push the SOCKS UDP layer
// onto the new fd so all sendto/recvfrom calls are tunneled via the
// proxy's UDP_ASSOCIATE relay. Without this, QUIC/HTTP-3 traffic
// would either bypass the proxy (leaking the host IP) or fail.
void StealthAttachSocksUdpLayer(PRFileDesc* aFd) {
  MOZ_ASSERT(aFd);

  if (Preferences::GetBool("network.proxy.socks_remote_udp", false)) {
    int32_t proxyType = Preferences::GetInt("network.proxy.type", 0);
    int32_t socksVer = Preferences::GetInt("network.proxy.socks_version", 5);
    if (proxyType == 1 && socksVer == 5) {
      nsAutoCString host;
      Preferences::GetCString("network.proxy.socks", host);
      int32_t port = Preferences::GetInt("network.proxy.socks_port", 0);
      if (!host.IsEmpty() && port > 0) {
        nsAutoCString user, pass;
        Preferences::GetCString("network.proxy.socks_username", user);
        Preferences::GetCString("network.proxy.socks_password", pass);
        nsresult sr = nsSOCKSUDPIOLayerAddToSocket(aFd, host.get(), port,
                                                   user.get(), pass.get());
        if (NS_FAILED(sr)) {
          UDPSOCKET_LOG(("SOCKS UDP layer push failed (%" PRIx32
                         "); falling back to direct UDP",
                         static_cast<uint32_t>(sr)));
        } else {
          UDPSOCKET_LOG(
              ("SOCKS UDP layer attached (proxy %s:%d)", host.get(), port));
        }
      }
    }
  }
}

}  // namespace mozilla::net
