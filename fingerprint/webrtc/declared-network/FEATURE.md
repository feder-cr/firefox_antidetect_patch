# fingerprint/webrtc/declared-network

Category: build, other, spoof
Prefs: none (the declaration is the environment variable STEALTHFOX_WEBRTC_NETWORK, read where the interfaces are enumerated)
Test: StealthDeclaredNetwork.* (dom/media/webrtc/transport/test/gtest/TestStealthDeclaredNetwork.cpp, in the default gtest filter of
the candidate); the mtransport unit tests gather on a declared adapter (test/gtest_utils.h); the node test
juggler/tests/ipv6_is_one_fact_of_the_declared_network.test.mjs

## What it does

The network WebRTC gathers on is declared by the core, never read from the host.

- R1, the interfaces. `nr_stun_get_addrs` returns the declared adapter: one private IPv4 address of a LAN (RFC 1918) and, with
  `v6=1` and a global unicast `/64` prefix of the exit, a temporary IPv6 address on the same adapter (an interface identifier
  drawn at random once per process, RFC 4941). `v6=1` without a prefix is no IPv6. Without a declaration, or with one that does
  not parse, there is no interface and an error in the log: never a fallback to the host. The OS enumerators (addrs-win32,
  addrs-netlink, addrs-netlink-wireless, addrs-bsd) are not built and not in the tree.
- R2, the default route. The default address of a family is the declared entry of that family; there is no route probe (no socket
  connected to the page's server or to a public address, no getsockname). A family the declaration does not have has no default
  address. A declared machine without a family asks for the family it has (`nr_stealth_default_family`): a page reached over IPv6 on
  a machine declared without IPv6 gets the IPv4 default address. When there is no address at all the gathering fails as upstream does.
- IPv6 is one fact, the declared network's. The same parse decides `network.dns.disableIPv6` in the parent (`StealthApplyDeclaredIPFamily`,
  once, at the top of `nsIOService::Init`): no IPv6 interface declared means no AAAA lookup in any process, so a page is never loaded over
  IPv6 while ICE says there is none. It is a default value (never written to the profile, the user value cleared); with no declaration, or one
  that does not parse, nothing is touched. The answer is the parser's own (`nr_stealth_declared_has_ipv6`, quiet), never a second reading. An IPv6 literal in an address, which needs no lookup, fails like a host with no IPv6 route (network/ipv6-literal).
- R3, the sockets. A host UDP socket binds the wildcard of its family on a port of its connection and reports the declared address
  with that port. The ports are consecutive in the order the sockets are made, from a base drawn at random in 49152-65535 for each
  connection, as Windows hands them out; a port another socket holds, or one the system keeps (Windows reserves ranges of the dynamic
  range and answers "access denied"), is skipped, and the search goes through the whole range (`StealthHostPorts::NextAvailable`; the hook
  in `nr_socket_local_create` passes it the attempt). The port is registered as the last step of `NrSocket::create`, so a create that aborts
  leaves nothing in the set. A datagram between two sockets of this process goes on the
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
- `netwerk/base/nsIOService.cpp`: the IPv6 fact, one call at the top of `Init` in the parent (include behind `MOZ_WEBRTC`).
- `dom/media/webrtc/transport/ipc/StunAddrsRequestParent.cpp`, `mdns_service/src/lib.rs`: the responder on the loopback.
- Build lists: `dom/media/webrtc/transport/{moz.build,common.build,build/moz.build}`, `third_party/nICEr/{nicer.gyp,moz.yaml}`,
  `test/gtest_utils.h`.
  `common.build` has our source line (`nr_stealth_network.cpp`) inside Mozilla's `transport_lcppsrcs` list, without a marker: it is
  the one hook of ours that sits in a list (the rule is to put it after the closing bracket); the check does not need a marker there.
- `dom/media/webrtc/transport/nr_socket_prsock.{h,cpp}` carries the R3 hooks (bind, own ports, loopback rewrite); the file belongs to
  network/proxy-udp, which has the other hooks in it. Those lines are tagged `STEALTH-HOOK(fingerprint/webrtc/declared-network)`.

## Our own files

- `dom/media/webrtc/transport/nr_stealth_network.{h,cpp}` (the declaration, its parser, the default address, the host ports, the own-port
  rewrite, the IPv6 fact and the default family), `dom/media/webrtc/transport/test/gtest/{moz.build,TestStealthDeclaredNetwork.cpp}`,
  `juggler/tests/ipv6_is_one_fact_of_the_declared_network.test.mjs`.
- `third_party/libwebrtc/api/location.h`: a header shim of the libwebrtc overrides, unrelated to the declaration (it lives here
  because the folder it was in is gone).
