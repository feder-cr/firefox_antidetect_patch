# network/proxy-udp

Category: other, spoof
Prefs: none
Test: StealthSessionUdpProxy.* (netwerk/test/gtest/TestStealthSocksCredentials.cpp); the e2e of invisible_playwright (tests/test_*_e2e.py: srflx through the relay, proxy without UDP, TURN over TCP per context, two contexts, HTTP/3)

## What it does

Behind a SOCKS5 proxy the UDP of WebRTC (STUN, TURN) and of HTTP/3 leaves through the proxy's UDP relay, or does not leave,
and never goes out from the machine's own address. A browser with no proxy is a plain Firefox: nothing here changes it.

The proxy is the one of the document, found by the same lookup Firefox uses for its TCP (ProxyConfigLookup, with the load
info of the page, so the filters of the Juggler answer for the context of the page). `PcUdpProxy` (netwerk/socket/StealthPcUdpProxy)
keeps the answer for one PeerConnection: no proxy (direct), SOCKS5, or a proxy that carries no UDP (blocked). A lookup that
fails, or a document with no URI, is blocked: it never opens a road around the proxy.

The UDP is decided where a socket is born. `NrSocket::create` (ICE sockets) and `nsUDPSocket::InitWithAddress` (HTTP/3,
`setSocksProxy`) push the SOCKS UDP layer (netwerk/socket/nsSOCKSUDPIOLayer) when the proxy is SOCKS5. The layer returns at
once: the association (resolve, connect, greeting, auth, UDP ASSOCIATE) is made afterwards on the socket thread, the first
datagrams are held (16 at most) and sent when it is ready, so a candidate is not late. A proxy without UDP is a router that
drops: what is sent goes nowhere and nothing is received. Datagrams to loopback go direct in every state. In a content
process the UDP sockets of a PeerConnection are refused: they would go out direct.

The SOCKS5 connections of Necko tell the layer's pool the proxy (nsSOCKSIOLayer), so an association is warm when the first
socket asks for it. TURN over TCP and the HTTP tunnel build their lookup channel with the same function
(`StealthNewChannelOfLoadInfo`, netwerk/ipc/StealthDocumentChannel).

HTTP/3 through SOCKS5: the h3 Alt-Svc is kept, the host name is resolved (QUIC needs the address) and the socket carries the
proxy; through a proxy that carries no UDP the handshake never completes and Firefox falls back to h2 by itself.

## Hooks in Mozilla files

- `dom/media/webrtc/transport/nr_socket_prsock.{h,cpp}`: the NrSocket takes the proxy config of its PeerConnection; one hook
  in `create` (nr_stealth_proxy_udp.h) and the refusal in a content process.
- `dom/media/webrtc/transport/nr_socket_proxy_config.{h,cpp}`: `UdpProxy()`, the lookup started with the config.
- `dom/media/webrtc/transport/ipc/WebrtcTCPSocket.cpp`: the lookup channel and the tunnel channel come from
  StealthNewChannelOfLoadInfo (Mozilla's own channel-building code is gone from here).
- `netwerk/ipc/ProxyConfigLookup*.{h,cpp}`, `PSocketProcess.ipdl`, `SocketProcessParent.{h,cpp}`: the optional load info of the
  document goes with the lookup.
- `netwerk/base/nsIUDPSocket.idl`, `nsUDPSocket.{h,cpp}`, `nsProxyInfo.{h,cpp}`: `setSocksProxy` and the SOCKS5 test.
- `netwerk/protocol/http/{AlternateServices,DnsAndConnectSocket,HttpBaseChannel,HttpConnectionUDP}.cpp`: the HTTP/3 gates.
- `netwerk/socket/nsSOCKSIOLayer.cpp`: one call that warms the pool.
- `moz.build` of netwerk/ipc, netwerk/socket (and netwerk/protocol/http, for another folder's file): our sources.

## Our own files

- `netwerk/socket/nsSOCKSUDPIOLayer.{h,cpp}` (the layer and the pool), `netwerk/socket/StealthPcUdpProxy.{h,cpp}` (the proxy of a
  PeerConnection), `netwerk/ipc/StealthDocumentChannel.{h,cpp}` (the channel of a document's load info),
  `dom/media/webrtc/transport/nr_stealth_proxy_udp.h` (the rule at the birth of a socket).
