# network/doh

Category: build, other
Prefs: none (network.trr.* are Firefox's own; the build keeps network.trr.async_connInfo off and network.trr.exclude-etc-hosts false)
Test: TestStealthTRRContext.* (netwerk/test/gtest/TestStealthTRRContext.cpp, in the default gtest filter of the candidate); the node
tests of juggler/tests: dns_has_no_gate_of_ours, doh_never_goes_direct_when_its_proxy_is_lost,
doh_request_carries_the_context_of_its_lookup, proxy_info_leaves_name_resolution_to_the_browser,
trr_connection_info_is_not_cached_in_this_build, etc_hosts_exclusion_is_off, http_proxy_credentials_are_sent_preemptively_for_doh,
parental_controls_are_declared_off_where_the_os_is_read, no_channel_leaves_before_the_session_proxy_is_decided,
no_channel_leaves_before_the_network_observer_exists, no_ping_leaves_before_the_session_proxy_is_decided

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
- Parental controls are declared off, where the OS is read. The constructor of every `nsIParentalControlsService` implementation
  (Windows, Cocoa, Android, Default) takes the declared value (`StealthParentalControlsDeclared`, a header of toolkit/components/
  parentalcontrols) and returns before its first OS call. Every reader (`nsHttpHandler`, the DoH heuristics, downloads,
  `CanonicalBrowsingContext`) goes through the service and sees the same value, so a host setting does not skip TRR
  (TRR_PARENTAL_CONTROL), add "Prefer: safe", or block downloads. The hosts file is not an exclusion either:
  `network.trr.exclude-etc-hosts` is false (guarded by a test).
- Credentials of an http/https session proxy. `TRRServiceChannel` has no auth provider, so the 407 on the DoH CONNECT would never be
  answered: the channel is seeded with a preemptive `Proxy-Authorization` Basic header (UTF-8 bytes of user:pass) built from the
  nsIProxyInfo in `SessionRoute.js`. SOCKS keeps it empty: its credentials travel in the handshake.
- No channel before the proxy is decided (Juggler files, owned by automation/juggler). The proxy filter starts pending: channels asked
  before the first `Browser.setBrowserProxy` / `Browser.setNoBrowserProxy` wait unanswered (DoH lookups come through the same filter),
  and a proxy that cannot be expressed throws and leaves them pending. The NetworkObserver exists only from final-ui-startup, so
  `juggler/EarlyProxyHold.js` registers the hold filter at command-line-startup, once the pipe flag is known, and the NetworkObserver
  adopts it (one registration; without the flag nothing is registered). `TargetRegistry.roadOfSession` answers {decided, proxy}:
  the pingsender at exit does not start while the session is undecided (a null record means DIRECT).

## Hooks in Mozilla files

- `netwerk/dns/TRR.cpp`: the context goes to the channel's load info; the cached connection info is skipped for a non-default context.
- `netwerk/protocol/http/TRRServiceChannel.cpp`: the proxy lookup is made for the load info's context and fails closed.
- `toolkit/components/parentalcontrols/nsParentalControlsService{Win,Cocoa,Android,Default}.{cpp,mm}`: the declared value before the first OS call.
- Hooks of this feature in files another folder owns (a file has one owner): `netwerk/base/TRRLoadInfo.cpp`
  (automation/navigation), `netwerk/ipc/ProxyConfigLookup*.{h,cpp}`, `PSocketProcess.ipdl`, `SocketProcessParent.{h,cpp}`,
  `netwerk/ipc/moz.build` (network/proxy-udp: the lookup carries the origin attributes next to the load info),
  `netwerk/test/gtest/moz.build` (network/socks). Their lines are tagged `STEALTH-HOOK(network/doh)`. The Juggler files of the pending
  filter (NetworkObserver.js, TargetRegistry.js, Protocol.js, BrowserHandler.js, Juggler.js, SessionRoute.js) belong to automation/juggler.

## Our own files

- `netwerk/ipc/StealthTRRContext.{h,cpp}`, `toolkit/components/parentalcontrols/StealthParentalControls.h`,
  `netwerk/test/gtest/TestStealthTRRContext.cpp`, `juggler/EarlyProxyHold.js`, and the node tests above.
