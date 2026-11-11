/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthSessionUdpProxyService.h"

#include "nsSOCKSUDPIOLayer.h"
#include "nsThreadUtils.h"
#include "nsXULAppAPI.h"

namespace mozilla {

NS_IMPL_ISUPPORTS(StealthSessionUdpProxyService, nsIStealthSessionUdpProxy)

NS_IMETHODIMP
StealthSessionUdpProxyService::Set(const nsACString& aHost, int32_t aPort,
                                   const nsACString& aUser,
                                   const nsACString& aPass) {
  if (!XRE_IsParentProcess() || !NS_IsMainThread()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  if (aHost.IsEmpty() || aPort <= 0 || aPort > 65535) {
    return NS_ERROR_INVALID_ARG;
  }
  net::StealthSessionUdpProxyPublish(aHost, aPort, aUser, aPass);
  return NS_OK;
}

NS_IMETHODIMP
StealthSessionUdpProxyService::Clear() {
  if (!XRE_IsParentProcess() || !NS_IsMainThread()) {
    return NS_ERROR_NOT_AVAILABLE;
  }
  net::StealthSessionUdpProxyPublish(""_ns, 0, ""_ns, ""_ns);
  return NS_OK;
}

}  // namespace mozilla
