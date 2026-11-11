/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/proxy-udp)
//
// The rule made where the UDP socket of a PeerConnection is born (NrSocket::create -> StealthUdpProxyAttachRefused), from the proxy the
// lookup answered (PcUdpProxy): no proxy config is a plain socket; no proxy is a pass-through; a SOCKS5 proxy the session measured
// coherent is the relay; every other case (a proxy that carries no UDP, a lookup that failed) is a router that drops the datagrams.
// Never a direct send around a proxy. Pure function of the proxy: no PeerConnection is needed.

#include "gtest/gtest.h"

#include "nr_stealth_proxy_udp.h"

#include "StealthPcUdpProxy.h"
#include "mozilla/RefPtr.h"
#include "nsSOCKSUDPIOLayer.h"
#include "nsString.h"
#include "prio.h"
#include "prnetdb.h"

namespace mozilla {
namespace {

// An address of this host that is not loopback (a datagram to the loopback goes direct in every state): where a receiver can stand.
PRNetAddr HostAddressOf(PRFileDesc* aReceiver) {
  PRNetAddr probe;
  PR_InitializeNetAddr(PR_IpAddrNull, 9, &probe);
  probe.inet.ip = PR_htonl(0xC0000201);  // 192.0.2.1: nothing is sent
  PRFileDesc* s = PR_NewUDPSocket();
  PRNetAddr mine;
  PR_InitializeNetAddr(PR_IpAddrAny, 0, &mine);
  if (PR_Connect(s, &probe, PR_INTERVAL_NO_WAIT) == PR_SUCCESS) {
    PR_GetSockName(s, &mine);
  }
  PR_Close(s);
  PRNetAddr bound;
  PR_GetSockName(aReceiver, &bound);
  mine.inet.port = bound.inet.port;
  return mine;
}

PRFileDesc* ReceiverOnAnyAddress() {
  PRFileDesc* receiver = PR_NewUDPSocket();
  PRNetAddr any;
  PR_InitializeNetAddr(PR_IpAddrAny, 0, &any);
  EXPECT_EQ(PR_SUCCESS, PR_Bind(receiver, &any));
  return receiver;
}

// Whether a datagram sent from a socket made under aProxy reaches a receiver at the host's own (non-loopback) address.
bool Arrives(const RefPtr<net::PcUdpProxy>& aProxy, bool* aRefused = nullptr) {
  PRFileDesc* receiver = ReceiverOnAnyAddress();
  PRNetAddr to = HostAddressOf(receiver);
  PRFileDesc* fd = PR_NewUDPSocket();
  const bool refused = StealthUdpProxyAttachRefused(fd, aProxy);
  if (aRefused) {
    *aRefused = refused;
  }
  bool arrived = false;
  if (!refused) {
    EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &to, PR_INTERVAL_NO_WAIT));
    char buf[16];
    PRNetAddr from;
    arrived = PR_RecvFrom(receiver, buf, sizeof(buf), 0, &from, PR_MillisecondsToInterval(500)) > 0;
  }
  PR_Close(fd);
  PR_Close(receiver);
  return arrived;
}

}  // namespace

TEST(StealthUdpProxyBirth, ASocketOfNoPeerConnectionIsLeftPlain)
{
  PRFileDesc* fd = PR_NewUDPSocket();
  RefPtr<net::PcUdpProxy> none;
  EXPECT_FALSE(StealthUdpProxyAttachRefused(fd, none));
  EXPECT_FALSE(net::nsSOCKSUDPIOLayerIsOn(fd)) << "a socket with no proxy config got the layer";
  PR_Close(fd);
}

TEST(StealthUdpProxyBirth, ThePeerConnectionWithNoProxyGoesOutAsOnAMachineWithNone)
{
  bool refused = true;
  EXPECT_TRUE(Arrives(net::PcUdpProxy::Known(net::PcUdpProxy::Kind::Direct), &refused));
  EXPECT_FALSE(refused);
}

TEST(StealthUdpProxyBirth, AProxyThatCarriesNoUdpMakesASocketWhoseDatagramsGoNowhere)
{
  bool refused = true;
  EXPECT_FALSE(Arrives(net::PcUdpProxy::Known(net::PcUdpProxy::Kind::Blocked), &refused))
      << "a datagram of a socket behind a proxy without UDP went out direct";
  EXPECT_FALSE(refused) << "the socket must exist (a router that drops), not be refused";
}

TEST(StealthUdpProxyBirth, ASocks5ProxyThatAnswersNothingIsARouterThatDrops)
{
  // A SOCKS5 proxy on a port nothing listens on: the association fails, and what is sent goes nowhere, never direct.
  net::PcUdpProxy::Socks5Info info;
  info.mHost.AssignLiteral("127.0.0.1");
  info.mPort = 1;
  bool refused = true;
  EXPECT_FALSE(Arrives(net::PcUdpProxy::Known(net::PcUdpProxy::Kind::Socks5, info), &refused))
      << "a datagram went out direct while its SOCKS5 association failed";
  EXPECT_FALSE(refused);
}

TEST(StealthUdpProxyBirth, TheLayerIsOnEverySocketOfAPeerConnectionWithAProxyAnswer)
{
  net::PcUdpProxy::Socks5Info info;
  info.mHost.AssignLiteral("127.0.0.1");
  info.mPort = 1;
  for (auto kind : {net::PcUdpProxy::Kind::Direct, net::PcUdpProxy::Kind::Blocked, net::PcUdpProxy::Kind::Socks5}) {
    PRFileDesc* fd = PR_NewUDPSocket();
    EXPECT_FALSE(StealthUdpProxyAttachRefused(fd, net::PcUdpProxy::Known(kind, info)));
    EXPECT_TRUE(net::nsSOCKSUDPIOLayerIsOn(fd)) << "kind " << static_cast<int>(kind);
    PR_Close(fd);
  }
}

}  // namespace mozilla
