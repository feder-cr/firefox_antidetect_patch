/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/StealthGeolocation.h"

#include "nsServiceManagerUtils.h"

namespace mozilla {

const char* StealthGeolocationProviderContract() {
  return "@mozilla.org/geolocation/provider;1";
}

bool StealthSelectGeolocationProvider(
    nsCOMPtr<nsIGeolocationProvider>& aProvider) {
  aProvider = do_GetService(StealthGeolocationProviderContract());
  return true;
}

bool StealthSystemLocationDeclaredOn() { return true; }

}  // namespace mozilla
