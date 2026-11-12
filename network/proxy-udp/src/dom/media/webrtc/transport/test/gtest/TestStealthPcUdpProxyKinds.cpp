/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/proxy-udp)
//
// What the lookup of the proxy of a PeerConnection MEANS, as a function of its answer (PcUdpProxy::KindFor, which the callback of the
// lookup calls): the table of the cases, among them the one no lookup can be made to produce in a test, a lookup that FAILED. A failed
// lookup is a proxy that is not known and nothing goes out (Blocked), never a PeerConnection with no proxy (Direct): the old reading
// of a failed answer as 'no proxy' would send the UDP of a PeerConnection direct around a proxy it could not find out.

#include "gtest/gtest.h"

#include <string.h>

#include "StealthPcUdpProxy.h"
#include "nsCOMPtr.h"
#include "nsIProtocolProxyService.h"
#include "nsIProxyInfo.h"
#include "nsNetCID.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"

namespace mozilla {
namespace {

using net::PcUdpProxy;

// The proxy info Firefox itself makes for a proxy of aType. Only SOCKS and SOCKS4 take credentials: for the others the service refuses them.
nsCOMPtr<nsIProxyInfo> InfoOf(const char* aType, uint32_t aFlags) {
  const bool auth = !strcmp(aType, "socks") || !strcmp(aType, "socks4");
  nsCOMPtr<nsIProtocolProxyService> ps = do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
  nsCOMPtr<nsIProxyInfo> info;
  if (ps) {
    EXPECT_EQ(NS_OK, ps->NewProxyInfoWithAuth(nsDependentCString(aType), "192.0.2.7"_ns, 1080, auth ? "alice"_ns : ""_ns,
                                              auth ? "s3cret"_ns : ""_ns, ""_ns, ""_ns, aFlags, UINT32_MAX, nullptr,
                                              getter_AddRefs(info)));
  }
  return info;
}

}  // namespace

TEST(StealthPcUdpProxyKinds, ALookupThatAnswersNoProxyIsAPeerConnectionWithNone)
{
  EXPECT_EQ(PcUdpProxy::Kind::Direct, PcUdpProxy::KindFor(NS_OK, nullptr));
}

TEST(StealthPcUdpProxyKinds, ALookupThatFailedIsAProxyThatIsNotKnownSoNothingGoesOut)
{
  EXPECT_EQ(PcUdpProxy::Kind::Blocked, PcUdpProxy::KindFor(NS_ERROR_FAILURE, nullptr)) << "a failed lookup was read as 'no proxy'";
  EXPECT_EQ(PcUdpProxy::Kind::Blocked, PcUdpProxy::KindFor(NS_ERROR_ABORT, nullptr));
  // A failed lookup is Blocked even when it carries an answer: the answer of a failed lookup is not to be trusted.
  nsCOMPtr<nsIProxyInfo> coherent = InfoOf("socks", nsIProxyInfo::CARRIES_COHERENT_UDP);
  ASSERT_TRUE(coherent);
  PcUdpProxy::Socks5Info info;
  EXPECT_EQ(PcUdpProxy::Kind::Blocked, PcUdpProxy::KindFor(NS_ERROR_FAILURE, coherent, &info));
  EXPECT_TRUE(info.mHost.IsEmpty()) << "a failed lookup handed over the address of a relay";
}

TEST(StealthPcUdpProxyKinds, ASocks5ProxyMeasuredCoherentIsTheRoadOfTheUdpWithItsCredentials)
{
  nsCOMPtr<nsIProxyInfo> coherent = InfoOf("socks", nsIProxyInfo::CARRIES_COHERENT_UDP);
  ASSERT_TRUE(coherent);
  PcUdpProxy::Socks5Info info;
  EXPECT_EQ(PcUdpProxy::Kind::Socks5, PcUdpProxy::KindFor(NS_OK, coherent, &info));
  EXPECT_EQ("192.0.2.7"_ns, info.mHost);
  EXPECT_EQ(1080, info.mPort);
  EXPECT_EQ("alice"_ns, info.mUser);
  EXPECT_EQ("s3cret"_ns, info.mPass);
  // Without somewhere to put the info the kind is the same.
  EXPECT_EQ(PcUdpProxy::Kind::Socks5, PcUdpProxy::KindFor(NS_OK, coherent));
}

TEST(StealthPcUdpProxyKinds, EveryOtherProxyCarriesNoUdpSoItIsARouterThatDrops)
{
  // A null info would read as 'no proxy' (Direct): each is checked to exist before it is judged.
  nsCOMPtr<nsIProxyInfo> socksNotMeasured = InfoOf("socks", 0);
  nsCOMPtr<nsIProxyInfo> socks4 = InfoOf("socks4", nsIProxyInfo::CARRIES_COHERENT_UDP);
  nsCOMPtr<nsIProxyInfo> httpFlagged = InfoOf("http", nsIProxyInfo::CARRIES_COHERENT_UDP);
  nsCOMPtr<nsIProxyInfo> http = InfoOf("http", 0);
  ASSERT_TRUE(socksNotMeasured);
  ASSERT_TRUE(socks4);
  ASSERT_TRUE(httpFlagged);
  ASSERT_TRUE(http);
  EXPECT_EQ(PcUdpProxy::Kind::Blocked, PcUdpProxy::KindFor(NS_OK, socksNotMeasured)) << "SOCKS5 whose UDP was not measured coherent";
  EXPECT_EQ(PcUdpProxy::Kind::Blocked, PcUdpProxy::KindFor(NS_OK, socks4)) << "SOCKS4 carries no UDP, whatever the flag";
  EXPECT_EQ(PcUdpProxy::Kind::Blocked, PcUdpProxy::KindFor(NS_OK, httpFlagged)) << "an HTTP proxy carries no UDP, whatever the flag";
  PcUdpProxy::Socks5Info info;
  info.mHost.AssignLiteral("left over");
  EXPECT_EQ(PcUdpProxy::Kind::Blocked, PcUdpProxy::KindFor(NS_OK, http, &info));
  EXPECT_TRUE(info.mHost.IsEmpty()) << "the info of an earlier answer was left in the result";
}

}  // namespace mozilla
