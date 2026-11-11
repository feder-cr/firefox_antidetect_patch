# network/proxy-udp

Category: other, spoof
Prefs: network.proxy.socks_remote_udp, zoom.stealth.webrtc.no_direct_udp, zoom.stealth.udp_proxy.armed
Test: StealthSessionUdpProxy.* (netwerk/test/gtest/TestStealthSocksCredentials.cpp); juggler/tests/session_udp_proxy_reaches_the_engine.test.mjs

## What it does

Behind a SOCKS5 proxy the UDP of WebRTC (STUN, TURN) and of HTTP/3 leaves through the proxy's UDP relay, or does not leave,
and never goes out from the machine's own address. The proxy is the session's: the Juggler (SessionRoute) publishes the
SOCKS5 proxy of the session to the engine through `nsIStealthSessionUdpProxy` (juggler/udp), which keeps it, user and
password included, in one place in the parent and sends it to the socket process by an IPC message
(`PSocketProcess::StealthSessionUdpProxy`, sent again whenever the socket process is launched). The credentials never
travel by pref and are never in a content process or a log; the content processes only see the pref
`zoom.stealth.udp_proxy.armed`.

The session arms the UDP road only when the core proved the proxy's UDP coherent (UDP ASSOCIATE granted, a STUN answered
through the relay, the UDP exit equal to the TCP exit) and wrote `network.proxy.socks_remote_udp`. Armed, the SOCKS UDP layer
(netwerk/socket/nsSOCKSUDPIOLayer) is pushed on the UDP sockets of nsUDPSocket and on the ICE sockets of the socket process
(NrSocket::create); a socket whose layer cannot be attached is not created. Not armed (or no proxy): a direct UDP send to a
public address is refused (`zoom.stealth.webrtc.no_direct_udp`, always true behind a proxy), as before. A send passes only if
its socket has the layer.

## Hooks in Mozilla files

- `dom/media/webrtc/transport/nr_socket_prsock.cpp`: four one-line hooks into nr_stealth_proxy_udp.h (the include, the layer
  on NrSocket::create, the filter in NrSocket::sendto and NrUdpSocketIpc::sendto).
- `netwerk/base/nsUDPSocket.cpp`: include and one call that attaches the layer of the session's proxy.
- `netwerk/base/nsIOService.cpp`: include and one call that sends the session's proxy again to a socket process that was
  (re)launched.
- `netwerk/ipc/PSocketProcess.ipdl`, `netwerk/ipc/SocketProcessChild.h`: the IPC message and its receiver.
- `netwerk/base/moz.build`: the hook of the folder network/socks (credentials), one line.

## Our own files

- `netwerk/socket/nsSOCKSUDPIOLayer.{h,cpp}` (the layer, the session's proxy and its IPC), `juggler/udp/*` (the service the
  Juggler calls).
