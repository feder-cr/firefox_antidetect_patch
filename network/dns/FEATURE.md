# network/dns

Category: spoof
Prefs: zoom.stealth.dns.no_local_resolution
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `netwerk/dns/DNSServiceBase.cpp` (DNSServiceBase::DNSForbiddenByActiveProxy, movable): Adds an OR to the existing proxy guard: when the launcher declares a proxy, DNS for non-IP hostnames is refused. Pref name confirmed at StaticPrefList.yaml:21529 as zoom.stealth.dns.no_local_resolution (accessor zoom_stealth_dns_no_local_resolution). Of the 9 added logic lines, 7 are the old condition re-indented. The comment names the ICE resolver as a caller that bypasses channels; unverified. C
