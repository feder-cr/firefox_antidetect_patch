# network/doh

Category: build, other
Prefs: none (network.trr.* are Firefox's own; the build keeps network.trr.async_connInfo off and network.trr.exclude-etc-hosts false)
Test: TestStealthTRRContext.* (netwerk/test/gtest/TestStealthTRRContext.cpp); the node tests juggler/tests/dns_has_no_gate_of_ours,
doh_never_goes_direct_when_its_proxy_is_lost, doh_request_carries_the_context_of_its_lookup,
proxy_info_leaves_name_resolution_to_the_browser, resolver_never_asks_the_os_for_parental_controls,
trr_connection_info_is_not_cached_in_this_build

## What it does

Behind a proxy the names are resolved by Firefox's own DNS-over-HTTPS (TRR), and the DoH request leaves by the proxy of the
browser context whose lookup caused it. There is no DNS gate of ours: the resolver is upstream's, and the proxy info no longer
tells the proxy to resolve the host (the browser resolves the name and connects to the IP).

- The context. `StealthTRRContext` (netwerk/ipc) takes the user context id from the lookup's origin suffix and gives it to the load
  info of the TRR channel (`TRRLoadInfo` keeps the origin attributes, also in its clone) and to the stand-in channel of the proxy
  lookup, so the proxy filters (the Juggler's) answer for that context. The default context carries nothing: the browser's proxy
  answers it, as upstream. The cached connection info of the default server is not used for another context.
- Fail closed. A DoH request whose proxy lookup fails or cannot start ends with an error (`StealthTRRProxyFailure`) instead of
  failing over to DIRECT, which would send the name from the machine's own address. A cancellation keeps its own code.
- Parental controls are declared off (`StealthParentalControlsEnabled`, netwerk/dns): `nsHttpHandler::UpdateParentalControlsEnabled`
  returns before the OS service is created, so TRR is not skipped by a host setting and "Prefer: safe" is not sent.

## Hooks in Mozilla files

- `netwerk/dns/TRR.cpp`: the context goes to the channel's load info; the cached connection info is skipped for a non-default context.
- `netwerk/protocol/http/TRRServiceChannel.cpp`: the proxy lookup is made for the load info's context and fails closed.
- `netwerk/dns/moz.build`: registers StealthParentalControls.cpp.
- Hooks of this feature in files another folder owns (a file has one owner): `netwerk/base/TRRLoadInfo.cpp`
  (automation/navigation), `netwerk/protocol/http/nsHttpHandler.cpp` (fingerprint/locale-time/accept-language),
  `netwerk/ipc/ProxyConfigLookup*.{h,cpp}`, `PSocketProcess.ipdl`, `SocketProcessParent.{h,cpp}`, `netwerk/ipc/moz.build`
  (network/proxy-udp: the lookup carries the origin attributes next to the load info), `netwerk/test/gtest/moz.build` (network/socks).
  Their lines are tagged `STEALTH-HOOK(network/doh)`.

## Our own files

- `netwerk/ipc/StealthTRRContext.{h,cpp}`, `netwerk/dns/StealthParentalControls.{h,cpp}`,
  `netwerk/test/gtest/TestStealthTRRContext.cpp`, and the six node tests above.
