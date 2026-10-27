# network/socks

Category: other
Prefs: network.proxy.socks_password, network.proxy.socks_username
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `netwerk/base/nsProtocolProxyService.cpp` (nsProtocolProxyService::PrefsChanged; nsProtocolProxyService::Resolve_Internal (SOCKS NewProxyInfo_Internal call), movable): Loads SOCKS5 username and password from network.proxy.socks_username and socks_password (PROXY_PREF expands to network.proxy.<x>, nsProtocolProxyService.cpp:62-63) and passes them as the username and password arguments of NewProxyInfo_Internal (nsProtocolProxyService.h:216-219). RISK CONFIRMED (from code): that single call at nsProtocolProxyService.cpp:2329 serves every manual-config branch, inclu
- `netwerk/base/nsProtocolProxyService.h` (nsProtocolProxyService class members (after mSOCKSProxyVersion), already-hook): Two member declarations holding the SOCKS credentials read in nsProtocolProxyService.cpp. Kept in plain nsCString in memory.
