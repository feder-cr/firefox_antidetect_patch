/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_WEBRTC_JSAPI_STEALTHICEGATHERING_H_
#define DOM_MEDIA_WEBRTC_JSAPI_STEALTHICEGATHERING_H_

namespace mozilla {

/**
 * True when the session declares a host IP (pref zoom.stealth.webrtc.host_ip is
 * not empty).
 *
 * PeerConnectionImpl::EnsureIceGathering uses it in a content process that
 * received no STUN addresses from the parent: with a declared host IP,
 * gathering goes ahead instead of stopping. Main thread only, like its caller.
 * It reads only the pref and touches no PeerConnectionImpl member.
 */
bool StealthWebrtcHostIpDeclared();

}  // namespace mozilla

#endif  // DOM_MEDIA_WEBRTC_JSAPI_STEALTHICEGATHERING_H_
