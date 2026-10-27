# network/socks-udp-h3

Category: build, other
Prefs: network.proxy.socks_remote_udp
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `netwerk/protocol/http/AlternateServices.cpp` (AltSvcCache::UpdateAltServiceMapping, movable): Hunk 2 has 6 logic lines. Move the predicate (direct, or h3 proxy, or socks_remote_udp and SOCKS) into one function in our own file and keep a one-line hook. Not a fingerprint change in itself, but the h3 path is visible to pages (nextHopProtocol); reclassify as spoof if the team wants that. The pref is read on every call.
- `netwerk/protocol/http/DnsAndConnectSocket.cpp` (DnsAndConnectSocket::DnsAndConnectSocket; DnsAndConnectSocket::CheckProxyConfig, movable): Two logic hunks of 7 lines each (constructor and CheckProxyConfig). Both move to our own file behind one-line hooks. Privacy: with socks_remote_udp on and an h3 request through SOCKS, the hostname is now resolved on the client (skipDns false), so the local resolver sees the name; the old code let the proxy resolve it. Confirm this matches the intended DNS policy. The removed comment block explaini
- `netwerk/protocol/http/HttpBaseChannel.cpp` (HttpBaseChannel::Http3Allowed, movable): Same predicate as AlternateServices.cpp: use one shared function from our file so the two stay identical. Http3Allowed is const; the pref is read on every call.
- `netwerk/socket/moz.build` (EXPORTS and UNIFIED_SOURCES lists, already-hook): Registers our SOCKS UDP IO layer (both files present in C:\ff\source\netwerk\socket). Base layer for the socks-udp-h3 feature in AlternateServices, DnsAndConnectSocket and HttpBaseChannel.

## Our own files

- `netwerk/socket/nsSOCKSUDPIOLayer.cpp`
- `netwerk/socket/nsSOCKSUDPIOLayer.h`
