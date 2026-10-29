/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthH3Proxy_h
#define mozilla_net_StealthH3Proxy_h

class nsIProxyInfo;

namespace mozilla::net {

// Whether HTTP/3 may be used through the proxy a channel resolved to.
// Firefox allows it for no proxy, a direct proxy and an HTTP/3 proxy; we
// also allow a SOCKS proxy while network.proxy.socks_remote_udp is on.
// aProxyInfo may be null (no proxy). The pref is read on every call.
bool StealthH3ProxyAllowed(nsIProxyInfo* aProxyInfo);

}  // namespace mozilla::net

#endif  // mozilla_net_StealthH3Proxy_h
