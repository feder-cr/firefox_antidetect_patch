/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* C-callable bridge from nICEr (C) into mozilla::StaticPrefs (C++).
 *
 * Stealthfox WebRTC realism: when running through a SOCKS5 proxy that
 * blocks UDP, real STUN binding requests time out. The hook in
 * stun_client_ctx.cpp uses these helpers to read the proxy egress IP
 * (`zoom.stealth.webrtc.public_ip` or env var STEALTHFOX_WEBRTC_PUBLIC_IP)
 * and synthesize a STUN binding response so Firefox emits a srflx
 * candidate via its own existing callback path.
 */

#ifndef nr_stealth_bridge_h__
#define nr_stealth_bridge_h__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy the pref value zoom.stealth.webrtc.public_ip into `buf` (NUL
 * terminated). Returns the number of bytes written excluding the NUL,
 * or 0 if the pref is empty. `buf_len` must be >= 1. Reads env var
 * STEALTHFOX_WEBRTC_PUBLIC_IP first, falls back to the pref. */
size_t nr_stealth_get_webrtc_public_ip(char* buf, size_t buf_len);

/* Same for zoom.stealth.webrtc.host_ip. Reserved for Phase 2 (mDNS
 * host candidate). Currently unused. */
size_t nr_stealth_get_webrtc_host_ip(char* buf, size_t buf_len);

/* Returns 1 if IPv6 host candidates must be dropped from ICE gathering, else
 * 0. Reads env var STEALTHFOX_WEBRTC_DISABLE_IPV6 first ("0" = off, any other
 * non-empty value = on), then falls back to the pref
 * zoom.stealth.webrtc.disable_ipv6. The upstream
 * media.peerconnection.ice.disableIPv6 pref is dead in FF150 (read nowhere in
 * the ICE path), so this is the stealth path to stop the real global IPv6 host
 * candidate from leaking past a SOCKS proxy that only carries TCP. */
int nr_stealth_webrtc_disable_ipv6(void);

#ifdef __cplusplus
}
#endif

#endif /* nr_stealth_bridge_h__ */
