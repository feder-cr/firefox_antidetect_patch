# fingerprint/webrtc/ice-candidates

Category: build, spoof
Prefs: zoom.stealth.webrtc.host_ip
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/media/webrtc/jsapi/PeerConnectionImpl.cpp` (PeerConnectionImpl::EnsureIceGathering, movable): Content-process gathering proceeds without parent STUN addresses when zoom.stealth.webrtc.host_ip is set (the pref is only tested for empty). Pref defined at modules/libpref/init/StaticPrefList.yaml:21977. The added comment says nICEr injects synthetic candidates 'from public_ip pref'; CONFIRMED stale: that pref was removed (StaticPrefList.yaml:21964-21972) and the public IP now comes from env STE
- `dom/media/webrtc/transport/common.build` (transport_lcppsrcs (source list), already-hook): Registers our own file dom/media/webrtc/transport/nr_stealth_bridge.cpp (present in C:/ff/source). The bridge functions used by the ICE and socket files read environment variables only, not prefs (nr_stealth_bridge.cpp:65-67 and 69-75).
- `dom/media/webrtc/transport/third_party/nICEr/nicer.gyp` (include_dirs of the nICEr target (target name not in the hunk header), already-hook): Adds the transport directory so nICEr sources can include nr_stealth_bridge.h. The relative path resolves to dom/media/webrtc/transport. nicer.gyp is live: dom/media/webrtc/transport/third_party/moz.build:24 builds nICEr from it via GYP_DIRS. Of the 3 added lines, one is a comment and one is blank.
- `dom/media/webrtc/transport/third_party/nICEr/src/ice/ice_candidate.cpp` (nr_ice_srvrflx_stun_finished_cb (case NR_STUN_CLIENT_STATE_DONE), movable): Two blocks in the STUN-done case: suppress the real srflx when a synthetic one (stun_server == NULL) exists, and rewrite the mapped IP to the proxy IP keeping the port. Both could move behind one hook call. The public IP comes from env STEALTHFOX_WEBRTC_PUBLIC_IP via nr_stealth_bridge.cpp, not a pref. Also adds an include of ice_component.h that nothing in this file uses (only a comment at ice_can
- `dom/media/webrtc/transport/third_party/nICEr/src/ice/ice_component.cpp` (new static nr_ice_stealth_srflx_fire_ready and new nr_ice_component_inject_fallback_srflx (inserted after the includes), plus the call in nr_ice_component_initialize_udp, movable): About 150 logic lines of new code (the rest of the 370 added lines are comments) in a Mozilla file. Both functions can move to our own file, leaving the call site and a declaration. Reads env STEALTHFOX_WEBRTC_PUBLIC_IP through the bridge, no pref. Dead variable CONFIRMED: real_srflx_ct is declared at ice_component.cpp:603 and incremented at :623, never read (the only other mentions are comments a
- `dom/media/webrtc/transport/third_party/nICEr/src/ice/ice_component.h` (file-level declarations after nr_ice_component_dump_state, already-hook): Declaration only. The header comment says the function has two callers, including the STUN-failure branch in ice_candidate.cpp. CONFIRMED stale: the only caller in the tree is ice_component.cpp:649; ice_candidate.cpp only has an include and a comment (ice_candidate.cpp:56). Moves with the definition if that moves.
- `dom/media/webrtc/transport/third_party/nICEr/src/ice/ice_ctx.cpp` (new static nr_transport_addr_is_wildcard_ip (before nr_ice_get_default_address); wildcard refusal in nr_ice_get_default_address; fallback in nr_ice_set_local_addresses, movable): Two behaviour changes: a wildcard remote is refused before the default-route probe, and when the probe yields no default address ICE falls back to the filtered local addresses instead of failing. The fallback block (about 12 logic lines) can move behind one call. addr_ct holds the filtered survivor count at the fallback: CONFIRMED (ice_ctx.cpp:770-791 filter/force-interface, used at 843-848). The 
- `dom/media/webrtc/transport/third_party/nICEr/src/stun/addrs.cpp` (nr_stun_filter_addrs_for_ifname (else-if chain), already-hook): Drops IPv6 host addresses when nr_stealth_webrtc_disable_ipv6() is true. That reads env STEALTHFOX_WEBRTC_DISABLE_IPV6 (nr_stealth_bridge.cpp:69-75), not a pref. The comment says the upstream pref media.peerconnection.ice.disableIPv6 is dead in FF150: CONFIRMED for dom/media (no reader other than our own comments) and modules/libpref/init (not defined). That IPv6 srflx derived from the host is als

## Our own files

- `dom/media/webrtc/transport/nr_stealth_bridge.cpp`
- `dom/media/webrtc/transport/nr_stealth_bridge.h`
- `third_party/libwebrtc/api/location.h`
