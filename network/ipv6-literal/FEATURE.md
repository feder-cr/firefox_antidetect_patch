# network/ipv6-literal

Category: other
Prefs: none (the fact is the declared network's: network.dns.disableIPv6, set from the declaration by fingerprint/webrtc/declared-network)
Test: StealthDeclaredNetwork.* (netwerk/test/gtest/TestStealthIPv6Literal.cpp: AnIPv6LiteralFailsLikeAHostWithNoIPv6Route,
NothingIsBlockedWhileIPv6IsDeclared, AnAddressThatIsNotIPv6OrIPIsLeftAlone, TheConnectFailsWithTheRealErrorAndSendsNothing; the suite
name is the one already in the default gtest filter of the candidate); the node test
juggler/tests/ipv6_literal_fails_like_a_host_with_no_ipv6_route.test.mjs

## What it does

With IPv6 declared off, a name never resolves to AAAA, but `nsHostResolver::FromIPLiteral` hands `http://[2001:db8::1]/` straight to the
socket and the host's own route would decide. Every direct TCP connection (pages, fetch, XHR, WebSocket, Happy Eyeballs, DoH, a
connection to an HTTP proxy) passes `nsSocketTransport::InitiateSocket`, and a SOCKS connection reaches its proxy in
`nsSOCKSSocketInfo::ConnectToProxy`: both call `StealthIPv6Connect` instead of `PR_Connect`. With the declaration, a routable IPv6
destination fails with `PR_NETWORK_UNREACHABLE_ERROR` (WSAENETUNREACH), the unspecified and multicast ones with
`PR_ADDRESS_NOT_AVAILABLE_ERROR`, and nothing is sent; both become `NS_ERROR_CONNECTION_REFUSED` as on Windows. `::1`, link-local and
IPv4-mapped addresses are left alone, and so is a destination behind a SOCKS proxy, which the proxy reaches, not this host.

## Hooks in Mozilla files

- `netwerk/base/nsSocketTransport2.cpp`: the include and the connect in `InitiateSocket` (3 lines added, 1 of Mozilla's replaced).
- Hooks of this feature in files another folder owns (a file has one owner): `netwerk/socket/nsSOCKSIOLayer.cpp` (network/proxy-udp:
  the connect to the proxy), `netwerk/base/moz.build` and `netwerk/test/gtest/moz.build` (network/socks). Their lines are tagged
  `STEALTH-HOOK(network/ipv6-literal)`.

## Our own files

- `netwerk/base/StealthIPv6Literal.{h,cpp}`, `netwerk/test/gtest/TestStealthIPv6Literal.cpp`,
  `juggler/tests/ipv6_literal_fails_like_a_host_with_no_ipv6_route.test.mjs`.
