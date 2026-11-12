/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/proxy-udp)
//
// See StealthTestProxyKit.h. The filter is the one of netwerk/test/gtest/TestStealthSocksCredentials.cpp (a proxy filter that
// answers the proxy the session was made with), the config is made as PeerConnectionImpl makes it.

#include "StealthTestProxyKit.h"

#include "StealthPcUdpProxy.h"
#include "mozilla/Preferences.h"
#include "mozilla/RefPtr.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "mozilla/TimeStamp.h"
#include "mozilla/dom/ipc/IdType.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/net/NeckoChannelParams.h"
#include "mozilla/net/WebrtcProxyConfig.h"
#include "nr_socket_proxy_config.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsIChannel.h"
#include "nsIContentPolicy.h"
#include "nsILoadInfo.h"
#include "nsIProtocolProxyFilter.h"
#include "nsIProtocolProxyService.h"
#include "nsIProxyInfo.h"
#include "nsNetUtil.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"

namespace mozilla::stealthtest {

namespace {

// Every channel gets an HTTP proxy: a proxy that carries no UDP.
class HttpProxyFilter final : public nsIProtocolProxyChannelFilter {
 public:
  NS_DECL_ISUPPORTS
  HttpProxyFilter() = default;
  NS_IMETHOD ApplyFilter(nsIChannel* aChannel, nsIProxyInfo* aProxyInfo,
                         nsIProxyProtocolFilterResult* aCallback) override {
    nsCOMPtr<nsIProtocolProxyService> ps = do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
    NS_ENSURE_TRUE(ps, NS_ERROR_FAILURE);
    nsCOMPtr<nsIProxyInfo> info;
    nsresult rv = ps->NewProxyInfoWithAuth("http"_ns, "192.0.2.7"_ns, 3128, ""_ns, ""_ns, ""_ns, ""_ns, 0, UINT32_MAX,
                                           nullptr, getter_AddRefs(info));
    NS_ENSURE_SUCCESS(rv, rv);
    return aCallback->OnProxyFilterResult(info);
  }

 private:
  ~HttpProxyFilter() = default;
};
NS_IMPL_ISUPPORTS(HttpProxyFilter, nsIProtocolProxyChannelFilter)

}  // namespace

struct SessionProxyScope::Impl {
  RefPtr<HttpProxyFilter> mFilter;
  bool mOk = false;
  int32_t mOldProxyType = 0;
  bool mHadProxyType = false;
};

SessionProxyScope::SessionProxyScope(ProxyOfTheSession aProxy) : mImpl(new Impl()) {
  // The system proxy settings of the machine running the test must not decide the answer.
  mImpl->mHadProxyType = Preferences::HasUserValue("network.proxy.type");
  mImpl->mOldProxyType = Preferences::GetInt("network.proxy.type", 0);
  mImpl->mOk = NS_SUCCEEDED(Preferences::SetInt("network.proxy.type", 0));
  if (aProxy == ProxyOfTheSession::HttpWithoutUdp) {
    nsCOMPtr<nsIProtocolProxyService> ps = do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
    mImpl->mFilter = new HttpProxyFilter();
    mImpl->mOk = mImpl->mOk && ps && NS_SUCCEEDED(ps->RegisterChannelFilter(mImpl->mFilter, 0));
    if (!mImpl->mOk) {
      mImpl->mFilter = nullptr;
    }
  }
}

SessionProxyScope::~SessionProxyScope() {
  if (mImpl->mFilter) {
    nsCOMPtr<nsIProtocolProxyService> ps = do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
    if (ps) {
      (void)ps->UnregisterChannelFilter(mImpl->mFilter);
    }
  }
  if (mImpl->mHadProxyType) {
    (void)Preferences::SetInt("network.proxy.type", mImpl->mOldProxyType);
  } else {
    (void)Preferences::ClearUser("network.proxy.type");
  }
}

bool SessionProxyScope::Ok() const { return mImpl->mOk; }

std::shared_ptr<NrSocketProxyConfig> MakeConfigAfterTheLookup() {
  nsCOMPtr<nsIURI> uri;
  if (NS_FAILED(NS_NewURI(getter_AddRefs(uri), "https://example.net/"_ns))) {
    return nullptr;
  }
  nsCOMPtr<nsIChannel> channel;
  if (NS_FAILED(NS_NewChannel(getter_AddRefs(channel), uri, nsContentUtils::GetSystemPrincipal(),
                              nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
                              nsIContentPolicy::TYPE_OTHER))) {
    return nullptr;
  }
  net::LoadInfoArgs args;
  nsCOMPtr<nsILoadInfo> loadInfo = channel->LoadInfo();
  if (NS_FAILED(ipc::LoadInfoToLoadInfoArgs(loadInfo, &args))) {
    return nullptr;
  }
  auto config = std::make_shared<NrSocketProxyConfig>(net::WebrtcProxyConfig(dom::TabId(), args, false));
  RefPtr<net::PcUdpProxy> proxy = config->UdpProxy();
  if (!proxy) {
    return nullptr;
  }
  // Bounded: a lookup that never answers fails the test, it does not hang the job. The spin sleeps until an event arrives, so an
  // empty event is queued for the deadline: the predicate is asked again then, whatever else happens.
  const TimeStamp deadline = TimeStamp::Now() + TimeDuration::FromSeconds(10);
  (void)NS_DelayedDispatchToCurrentThread(NS_NewRunnableFunction("StealthTestProxyKitDeadline", [] {}), 10000);
  if (!SpinEventLoopUntil("StealthTestProxyKit"_ns, [&] {
        return proxy->GetKind() != net::PcUdpProxy::Kind::Resolving || TimeStamp::Now() > deadline;
      })) {
    return nullptr;
  }
  if (proxy->GetKind() == net::PcUdpProxy::Kind::Resolving) {
    return nullptr;
  }
  return config;
}

}  // namespace mozilla::stealthtest
