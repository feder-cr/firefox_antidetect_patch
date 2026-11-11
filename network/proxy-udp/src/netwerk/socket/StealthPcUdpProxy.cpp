/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthPcUdpProxy.h"

#include "mozilla/net/ProxyConfigLookup.h"
#include "mozilla/Maybe.h"
#include "mozilla/dom/RemoteType.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/net/NeckoChannelParams.h"
#include "nsIPrincipal.h"
#include "nsIProtocolProxyService.h"
#include "nsIProxyInfo.h"
#include "nsIURI.h"
#include "nsThreadUtils.h"

namespace mozilla::net {

// static
already_AddRefed<PcUdpProxy> PcUdpProxy::Known(Kind aKind, const Socks5Info& aInfo) {
  RefPtr<PcUdpProxy> proxy = new PcUdpProxy();
  proxy->mKind = aKind;
  proxy->mInfo = aInfo;
  return proxy.forget();
}

// static
already_AddRefed<PcUdpProxy> PcUdpProxy::Lookup(const LoadInfoArgs& aLoadInfo) {
  RefPtr<PcUdpProxy> proxy = new PcUdpProxy();
  // The lookup is made on the main thread; the arguments are copied into the runnable.
  (void)NS_DispatchToMainThread(NS_NewRunnableFunction(
      "PcUdpProxy::Lookup", [proxy, args = aLoadInfo]() { proxy->DoLookup(args); }));
  return proxy.forget();
}

void PcUdpProxy::DoLookup(const LoadInfoArgs& aLoadInfo) {
  MOZ_ASSERT(NS_IsMainThread());
  nsCOMPtr<nsILoadInfo> loadInfo;
  nsCOMPtr<nsIURI> uri;
  // As WebrtcTCPSocket: the remote type of the sender is not used for a security decision.
  nsresult rv = ipc::LoadInfoArgsToLoadInfo(aLoadInfo, dom::RemoteType::NotRemote(), getter_AddRefs(loadInfo));
  if (NS_SUCCEEDED(rv)) {
    nsCOMPtr<nsIPrincipal> principal = loadInfo->GetLoadingPrincipal();
    rv = principal ? principal->GetURI(getter_AddRefs(uri)) : NS_ERROR_FAILURE;
  }
  if (NS_FAILED(rv) || !uri) {
    // The document has no address to ask a filter about (a null principal). Not knowing the proxy is not knowing there is
    // none: nothing of this PeerConnection's UDP goes out (as behind a router that drops it), never direct by default.
    Resolved(Kind::Blocked, Socks5Info());
    return;
  }
  RefPtr<PcUdpProxy> self = this;
  rv = ProxyConfigLookup::Create(
      [self](nsIProxyInfo* aProxyInfo, nsresult aStatus) {
        Socks5Info info;
        // A failed lookup is a proxy that is not known: nothing goes out. A lookup that answers with no proxy info is a
        // PeerConnection with no proxy.
        Kind kind = NS_SUCCEEDED(aStatus) ? Kind::Direct : Kind::Blocked;
        if (NS_SUCCEEDED(aStatus) && aProxyInfo) {
          nsAutoCString type;
          aProxyInfo->GetType(type);
          if (type.EqualsLiteral("socks")) {
            kind = Kind::Socks5;
            aProxyInfo->GetHost(info.mHost);
            aProxyInfo->GetPort(&info.mPort);
            aProxyInfo->GetUsername(info.mUser);
            aProxyInfo->GetPassword(info.mPass);
          } else if (!type.EqualsLiteral("direct")) {
            kind = Kind::Blocked;
          }
        }
        self->Resolved(kind, info);
      },
      uri, nsIProtocolProxyService::RESOLVE_PREFER_SOCKS_PROXY, false, nullptr, Some(aLoadInfo));
  if (NS_FAILED(rv)) {
    // The lookup could not be made: the proxy is not known, so nothing goes out (see above).
    Resolved(Kind::Blocked, Socks5Info());
  }
}

void PcUdpProxy::Resolved(Kind aKind, const Socks5Info& aInfo) {
  nsTArray<std::function<void()>> waiting;
  {
    MutexAutoLock lock(mMutex);
    if (mKind != Kind::Resolving) {
      return;
    }
    mKind = aKind;
    mInfo = aInfo;
    waiting = std::move(mWaiting);
  }
  for (auto& callback : waiting) {
    callback();
  }
}

PcUdpProxy::Kind PcUdpProxy::GetKind(Socks5Info* aInfo) {
  MutexAutoLock lock(mMutex);
  if (aInfo && mKind == Kind::Socks5) {
    *aInfo = mInfo;
  }
  return mKind;
}

void PcUdpProxy::WhenKnown(std::function<void()>&& aCallback) {
  {
    MutexAutoLock lock(mMutex);
    if (mKind == Kind::Resolving) {
      mWaiting.AppendElement(std::move(aCallback));
      return;
    }
  }
  aCallback();
}

}  // namespace mozilla::net
