/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthH3Proxy.h"

#include "mozilla/Preferences.h"
#include "nsIProxyInfo.h"
#include "nsProxyInfo.h"

namespace mozilla::net {

bool StealthH3ProxyAllowed(nsIProxyInfo* aProxyInfo) {
  // Stealthfox: also allow SOCKS proxy when our SOCKS5-UDP-ASSOCIATE
  // layer is enabled (network.proxy.socks_remote_udp). Otherwise Firefox
  // unconditionally disables h3 whenever any non-direct proxy is set.
  bool socksUdpEnabled =
      Preferences::GetBool("network.proxy.socks_remote_udp", false);
  if (!aProxyInfo) {
    return true;
  }
  nsProxyInfo* proxyInfo = static_cast<nsProxyInfo*>(aProxyInfo);
  return proxyInfo->IsDirect() || proxyInfo->IsHttp3Proxy() ||
         (socksUdpEnabled && proxyInfo->IsSOCKS());
}

}  // namespace mozilla::net
