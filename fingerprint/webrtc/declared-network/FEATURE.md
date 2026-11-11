# fingerprint/webrtc/declared-network

Category: build, other, spoof
Prefs: none (the declaration is the environment variable STEALTHFOX_WEBRTC_NETWORK, read where the interfaces are enumerated)
Test: StealthDeclaredNetwork.* (dom/media/webrtc/transport/test/gtest/TestStealthDeclaredNetwork.cpp, in the default gtest filter of
the candidate); the mtransport unit tests gather on a declared adapter (test/gtest_utils.h)

## What it does

The network WebRTC gathers on is declared by the core, never read from the host.

- R1, the interfaces. `nr_stun_get_addrs` returns the declared adapter: one private IPv4 address of a LAN (RFC 1918) and, with
  `v6=1` and a global unicast `/64` prefix of the exit, a temporary IPv6 address on the same adapter (an interface identifier
  drawn at random once per process, RFC 4941). `v6=1` without a prefix is no IPv6. Without a declaration, or with one that does
  not parse, there is no interface and an error in the log: never a fallback to the host. The OS enumerators (addrs-win32,
  addrs-netlink, addrs-netlink-wireless, addrs-bsd) are not built and not in the tree.
- R2, the default route. The default address of a family is the declared entry of that family; there is no route probe (no socket
  connected to the page's server or to a public address, no getsockname). A family the declaration does not have has no default
  address.
- R3, the sockets. A host UDP socket binds the wildcard of its family on a port of its connection and reports the declared address
  with that port. The ports are consecutive in the order the sockets are made, from a base drawn at random in 49152-65535 for each
  connection, as Windows hands them out; a busy port is skipped. A datagram between two sockets of this process goes on the
  kernel's loopback while each end sees the other's declared address (`StealthOwnPorts`); a server on localhost keeps its address.
- mDNS. The parent's responder joins the group on the loopback and sends through it (`nr_stealth_mdns_interfaces`; lib.rs sets the
  multicast interface to the address it joined on), so the query and the answer for our own .local names make the same round trip
  as on a real adapter; the names answer with the declared address.

## Hooks in Mozilla files

- `dom/media/webrtc/transport/third_party/nICEr/src/stun/addrs.cpp`: the interfaces are the declaration; the includes of the OS
  enumerators are gone.
- `dom/media/webrtc/transport/third_party/nICEr/src/ice/ice_ctx.cpp`: the default address is the declared one; Mozilla's route probe
  is replaced by one call.
- `dom/media/webrtc/transport/third_party/nICEr/src/stun/addrs-{bsd,netlink,netlink-wireless,win32}.{cpp,h}`: deleted.
- `dom/media/webrtc/transport/nricectx.{h,cpp}`: the host port counter of a connection and its use in `nr_socket_local_create`.
- `dom/media/webrtc/transport/ipc/StunAddrsRequestParent.cpp`, `mdns_service/src/lib.rs`: the responder on the loopback.
- Build lists: `dom/media/webrtc/transport/{moz.build,common.build,build/moz.build}`, `third_party/nICEr/{nicer.gyp,moz.yaml}`,
  `test/gtest_utils.h`.
  `common.build` has our source line (`nr_stealth_network.cpp`) inside Mozilla's `transport_lcppsrcs` list, without a marker: it is
  the one hook of ours that sits in a list (the rule is to put it after the closing bracket); the check does not need a marker there.
- `dom/media/webrtc/transport/nr_socket_prsock.{h,cpp}` carries the R3 hooks (bind, own ports, loopback rewrite); the file belongs to
  network/proxy-udp, which has the other hooks in it. Those lines are tagged `STEALTH-HOOK(fingerprint/webrtc/declared-network)`.

## Our own files

- `dom/media/webrtc/transport/nr_stealth_network.{h,cpp}` (the declaration, its parser, the default address, the host ports, the own-port
  rewrite), `dom/media/webrtc/transport/test/gtest/{moz.build,TestStealthDeclaredNetwork.cpp}`.
- `third_party/libwebrtc/api/location.h`: a header shim of the libwebrtc overrides, unrelated to the declaration (it lives here
  because the folder it was in is gone).
