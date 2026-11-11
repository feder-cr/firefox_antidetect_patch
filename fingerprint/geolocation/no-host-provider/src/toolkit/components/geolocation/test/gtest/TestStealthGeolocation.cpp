/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(fingerprint/geolocation/no-host-provider)
//
// The position a page reads is the core's declaration, never the host's. The
// suite is StealthDeclaredNetwork, which the candidate's gtest filter runs.

#include "gtest/gtest.h"

#include <string.h>

#include "mozilla/StealthGeolocation.h"
#include "nsIGeolocationProvider.h"

TEST(StealthDeclaredNetwork, GeolocationProviderIsTheNetworkProviderOnEveryOS)
{
  // The same contract on Windows, Linux and macOS: the network provider, which
  // with no URL and no Wi-Fi scan reads nobody. Not Portal, GeoClue or CoreLocation,
  // whose construction or start reads the host (Windows has no host provider left).
  EXPECT_STREQ(mozilla::StealthGeolocationProviderContract(),
               NS_GEOLOCATION_PROVIDER_CONTRACTID);
  EXPECT_STREQ(mozilla::StealthGeolocationProviderContract(),
               "@mozilla.org/geolocation/provider;1");
}

TEST(StealthDeclaredNetwork, TheSystemLocationSwitchIsDeclaredOn)
{
  // On, whatever the machine says: the answer does not come from the OS.
  EXPECT_TRUE(mozilla::StealthSystemLocationDeclaredOn());
}
