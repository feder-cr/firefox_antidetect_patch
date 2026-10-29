/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthIceGathering.h"

#include "mozilla/StaticPrefs_zoom.h"
#include "nsString.h"

namespace mozilla {

// A content process that received no STUN addresses from the parent normally
// stops before StartIceGathering, because gathering them itself would hit the
// sandbox. The session can declare a host IP (zoom.stealth.webrtc.host_ip);
// when it does, gathering goes on without the parent addresses. The synthetic
// candidates come from the environment variable STEALTHFOX_WEBRTC_PUBLIC_IP
// through nr_stealth_bridge, and are injected in
// nr_ice_component_initialize_udp (nr_ice_component_inject_fallback_srflx),
// so gathering completes without probing the real network. An empty pref keeps Mozilla's early return.
bool StealthWebrtcHostIpDeclared() {
  auto lock = StaticPrefs::zoom_stealth_webrtc_host_ip();
  return !(*lock).IsEmpty();
}

}  // namespace mozilla
