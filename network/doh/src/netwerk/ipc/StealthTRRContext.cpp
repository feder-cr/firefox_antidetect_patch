/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthTRRContext.h"

#include "nsCOMPtr.h"
#include "nsDebug.h"
#include "nsIChannel.h"
#include "nsILoadInfo.h"
#include "nsISocketTransport.h"
#include "nsString.h"

namespace mozilla::net {

static const uint32_t kStealthDefaultUserContextId = 0;

static Maybe<OriginAttributes> StealthContextOfAttributes(
    const OriginAttributes& aAttrs) {
  if (aAttrs.mUserContextId == kStealthDefaultUserContextId) {
    return Nothing();
  }
  OriginAttributes context;
  context.mUserContextId = aAttrs.mUserContextId;
  return Some(context);
}

Maybe<OriginAttributes> StealthTRRContextOfSuffix(
    const nsACString& aOriginSuffix) {
  OriginAttributes attrs;
  if (!attrs.PopulateFromSuffix(aOriginSuffix)) {
    return Nothing();
  }
  return StealthContextOfAttributes(attrs);
}

Maybe<OriginAttributes> StealthTRRContextOfLoadInfo(nsILoadInfo* aLoadInfo) {
  if (!aLoadInfo) {
    return Nothing();
  }
  OriginAttributes attrs;
  if (NS_FAILED(aLoadInfo->GetOriginAttributes(&attrs))) {
    return Nothing();
  }
  return StealthContextOfAttributes(attrs);
}

nsresult StealthApplyTRRContext(nsILoadInfo* aLoadInfo,
                                const Maybe<OriginAttributes>& aContext) {
  if (aContext.isNothing()) {
    return NS_OK;
  }
  NS_ENSURE_TRUE(aLoadInfo, NS_ERROR_UNEXPECTED);
  return aLoadInfo->SetOriginAttributes(*aContext);
}

nsresult StealthApplyTRRContextToChannel(
    nsIChannel* aChannel, const Maybe<OriginAttributes>& aContext) {
  if (aContext.isNothing()) {
    return NS_OK;
  }
  NS_ENSURE_TRUE(aChannel, NS_ERROR_UNEXPECTED);
  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
  return StealthApplyTRRContext(loadInfo, aContext);
}

void StealthKeepRequestContext(const OriginAttributes& aPage,
                               OriginAttributes& aRequest) {
  aRequest.mUserContextId = aPage.mUserContextId;
}

OriginAttributes StealthTransportContext(nsISocketTransport* aTransport) {
  OriginAttributes attrs;
  if (aTransport) {
    (void)aTransport->GetOriginAttributes(&attrs);
  }
  return attrs;
}

nsresult StealthTRRProxyFailure(nsresult aProxyStatus) {
  if (aProxyStatus == NS_ERROR_ABORT || aProxyStatus == NS_BINDING_ABORTED) {
    return aProxyStatus;
  }
  return NS_ERROR_PROXY_CONNECTION_REFUSED;
}

bool StealthFailClosedOnProxyFailure(nsresult aProxyStatus,
                                     bool aIsTRRServiceChannel,
                                     bool aCanceled) {
  return NS_FAILED(aProxyStatus) && aIsTRRServiceChannel && !aCanceled;
}

}  // namespace mozilla::net
