/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/proxy-udp)
//
// What a TCP socket of a PeerConnection does at the end of WebrtcTCPSocket::OnProxyAvailable, where upstream opens it plain (DIRECT):
// the same decision UDP takes in PcUdpProxy::KindFor. A lookup that failed (a PAC thread cancelled, a filter that could not answer) or
// that answered something the socket does not know is a proxy that is not known: the socket is refused, it never goes out around the
// proxy of its context. The one answer that opens it plain is "no proxy" (the lookup succeeded and gave no info).
// Red on the old code: the function does not exist, and OnProxyAvailable opened the socket plain for every one of these cases.

#include "gtest/gtest.h"

#include "StealthPcUdpProxy.h"
#include "nr_stealth_network.h"
#include "nsCOMPtr.h"
#include "nsIProtocolProxyService.h"
#include "nsIProxyInfo.h"
#include "nsNetCID.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "prnetdb.h"

namespace mozilla {
namespace {

// A proxy info Firefox itself makes: on the tail of OnProxyAvailable any non-null info is one whose type the socket did not recognise.
nsCOMPtr<nsIProxyInfo> SomeInfo() {
  nsCOMPtr<nsIProtocolProxyService> ps = do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
  nsCOMPtr<nsIProxyInfo> info;
  if (ps) {
    EXPECT_EQ(NS_OK, ps->NewProxyInfoWithAuth("http"_ns, "192.0.2.7"_ns, 3128, ""_ns, ""_ns, ""_ns, ""_ns, 0, UINT32_MAX, nullptr,
                                              getter_AddRefs(info)));
  }
  return info;
}

}  // namespace

TEST(StealthTcpProxyRoad, ALookupThatFailedNeverOpensThePlainSocket)
{
  EXPECT_TRUE(StealthTcpDirectRefused(true, true, nullptr)) << "a failed lookup (PAC cancelled, shutdown) went DIRECT";
}

TEST(StealthTcpProxyRoad, AFailedLookupIsRefusedWhateverInfoItCarries)
{
  nsCOMPtr<nsIProxyInfo> info = SomeInfo();
  ASSERT_TRUE(info);
  EXPECT_TRUE(StealthTcpDirectRefused(true, true, info));
}

TEST(StealthTcpProxyRoad, AnInfoOfATypeTheSocketDoesNotKnowIsRefused)
{
  nsCOMPtr<nsIProxyInfo> info = SomeInfo();
  ASSERT_TRUE(info);
  EXPECT_TRUE(StealthTcpDirectRefused(true, false, info)) << "an unknown proxy type went DIRECT";
}

TEST(StealthTcpProxyRoad, ALookupThatAnswersNoProxyOpensThePlainSocket)
{
  // The context has no proxy: plain is the right road, as for a retail Firefox with none.
  EXPECT_FALSE(StealthTcpDirectRefused(true, false, nullptr));
}

TEST(StealthTcpProxyRoad, ASocketWithNoProxyConfigIsLeftAsUpstreamHasIt)
{
  nsCOMPtr<nsIProxyInfo> info = SomeInfo();
  EXPECT_FALSE(StealthTcpDirectRefused(false, true, nullptr));
  EXPECT_FALSE(StealthTcpDirectRefused(false, false, nullptr));
  EXPECT_FALSE(StealthTcpDirectRefused(false, false, info));
}

// The same table as UDP's: a lookup the UDP socket treats as "not known" (Blocked) is one the TCP socket refuses, and the answer UDP
// treats as "no proxy" (Direct) is the one TCP opens plain on.
TEST(StealthTcpProxyRoad, TcpAndUdpDecideAlike)
{
  using net::PcUdpProxy;
  EXPECT_EQ(PcUdpProxy::Kind::Blocked == PcUdpProxy::KindFor(NS_ERROR_ABORT, nullptr),
            StealthTcpDirectRefused(true, true, nullptr));
  EXPECT_EQ(PcUdpProxy::Kind::Direct == PcUdpProxy::KindFor(NS_OK, nullptr),
            !StealthTcpDirectRefused(true, false, nullptr));
}

// The two things a plain socket is given before it is bound come from one call: with nothing to give a context to it still gives the
// bind address, and a declared address that does not parse is refused.
TEST(StealthTcpProxyRoad, TheSocketIsBornWithItsBindAddressAndNoOtherCondition)
{
  PRNetAddr a;
  ASSERT_TRUE(StealthTcpSocketBorn(nullptr, nullptr, "192.168.31.7", 51234, &a));
  EXPECT_EQ(a.raw.family, PR_AF_INET);
  EXPECT_EQ(a.inet.port, 0);
  EXPECT_FALSE(StealthTcpSocketBorn(nullptr, nullptr, "not an address", 51234, &a));
  EXPECT_FALSE(StealthTcpSocketBorn(nullptr, nullptr, nullptr, 51234, &a));
}

}  // namespace mozilla
