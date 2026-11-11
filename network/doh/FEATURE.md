# network/doh

Category: build, other
Prefs: none (network.trr.* are Firefox's own; the build keeps network.trr.async_connInfo off and network.trr.exclude-etc-hosts false)
Test: TestStealthTRRContext.* (netwerk/test/gtest/TestStealthTRRContext.cpp, in the default gtest filter of the candidate); the node
tests of juggler/tests: dns_has_no_gate_of_ours, doh_never_goes_direct_when_its_proxy_is_lost,
doh_request_carries_the_context_of_its_lookup, proxy_info_leaves_name_resolution_to_the_browser,
trr_connection_info_is_not_cached_in_this_build, etc_hosts_exclusion_is_off, http_proxy_credentials_are_sent_preemptively_for_doh,
parental_controls_are_declared_off_where_the_os_is_read, doh_tunnel_is_not_shared_between_credentials,
doh_request_on_the_main_thread_fails_closed, no_channel_leaves_before_the_session_proxy_is_decided,
no_channel_leaves_before_the_network_observer_exists, no_ping_leaves_before_the_session_proxy_is_decided, no_lookup_asks_for_the_system_resolver, trr_follows_the_proxies_present,
warnings_reach_a_sink_that_is_not_gated

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
- Fail closed on the main thread too. With no socket process, or after a proxy 401/407, the DoH request is sent from the main thread by a
  real `nsHttpChannel` (DNSUtils::CreateChannelHelper), whose own `OnProxyAvailable` would fail over to DIRECT. `StealthFailClosedOnProxyFailure`
  is true for a TRR channel, not cancelled, whose proxy lookup failed: it ends with `StealthTRRProxyFailure`. Other channels keep upstream's behaviour.
- The tunnel is isolated by credentials. The connection info of a DoH request carries the origin attributes of the context, so the tunnel
  (CONNECT through the proxy, which is authenticated) of one browser context is never reused by another context with other credentials.
- Parental controls are declared off, where the OS is read. The constructor of every `nsIParentalControlsService` implementation
  (Windows, Cocoa, Android, Default) takes the declared value (`StealthParentalControlsDeclared`, a header of toolkit/components/
  parentalcontrols) and returns before its first OS call. Every reader (`nsHttpHandler`, the DoH heuristics, downloads,
  `CanonicalBrowsingContext`) goes through the service and sees the same value, so a host setting does not skip TRR
  (TRR_PARENTAL_CONTROL), add "Prefer: safe", or block downloads. The hosts file is not an exclusion either:
  `network.trr.exclude-etc-hosts` is false (guarded by a test).
- No lookup asks for the system resolver. Under TRR mode 3 a lookup that asks for the request mode "TRR disabled" is answered by the
  host's resolver anyway, so the services that stay on as in retail follow the session resolver (DoH through the proxy; without a
  proxy the mode is 0 and the system resolver answers as before): the four lookups and the IP check channel of
  `NetworkConnectivityService`, the captive portal check (`CaptiveDetect.sys.mjs`) and tab (`Tabbrowser.sys.mjs`), and the OCSP
  channels (`HttpBaseChannel::GetTRRMode`, which upstream forces to "disabled" under mode 3). The test pins the changed lines and
  keeps an allowlist of every remaining TRR-disabling occurrence in non-test source, each with its verdict.
- The DoH prefs follow the proxies present. `juggler/TrrPrefs.js` derives them from the set of proxies the engine received (the
  browser-wide one plus every live context's): with at least one, mode 3, `network.trr.exclude-etc-hosts` false and
  `network.trr.excluded-domains` are set as user values; with none, exactly those three are cleared. The provider
  (`network.trr.uri`, `network.trr.bootstrapAddr`) is the core's: the engine neither writes nor clears it, and turning DoH on while
  `network.trr.uri` is empty throws before any write, so the proxy command is refused and the previous proxy restored.
  `TargetRegistry` recomputes them at `Browser.setBrowserProxy` / `setNoBrowserProxy` (before the held channels are released),
  `BrowserContext.setProxy` and the end of a context.
- The warnings that explain a hang reach stderr. The no-decision, failed-answer and hold-not-registered warnings go through `say()`
  (`juggler/Say.js`, owned by automation/juggler), which writes with `printStderr`: no pref, nothing a page reads, never throws, so
  an official build (`browser.dom.window.dump.enabled` false) shows them.
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
- `netwerk/protocol/http/TRRServiceChannel.cpp`: the proxy lookup is made for the load info's context and fails closed; the connection info carries the context.
- `netwerk/protocol/http/nsHttpChannel.cpp`: the same fail-closed rule for a DoH request that runs as an nsHttpChannel (two hooks).
- `toolkit/components/parentalcontrols/nsParentalControlsService{Win,Cocoa,Android,Default}.{cpp,mm}`: the declared value before the first OS call.
- `netwerk/base/NetworkConnectivityService.cpp` (12 lines added, 14 of Mozilla's rewritten: the TRR-disabling flag of each of the four
  lookups, whose arguments are re-wrapped, and the `SetTRRMode` call), `toolkit/components/captivedetect/CaptiveDetect.sys.mjs`,
  `browser/components/tabbrowser/Tabbrowser.sys.mjs`: the system-resolver request is removed and a hook comment stands in its place.
- `netwerk/protocol/http/HttpBaseChannel.cpp` (owned by network/proxy-udp, tagged `STEALTH-HOOK(network/doh)`): the OCSP block of `GetTRRMode`.
- Hooks of this feature in files another folder owns (a file has one owner): `netwerk/base/TRRLoadInfo.cpp`
  (automation/navigation), `netwerk/ipc/ProxyConfigLookup*.{h,cpp}`, `PSocketProcess.ipdl`, `SocketProcessParent.{h,cpp}`,
  `netwerk/ipc/moz.build` (network/proxy-udp: the lookup carries the origin attributes next to the load info),
  `netwerk/test/gtest/moz.build` (network/socks). Their lines are tagged `STEALTH-HOOK(network/doh)`. The Juggler files of the pending
  filter (NetworkObserver.js, TargetRegistry.js, Protocol.js, BrowserHandler.js, Juggler.js, SessionRoute.js) belong to automation/juggler.

## Our own files

- `netwerk/ipc/StealthTRRContext.{h,cpp}`, `toolkit/components/parentalcontrols/StealthParentalControls.h`,
  `netwerk/test/gtest/TestStealthTRRContext.cpp`, `juggler/EarlyProxyHold.js`, `juggler/TrrPrefs.js`, and the node tests above.
