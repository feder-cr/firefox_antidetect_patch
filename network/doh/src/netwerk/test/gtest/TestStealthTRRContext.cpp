/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Behind a proxy the DNS-over-HTTPS request leaves by the proxy of the browser
// context whose lookup caused it: the context (userContextId) of the lookup
// travels from the TRR request to the proxy lookup, and the proxy filter of the
// session sees it on the stand-in channel.

#include "TRRLoadInfo.h"
#include "gtest/gtest.h"
#include "mozilla/Maybe.h"
#include "mozilla/OriginAttributes.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "mozilla/net/ProxyConfigLookup.h"
#include "mozilla/net/StealthTRRContext.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
#include "nsContentUtils.h"
#include "nsIChannel.h"
#include "nsIContentPolicy.h"
#include "nsILoadInfo.h"
#include "nsIProtocolProxyFilter.h"
#include "nsIProtocolProxyService.h"
#include "nsIParentalControlsService.h"
#include "nsIProxyInfo.h"
#include "nsNetUtil.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"

namespace mozilla::net {

namespace {

// The proxy filter of the session in one line: it records the context of the
// channel it is asked about and answers with the default proxy info.
class RecordingFilter final : public nsIProtocolProxyChannelFilter {
 public:
  NS_DECL_ISUPPORTS
  NS_IMETHOD ApplyFilter(nsIChannel* aChannel, nsIProxyInfo* aProxyInfo,
                         nsIProxyProtocolFilterResult* aCallback) override {
    nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
    mUserContextId = loadInfo->GetOriginAttributes().mUserContextId;
    mAsked = true;
    return aCallback->OnProxyFilterResult(aProxyInfo);
  }
  bool mAsked = false;
  uint32_t mUserContextId = 99999;

 private:
  ~RecordingFilter() = default;
};
NS_IMPL_ISUPPORTS(RecordingFilter, nsIProtocolProxyChannelFilter)

Maybe<OriginAttributes> ContextOf(uint32_t aUserContextId) {
  OriginAttributes attrs;
  attrs.mUserContextId = aUserContextId;
  return Some(attrs);
}

}  // namespace

// The default context is answered by the browser's proxy, as upstream: nothing
// is carried. Every other attribute of the suffix is left behind.
TEST(TestStealthTRRContext, SuffixOfTheDefaultContextCarriesNothing)
{
  EXPECT_TRUE(StealthTRRContextOfSuffix(""_ns).isNothing());
  EXPECT_TRUE(StealthTRRContextOfSuffix("^privateBrowsingId=1"_ns).isNothing());
  EXPECT_TRUE(StealthTRRContextOfSuffix(
                  "^partitionKey=%28https%2Cexample.com%29"_ns)
                  .isNothing());
  EXPECT_TRUE(StealthTRRContextOfSuffix("^userContextId=0"_ns).isNothing());
}

TEST(TestStealthTRRContext, SuffixOfAContextCarriesTheUserContextIdAlone)
{
  Maybe<OriginAttributes> context = StealthTRRContextOfSuffix(
      "^userContextId=7&partitionKey=%28https%2Cexample.com%29"_ns);
  ASSERT_TRUE(context.isSome());
  EXPECT_EQ(context->mUserContextId, 7u);
  EXPECT_TRUE(context->mPartitionKey.IsEmpty());
}

// The load info of a DoH channel off the main thread is a TRRLoadInfo; it used
// to refuse the attributes (NS_ERROR_NOT_IMPLEMENTED), so the context could
// not be put on the channel.
TEST(TestStealthTRRContext, ATRRLoadInfoTakesTheContext)
{
  RefPtr<TRRLoadInfo> loadInfo =
      new TRRLoadInfo(nullptr, nsIContentPolicy::TYPE_OTHER);
  EXPECT_TRUE(StealthTRRContextOfLoadInfo(loadInfo).isNothing());
  ASSERT_EQ(StealthApplyTRRContext(loadInfo, ContextOf(7)), NS_OK);
  Maybe<OriginAttributes> context = StealthTRRContextOfLoadInfo(loadInfo);
  ASSERT_TRUE(context.isSome());
  EXPECT_EQ(context->mUserContextId, 7u);
}

// A redirect of the DoH request is made on a clone of the load info: it stays
// in the same context, so it leaves by the same proxy.
TEST(TestStealthTRRContext, TheCloneOfATRRLoadInfoKeepsTheContext)
{
  RefPtr<TRRLoadInfo> loadInfo =
      new TRRLoadInfo(nullptr, nsIContentPolicy::TYPE_OTHER);
  ASSERT_EQ(StealthApplyTRRContext(loadInfo, ContextOf(7)), NS_OK);
  nsCOMPtr<nsILoadInfo> clone = loadInfo->Clone();
  Maybe<OriginAttributes> context = StealthTRRContextOfLoadInfo(clone);
  ASSERT_TRUE(context.isSome());
  EXPECT_EQ(context->mUserContextId, 7u);
}

TEST(TestStealthTRRContext, NothingLeavesTheLoadInfoAsItIs)
{
  RefPtr<TRRLoadInfo> loadInfo =
      new TRRLoadInfo(nullptr, nsIContentPolicy::TYPE_OTHER);
  ASSERT_EQ(StealthApplyTRRContext(loadInfo, ContextOf(7)), NS_OK);
  ASSERT_EQ(StealthApplyTRRContext(loadInfo, Nothing()), NS_OK);
  EXPECT_TRUE(StealthTRRContextOfLoadInfo(loadInfo).isSome());
}

TEST(TestStealthTRRContext, ARealChannelTakesTheContext)
{
  nsCOMPtr<nsIURI> uri;
  ASSERT_EQ(NS_NewURI(getter_AddRefs(uri), "https://example.com/dns-query"_ns),
            NS_OK);
  nsCOMPtr<nsIChannel> channel;
  ASSERT_EQ(
      NS_NewChannel(getter_AddRefs(channel), uri,
                    nsContentUtils::GetSystemPrincipal(),
                    nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
                    nsIContentPolicy::TYPE_OTHER),
      NS_OK);
  ASSERT_EQ(StealthApplyTRRContextToChannel(channel, ContextOf(7)), NS_OK);
  nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();
  EXPECT_EQ(loadInfo->GetOriginAttributes().mUserContextId, 7u);
}

// A DoH request whose proxy could not be learned fails; it never goes DIRECT.
TEST(TestStealthTRRContext, AFailedProxyLookupEndsTheRequest)
{
  EXPECT_EQ(StealthTRRProxyFailure(NS_ERROR_FAILURE),
            NS_ERROR_PROXY_CONNECTION_REFUSED);
  EXPECT_EQ(StealthTRRProxyFailure(NS_ERROR_NOT_AVAILABLE),
            NS_ERROR_PROXY_CONNECTION_REFUSED);
  EXPECT_EQ(StealthTRRProxyFailure(NS_ERROR_NET_TIMEOUT),
            NS_ERROR_PROXY_CONNECTION_REFUSED);
  EXPECT_EQ(StealthTRRProxyFailure(NS_ERROR_UNEXPECTED),
            NS_ERROR_PROXY_CONNECTION_REFUSED);
}

// A cancellation is not a proxy failure: it keeps its own code.
TEST(TestStealthTRRContext, ACancellationKeepsItsCode)
{
  EXPECT_EQ(StealthTRRProxyFailure(NS_ERROR_ABORT), NS_ERROR_ABORT);
  EXPECT_EQ(StealthTRRProxyFailure(NS_BINDING_ABORTED), NS_BINDING_ABORTED);
}

// The nsHttpChannel of a DoH request (main thread of the parent, no socket
// process) fails closed like TRRServiceChannel: a failed lookup of a TRR service
// channel ends it; a success, a cancellation and any other channel do not.
TEST(TestStealthTRRContext, ATRRServiceChannelWhoseProxyLookupFailedFailsClosed)
{
  EXPECT_TRUE(StealthFailClosedOnProxyFailure(NS_ERROR_FAILURE, true, false));
  EXPECT_TRUE(
      StealthFailClosedOnProxyFailure(NS_ERROR_NOT_AVAILABLE, true, false));
}

TEST(TestStealthTRRContext, OtherChannelsAndOtherOutcomesKeepUpstreamBehaviour)
{
  // An ordinary channel fails over as upstream does.
  EXPECT_FALSE(StealthFailClosedOnProxyFailure(NS_ERROR_FAILURE, false, false));
  // A lookup that succeeded.
  EXPECT_FALSE(StealthFailClosedOnProxyFailure(NS_OK, true, false));
  // A cancelled channel goes on as upstream (BeginConnect returns its status).
  EXPECT_FALSE(StealthFailClosedOnProxyFailure(NS_ERROR_FAILURE, true, true));
}

// The proxy lookup of a DoH request asks the proxy filters with a stand-in
// channel. With the context of the lookup it is that context's channel; without
// one it is the default context's, as upstream.
TEST(TestStealthTRRContext, TheFilterSeesTheContextOfTheLookup)
{
  nsCOMPtr<nsIProtocolProxyService> pps =
      do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
  ASSERT_TRUE(pps);
  RefPtr<RecordingFilter> filter = new RecordingFilter();
  ASSERT_EQ(pps->RegisterChannelFilter(filter, 0), NS_OK);

  nsCOMPtr<nsIURI> uri;
  ASSERT_EQ(NS_NewURI(getter_AddRefs(uri),
                      "https://mozilla.cloudflare-dns.com/dns-query"_ns),
            NS_OK);

  auto Lookup = [&](const Maybe<OriginAttributes>& aContext) {
    bool done = false;
    filter->mAsked = false;
    EXPECT_EQ(ProxyConfigLookup::Create(
                  [&done](nsIProxyInfo*, nsresult) { done = true; }, uri, 0,
                  /* aIsTRRServiceChannel */ true, nullptr, Nothing(),
                  aContext),
              NS_OK);
    EXPECT_TRUE(SpinEventLoopUntil("TestStealthTRRContext"_ns,
                                   [&] { return done; }));
    EXPECT_TRUE(filter->mAsked);
  };

  Lookup(ContextOf(7));
  EXPECT_EQ(filter->mUserContextId, 7u);
  Lookup(ContextOf(12));
  EXPECT_EQ(filter->mUserContextId, 12u);
  Lookup(Nothing());
  EXPECT_EQ(filter->mUserContextId, 0u);

  EXPECT_EQ(pps->UnregisterChannelFilter(filter), NS_OK);
}

// Parental controls are declared off by the service itself, so every reader
// (nsHttpHandler, downloads, the DoH heuristics) sees the same value and the OS
// is never asked. The Linux implementation is a stub that already says "off",
// so this passes on the old code there; it is a guard for the contract.
TEST(TestStealthTRRContext, ParentalControlsServiceReportsEverythingOff)
{
  nsCOMPtr<nsIParentalControlsService> pcs =
      do_CreateInstance("@mozilla.org/parental-controls-service;1");
  if (!pcs) {
    GTEST_SKIP() << "no parental controls service in this build";
  }
  bool enabled = true;
  EXPECT_EQ(pcs->GetParentalControlsEnabled(&enabled), NS_OK);
  EXPECT_FALSE(enabled);
  // An implementation without the state answers an error and leaves false.
  bool blocked = false;
  if (NS_SUCCEEDED(pcs->GetBlockFileDownloadsEnabled(&blocked))) {
    EXPECT_FALSE(blocked);
  }
  bool logging = false;
  if (NS_SUCCEEDED(pcs->GetLoggingEnabled(&logging))) {
    EXPECT_FALSE(logging);
  }
}

}  // namespace mozilla::net
