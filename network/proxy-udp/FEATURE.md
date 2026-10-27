# network/proxy-udp

Category: other, spoof
Prefs: network.proxy.socks, network.proxy.socks_password, network.proxy.socks_port, network.proxy.socks_remote_udp, network.proxy.socks_username, network.proxy.socks_version, network.proxy.type, zoom.stealth.webrtc.no_direct_udp
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/media/webrtc/transport/nr_socket_prsock.cpp` (new static StealthUscitaUdpDirettaVietata (before NrSocket::sendto); hooks in NrSocket::sendto and NrUdpSocketIpc::sendto, movable): About 36 lines of address-range decision logic sit inside the Mozilla file, plus two 3-4 line hooks. The function can move to nr_stealth_bridge.cpp behind one call. Pref zoom.stealth.webrtc.no_direct_udp defined at StaticPrefList.yaml:21539. IPv4 0.0.0.0 passes (if (a == 0) return false), but IPv6 unspecified '::' is refused: all-zero bytes leave loopback=true, b[15]!=1, so the function returns tr
- `netwerk/base/nsUDPSocket.cpp` (nsUDPSocket::InitWithAddress (block before the FUZZING check), movable): With SOCKS5 and network.proxy.socks_remote_udp set, pushes the SOCKS UDP layer (netwerk/socket/nsSOCKSUDPIOLayer.h:50, implemented at nsSOCKSUDPIOLayer.cpp:351) onto the new fd so QUIC/HTTP3 traffic goes through the proxy. network.proxy.socks_remote_udp is not defined in modules/libpref/init (read by string with default false). RISK CONFIRMED (from diff): if the layer attach fails the code logs 'f
