/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthDocumentChannel.h"

#include "mozilla/Components.h"
#include "mozilla/dom/RemoteType.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "nsContentUtils.h"
#include "nsIChannel.h"
#include "nsIContentPolicy.h"
#include "nsIIOService.h"
#include "nsILoadInfo.h"

namespace mozilla::net {

nsresult StealthNewChannelOfLoadInfo(nsIURI* aURI, uint32_t aProxyResolveFlags,
                                     const LoadInfoArgs& aLoadInfoArgs,
                                     nsIChannel** aResult,
                                     nsILoadInfo** aLoadInfoOut) {
  MOZ_ASSERT(NS_IsMainThread());
  nsresult rv;
  nsCOMPtr<nsIIOService> ioService = components::IO::Service(&rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsILoadInfo> loadInfo;
  // As WebrtcTCPSocket::OpenWithHttpProxy: the remote type of the process that
  // sent the arguments is not used for a security decision here.
  rv = ipc::LoadInfoArgsToLoadInfo(aLoadInfoArgs, dom::RemoteType::NotRemote(),
                                   getter_AddRefs(loadInfo));
  NS_ENSURE_SUCCESS(rv, rv);

  // -SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL: the channel is used to CONNECT to an HTTP proxy, so it must be allowed
  // to load from any origin.
  rv = ioService->NewChannelFromURIWithProxyFlags(
      aURI, aURI, aProxyResolveFlags, loadInfo->LoadingNode(),
      loadInfo->GetLoadingPrincipal(), loadInfo->TriggeringPrincipal(),
      nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
      nsIContentPolicy::TYPE_PROXIED_WEBRTC_MEDIA, aResult);
  NS_ENSURE_SUCCESS(rv, rv);
  if (aLoadInfoOut) {
    loadInfo.forget(aLoadInfoOut);
  }
  return NS_OK;
}

}  // namespace mozilla::net
