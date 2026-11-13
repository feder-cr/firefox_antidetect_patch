/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/doh): see StealthIceResolverContext.h.
// STEALTH-HOOK(network/proxy-udp): the road of the TCP sockets of a PeerConnection (StealthTcpDirectRefused, declared in
// nr_stealth_network.h, which WebrtcTCPSocket.cpp already includes).

#include "StealthIceResolverContext.h"

#include <utility>

#include "MainThreadUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/LoadInfo.h"
#include "mozilla/NullPrincipal.h"
#include "mozilla/Preferences.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Result.h"
#include "mozilla/dom/BrowserChild.h"
#include "mozilla/dom/ipc/IdType.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/net/NeckoChannelParams.h"
#include "mozilla/net/WebrtcProxyConfig.h"
#include "nr_socket_proxy_config.h"
#include "nr_stealth_network.h"
#include "nsCOMPtr.h"
#include "nsDebug.h"
#include "nsIContentPolicy.h"
#include "nsILoadInfo.h"
#include "nsIPrincipal.h"
#include "nsIProxyInfo.h"
#include "nsIScriptObjectPrincipal.h"
#include "nsISocketTransport.h"
#include "nsPIDOMWindow.h"
#include "nsThreadUtils.h"
#include "prnetdb.h"

namespace mozilla {

OriginAttributes StealthIceResolverAttributes(
    const OriginAttributes& aPeerConnection) {
  OriginAttributes attrs;
  attrs.mUserContextId = aPeerConnection.mUserContextId;
  return attrs;
}

OriginAttributes StealthIceResolverAttributes(
    const NrSocketProxyConfig& aConfig) {
  return StealthIceResolverAttributes(
      aConfig.GetConfig().loadInfoArgs().originAttributes());
}

void StealthTcpNameContext(nsISocketTransport* aTransport,
                           const net::WebrtcProxyConfig* aConfig) {
  if (aTransport && aConfig) {
    (void)aTransport->SetOriginAttributes(StealthIceResolverAttributes(
        aConfig->loadInfoArgs().originAttributes()));
  }
}

bool StealthTcpDirectRefused(bool aHasProxyConfig, bool aLookupFailed,
                             nsIProxyInfo* aInfo) {
  // Only the answer "no proxy" (the lookup succeeded and gave no info) opens the socket plain. Anything else that reaches the
  // end of OnProxyAvailable is a failed lookup or a proxy info of a type the socket does not know: refused, as UDP does
  // (PcUdpProxy::KindFor: a failed lookup is a proxy that is not known).
  return aHasProxyConfig && (aLookupFailed || aInfo != nullptr);
}

bool StealthTcpSocketBorn(nsISocketTransport* aTransport,
                          const net::WebrtcProxyConfig* aConfig,
                          const char* aDeclaredAddress, uint16_t aNicerPort,
                          PRNetAddr* aOut) {
  StealthTcpNameContext(aTransport, aConfig);
  return StealthTcpBindAddress(aDeclaredAddress, aNicerPort, aOut);
}

std::unique_ptr<NrSocketProxyConfig> StealthProxyConfigOfPrincipal(
    nsIPrincipal* aPrincipal, uint64_t aTabId, bool aForceProxy) {
  MOZ_ASSERT(NS_IsMainThread());
  nsCOMPtr<nsIPrincipal> principal = aPrincipal;
  bool forceProxy = aForceProxy;
  if (!principal) {
    principal = NullPrincipal::CreateWithoutOriginAttributes();
    forceProxy = true;
  }
  // No loading node: LoadInfo::Create only refuses a load whose node is a document without ClientInfo
  // (IsDocumentMissingClientInfo returns false when there is no node), which is what a torn-down window has.
  Result<RefPtr<net::LoadInfo>, nsresult> maybeLoadInfo = net::LoadInfo::Create(
      principal, principal, nullptr,
      nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
      nsIContentPolicy::TYPE_PROXIED_WEBRTC_MEDIA);
  if (NS_WARN_IF(maybeLoadInfo.isErr())) {
    return nullptr;
  }
  RefPtr<net::LoadInfo> loadInfo = maybeLoadInfo.unwrap();
  net::LoadInfoArgs loadInfoArgs;
  if (NS_WARN_IF(NS_FAILED(
          mozilla::ipc::LoadInfoToLoadInfoArgs(loadInfo, &loadInfoArgs)))) {
    return nullptr;
  }
  return std::make_unique<NrSocketProxyConfig>(
      net::WebrtcProxyConfig(dom::TabId(aTabId), loadInfoArgs, forceProxy));
}

std::unique_ptr<NrSocketProxyConfig> StealthProxyConfigOf(
    std::unique_ptr<NrSocketProxyConfig> aUpstream, nsPIDOMWindowInner* aWindow,
    bool aForceProxy) {
  if (aUpstream) {
    return aUpstream;
  }
  if (!aForceProxy &&
      Preferences::GetBool("media.peerconnection.disable_http_proxy", false)) {
    return nullptr;
  }
  uint64_t tabId = 0;
  nsCOMPtr<nsIPrincipal> principal;
  if (aWindow) {
    if (dom::BrowserChild* browserChild = dom::BrowserChild::GetFrom(aWindow)) {
      tabId = browserChild->GetTabId();
    }
    if (nsCOMPtr<nsIScriptObjectPrincipal> scriptPrincipal =
            do_QueryInterface(aWindow)) {
      principal = scriptPrincipal->GetPrincipal();
    }
  }
  std::unique_ptr<NrSocketProxyConfig> config =
      StealthProxyConfigOfPrincipal(principal, tabId, aForceProxy);
  if (!config && principal) {
    // The config could not be made for the principal of the window: it is made for no principal at all, which forces
    // the proxy. A PeerConnection left with no config would have plain sockets.
    config = StealthProxyConfigOfPrincipal(nullptr, tabId, true);
  }
  return config;
}

}  // namespace mozilla
