/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/proxy-udp)
//
// What the gtests of the sockets of a PeerConnection need from the proxy side, in a translation unit of its own: the config of a
// PeerConnection (NrSocketProxyConfig) is made from an IPDL struct whose headers do not go with the headers of nICEr in one file.
// This header names no type of either.

#ifndef StealthTestProxyKit_h__
#define StealthTestProxyKit_h__

#include <memory>

namespace mozilla {

class NrSocketProxyConfig;

namespace stealthtest {

enum class ProxyOfTheSession {
  // No proxy filter and network.proxy.type 0: the lookup of the PeerConnection answers "no proxy".
  None,
  // A filter that answers an HTTP proxy: a proxy that carries no UDP, so the lookup answers "a router that drops it".
  HttpWithoutUdp,
};

// The proxy of the session for the length of the scope (a filter registered, the prefs it needs set); everything is put back
// when the scope ends. Main thread.
class SessionProxyScope {
 public:
  explicit SessionProxyScope(ProxyOfTheSession aProxy);
  ~SessionProxyScope();
  SessionProxyScope(const SessionProxyScope&) = delete;
  SessionProxyScope& operator=(const SessionProxyScope&) = delete;
  bool Ok() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> mImpl;
};

// The config of a PeerConnection of the system principal, made as the PeerConnection makes it, after the lookup of its proxy has
// answered (the event loop is spun until it has). Null if it could not be made. Main thread.
std::shared_ptr<NrSocketProxyConfig> MakeConfigAfterTheLookup();

}  // namespace stealthtest
}  // namespace mozilla

#endif  // StealthTestProxyKit_h__
