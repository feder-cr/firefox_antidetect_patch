/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(network/doh): see StealthIceResolverContext.h.

#include "StealthIceResolverContext.h"

#include "mozilla/net/WebrtcProxyConfig.h"
#include "nr_socket_proxy_config.h"

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

}  // namespace mozilla
