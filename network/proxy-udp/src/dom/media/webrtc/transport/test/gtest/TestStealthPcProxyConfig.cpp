/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/proxy-udp)
//
// The config of a PeerConnection always exists. PeerConnectionImpl::GetProxyConfig returns nullptr for a window that was torn down (a
// removed iframe, a closed popup: no BrowserChild, and no ClientInfo so LoadInfo::Create refuses the document); with no config every
// socket of the PeerConnection is plain. StealthProxyConfigOf gives such a PeerConnection the config of the principal of its window:
// its user context (so the proxy of its context), no tab id, a load info with no node. With no principal at all the config forces the
// proxy (no UDP socket, TCP only through a proxy). A config upstream made is returned as it is, and upstream's opt-out stays.
// Red on the old code: the functions do not exist, and the call site used GetProxyConfig() alone.
//
// Not covered here, needs a window and a browser: the whole PeerConnection of a detached iframe, described as a test of the wrapper's
// fake web (a PeerConnection of a removed iframe and of a closed popup sends nothing but through the proxy of its context).

#include "gtest/gtest.h"

#include <memory>

#include "StealthIceResolverContext.h"
#include "StealthTestProxyKit.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/OriginAttributes.h"
#include "mozilla/Preferences.h"
#include "mozilla/net/WebrtcProxyConfig.h"
#include "nr_socket_proxy_config.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsIPrincipal.h"
#include "nsIURI.h"
#include "nsNetUtil.h"
#include "nsString.h"
#include "nsThreadUtils.h"

namespace mozilla {
namespace {

// A principal of a page of the user context aContext: what the window of a torn-down iframe still has.
nsCOMPtr<nsIPrincipal> PagePrincipalOfContext(uint32_t aContext) {
  nsCOMPtr<nsIURI> uri;
  EXPECT_EQ(NS_OK, NS_NewURI(getter_AddRefs(uri), "https://page.example.test/"_ns));
  OriginAttributes attrs;
  attrs.mUserContextId = aContext;
  RefPtr<BasePrincipal> principal = BasePrincipal::CreateContentPrincipal(uri.get(), attrs);
  return nsCOMPtr<nsIPrincipal>(principal.get());
}

const net::WebrtcProxyConfig& Of(const std::unique_ptr<NrSocketProxyConfig>& aConfig) { return aConfig->GetConfig(); }

// The lookups the configs start (they run in this process) answer "no proxy" for the length of the test and are drained before it ends.
class Scope {
 public:
  Scope() : mProxy(stealthtest::ProxyOfTheSession::None) {}
  ~Scope() { NS_ProcessPendingEvents(NS_GetCurrentThread()); }

 private:
  stealthtest::SessionProxyScope mProxy;
};

}  // namespace

TEST(StealthPcProxyConfig, AConfigUpstreamMadeIsReturnedAsItIs)
{
  Scope scope;
  std::unique_ptr<NrSocketProxyConfig> upstream =
      StealthProxyConfigOfPrincipal(nsContentUtils::GetSystemPrincipal(), 5, false);
  ASSERT_NE(nullptr, upstream.get());
  NrSocketProxyConfig* raw = upstream.get();
  std::unique_ptr<NrSocketProxyConfig> given = StealthProxyConfigOf(std::move(upstream), nullptr, false);
  EXPECT_EQ(raw, given.get()) << "a live window got another config than upstream's";
  EXPECT_EQ(5u, static_cast<uint64_t>(Of(given).tabId()));
}

TEST(StealthPcProxyConfig, AWindowWithoutAConfigGetsTheConfigOfItsPrincipalAndItsContext)
{
  Scope scope;
  std::unique_ptr<NrSocketProxyConfig> config = StealthProxyConfigOfPrincipal(PagePrincipalOfContext(6).get(), 0, false);
  ASSERT_NE(nullptr, config.get()) << "a torn-down window is left with no config: every socket of its PeerConnection is plain";
  EXPECT_EQ(6u, Of(config).loadInfoArgs().originAttributes().mUserContextId) << "the proxy of the context is not the page's";
  EXPECT_EQ(0u, static_cast<uint64_t>(Of(config).tabId()));
  EXPECT_FALSE(Of(config).forceProxy());
  EXPECT_FALSE(config->GetForceProxy());
}

TEST(StealthPcProxyConfig, TheTabIdIsKeptWhenThereIsOne)
{
  Scope scope;
  std::unique_ptr<NrSocketProxyConfig> config = StealthProxyConfigOfPrincipal(PagePrincipalOfContext(0).get(), 9, false);
  ASSERT_NE(nullptr, config.get());
  EXPECT_EQ(9u, static_cast<uint64_t>(Of(config).tabId()));
  EXPECT_EQ(0u, Of(config).loadInfoArgs().originAttributes().mUserContextId);
}

TEST(StealthPcProxyConfig, AForcedProxyStaysForced)
{
  Scope scope;
  std::unique_ptr<NrSocketProxyConfig> config = StealthProxyConfigOfPrincipal(PagePrincipalOfContext(6).get(), 0, true);
  ASSERT_NE(nullptr, config.get());
  EXPECT_TRUE(config->GetForceProxy());
}

TEST(StealthPcProxyConfig, NoPrincipalAtAllForcesTheProxy)
{
  Scope scope;
  std::unique_ptr<NrSocketProxyConfig> config = StealthProxyConfigOfPrincipal(nullptr, 0, false);
  ASSERT_NE(nullptr, config.get()) << "no principal left the PeerConnection with no config";
  EXPECT_TRUE(config->GetForceProxy()) << "no principal: UDP must not be made and TCP only through a proxy";
}

TEST(StealthPcProxyConfig, NoWindowAndNoUpstreamConfigIsAForcedOne)
{
  Scope scope;
  std::unique_ptr<NrSocketProxyConfig> config = StealthProxyConfigOf(nullptr, nullptr, false);
  ASSERT_NE(nullptr, config.get());
  EXPECT_TRUE(config->GetForceProxy());
}

TEST(StealthPcProxyConfig, UpstreamsOptOutStaysWhereItAppliesAndOnlyThere)
{
  Scope scope;
  const bool hadValue = Preferences::HasUserValue("media.peerconnection.disable_http_proxy");
  const bool old = Preferences::GetBool("media.peerconnection.disable_http_proxy", false);
  ASSERT_EQ(NS_OK, Preferences::SetBool("media.peerconnection.disable_http_proxy", true));
  EXPECT_EQ(nullptr, StealthProxyConfigOf(nullptr, nullptr, false).get()) << "the pref of upstream no longer disables the proxy config";
  EXPECT_NE(nullptr, StealthProxyConfigOf(nullptr, nullptr, true).get()) << "a forced proxy ignores the opt-out, as upstream";
  if (hadValue) {
    Preferences::SetBool("media.peerconnection.disable_http_proxy", old);
  } else {
    Preferences::ClearUser("media.peerconnection.disable_http_proxy");
  }
}

}  // namespace mozilla
