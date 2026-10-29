/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthDNSDeclaration.h"

#include "mozilla/StaticPrefs_zoom.h"

namespace mozilla::net {

bool StealthDNSProxyDeclared() {
  return StaticPrefs::zoom_stealth_dns_no_local_resolution();
}

}  // namespace mozilla::net
