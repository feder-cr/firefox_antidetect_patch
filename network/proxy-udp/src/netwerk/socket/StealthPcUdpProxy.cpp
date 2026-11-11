/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthPcUdpProxy.h"

#include "mozilla/net/ProxyConfigLookup.h"
#include "mozilla/Maybe.h"
#include "mozilla/net/NeckoChannelParams.h"
#include "nsIProtocolProxyService.h"
#include "nsIProxyInfo.h"
#include "nsIURI.h"
#include "nsNetUtil.h"
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
  // The question is "which proxy does this browser context use for the internet?", not "for the address of this page": the
  // UDP of a PeerConnection goes to STUN and TURN servers on the internet whatever the page is. A page on the loopback, a
  // file:, a data: or a sandboxed srcdoc has no proxy of its own address (Necko answers "none" for them, or runs no filter),
  // and that answer must not become "this PeerConnection has no proxy" while the context has one. So the lookup asks about a
  // public https address, with the load info of the document: the proxy filters then see the context of the page.
  nsCOMPtr<nsIURI> uri;
  nsresult rv = NS_NewURI(getter_AddRefs(uri), "https://stun.invalid/"_ns);
  if (NS_FAILED(rv)) {
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
          uint32_t flags = 0;
          aProxyInfo->GetFlags(&flags);
          if (type.EqualsLiteral("socks") && (flags & nsIProxyInfo::CARRIES_COHERENT_UDP)) {
            // SOCKS5 whose UDP the session measured coherent: the road of the PeerConnection's UDP.
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
