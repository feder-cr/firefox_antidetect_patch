# network/proxy-udp

Category: other, spoof
Prefs: none
Test: StealthSessionUdpProxy.* (netwerk/test/gtest/TestStealthSocksCredentials.cpp), StealthUdpProxyBirth.* (dom/media/webrtc/transport/test/gtest/TestStealthUdpProxyBirth.cpp, the rule at the birth of a UDP socket); the node test the_udp_of_a_socks5_proxy_is_a_flag_of_its_proxy_info; the e2e of invisible_playwright (tests/test_*_e2e.py: srflx through the relay, proxy without UDP, TURN over TCP per context, two contexts, HTTP/3)

## What it does

Behind a SOCKS5 proxy the UDP of WebRTC (STUN, TURN) and of HTTP/3 leaves through the proxy's UDP relay, or does not leave,
and never goes out from the machine's own address. A browser with no proxy is a plain Firefox: nothing here changes it.

Only a SOCKS5 proxy the session measured coherent carries UDP: the core says it in the proxy it declares (`udp`: 'coherent', 'no' or
'unmeasured'), and the Juggler turns it into one flag of the nsIProxyInfo it builds, `nsIProxyInfo::CARRIES_COHERENT_UDP` (1 << 2;
`SessionRoute.udpFlagFor`: SOCKS5 and 'coherent' only). The flag is part of the proxy info's IPC clone, so it reaches the socket
process with the proxy. A "socks" proxy without it, SOCKS4 and HTTP are a router that drops: UDP goes nowhere, never direct, and
HTTP/3 falls back to h2 by itself.

The proxy is the one of the document, found by the same lookup Firefox uses for its TCP (ProxyConfigLookup, with the load
info of the page, so the filters of the Juggler answer for the context of the page). `PcUdpProxy` (netwerk/socket/StealthPcUdpProxy)
keeps the answer for one PeerConnection: no proxy (direct), SOCKS5, or a proxy that carries no UDP (blocked). A lookup that
fails, or a document with no URI, is blocked: it never opens a road around the proxy.

The TCP sockets of a PeerConnection follow the same rule. `WebrtcTCPSocket::OnProxyAvailable` opened the socket plain when the
lookup failed or gave a proxy of a type it does not know; `StealthTcpDirectRefused` (StealthIceResolverContext.cpp) closes it
instead: only the answer "no proxy" opens a plain socket. And a PeerConnection always has a proxy config: Mozilla's
`GetProxyConfig` returns none for a window that was torn down (a detached iframe, a closed popup), and a PeerConnection with no
config has plain sockets and a resolver in the default context. `StealthProxyConfigOf` keeps Mozilla's config when there is one
and otherwise builds it from the principal of the window; when that cannot be done the proxy is forced.

The UDP is decided where a socket is born. `NrSocket::create` (ICE sockets) and `nsUDPSocket::InitWithAddress` (HTTP/3,
`setSocksProxy`) push the SOCKS UDP layer (netwerk/socket/nsSOCKSUDPIOLayer) when the proxy is SOCKS5. The layer returns at
once: the association (resolve, connect, greeting, auth, UDP ASSOCIATE) is made afterwards on the socket thread, the first
datagrams are held (16 at most) and sent when it is ready, so a candidate is not late. A proxy without UDP is a router that
drops: what is sent goes nowhere and nothing is received. Datagrams to loopback go direct in every state. In a content
process the UDP sockets of a PeerConnection are refused: they would go out direct.

The SOCKS5 connections of Necko tell the layer's pool the proxy (nsSOCKSIOLayer), so an association is warm when the first
socket asks for it. The sockets of the PeerConnections are made in the socket process, which opens no SOCKS5 connection of Necko's, so the
parent also tells it: one message, `PSocketProcess::StealthWarmSocksUdp` (host, port, user, pass), at most once per proxy and per socket
process and only for a proxy measured coherent, whose receiver (`SocketProcessChild::RecvStealthWarmSocksUdp`) calls the same
`nsSOCKSUDPIOLayerWarm`. Endpoint and credentials travel in this message only. TURN over TCP and the HTTP tunnel build their lookup channel with the same function
(`StealthNewChannelOfLoadInfo`, netwerk/ipc/StealthDocumentChannel).

HTTP/3 through SOCKS5: the h3 Alt-Svc is kept, the host name is resolved (QUIC needs the address) and the socket carries the
proxy; through a proxy that carries no UDP the handshake never completes and Firefox falls back to h2 by itself. neqo's own I/O
writes on the OS handle and would never reach the layer, so a SOCKS5 connection uses Firefox's own NSPR path
(`Http3Session::Init`, `mUseNSPRForIO`) and `nsUDPSocket::GetFileDescriptor` gives -1 for a socket that has the layer: nothing can
write on its handle by another road. An IPv6 destination goes through an IPv4 relay (the family that counts is the socket's; RFC 1928
allows a destination of any family), and a plain socket asked whether the layer failed answers no. An association that cannot
start (no socket transport service) is not a refusal of the proxy: it does not tell the pool, which would take the lock the caller
holds. The proxy of a PeerConnection is the context's, not the page address's: the lookup asks about a public https address with the
document's load info, so a page on the loopback, a `file:`, a `data:` or a document with no address follows the context's proxy.

## Hooks in Mozilla files

- `dom/media/webrtc/transport/nr_socket_prsock.{h,cpp}`: the NrSocket takes the proxy config of its PeerConnection; one hook
  in `create` (nr_stealth_proxy_udp.h) and the refusal in a content process. The file also carries the hooks of
  fingerprint/webrtc/declared-network (bind, own ports; tagged with that feature).
- `dom/media/webrtc/transport/nr_socket_proxy_config.{h,cpp}`: `UdpProxy()`, the lookup started with the config.
- `dom/media/webrtc/transport/ipc/WebrtcTCPSocket.cpp`: the lookup channel and the tunnel channel come from
  StealthNewChannelOfLoadInfo (Mozilla's own channel-building code is gone from here), and one call of `StealthTcpDirectRefused`
  at the end of `OnProxyAvailable`.
- `dom/media/webrtc/jsapi/PeerConnectionImpl.cpp`: one include and one call of `StealthProxyConfigOf`, beside Mozilla's
  `GetProxyConfig()` line.
- `netwerk/ipc/ProxyConfigLookup*.{h,cpp}`, `PSocketProcess.ipdl`, `SocketProcessParent.{h,cpp}`: the optional load info of the
  document goes with the lookup. The same files carry the hooks of network/doh (the origin attributes of the DoH context go with
  the load info; tagged with that feature).
- `netwerk/base/nsIUDPSocket.idl`, `nsUDPSocket.{h,cpp}`, `nsProxyInfo.{h,cpp}`, `nsIProxyInfo.idl`: `setSocksProxy`, the SOCKS5 test, the flag CARRIES_COHERENT_UDP (a value of ours in a slot Mozilla leaves free), and no OS handle for a socket with the layer.
- `netwerk/protocol/http/{AlternateServices,DnsAndConnectSocket,HttpBaseChannel,HttpConnectionUDP}.cpp`: the HTTP/3 gates.
- `netwerk/protocol/http/Http3Session.cpp`: `mUseNSPRForIO` also for a SOCKS5 connection (3 lines).
- `netwerk/protocol/http/Http3ConnectUDPStream.cpp`: `setSocksProxy` answers not implemented (the CONNECT-UDP stream has its own proxy).
- `netwerk/socket/nsSOCKSIOLayer.cpp`: one call that warms the pool (tagged `STEALTH-HOOK(network/proxy-udp)`).
- `netwerk/ipc/SocketProcessChild.{h,cpp}`: the receiver of `StealthWarmSocksUdp`, one call.
- `moz.build` of netwerk/ipc, netwerk/socket (and netwerk/protocol/http, for another folder's file): our sources.

## Our own files

- `netwerk/socket/nsSOCKSUDPIOLayer.{h,cpp}` (the layer and the pool), `netwerk/socket/StealthPcUdpProxy.{h,cpp}` (the proxy of a
  PeerConnection), `netwerk/ipc/StealthDocumentChannel.{h,cpp}` (the channel of a document's load info),
  `dom/media/webrtc/transport/nr_stealth_proxy_udp.h` (the rule at the birth of a socket).
