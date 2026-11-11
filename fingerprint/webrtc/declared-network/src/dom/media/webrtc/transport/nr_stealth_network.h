/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* STEALTH-HOOK(fingerprint/webrtc/declared-network)
 *
 * The network WebRTC gathers on is declared by the core, never read from the host.
 *
 * The declaration is the environment variable STEALTHFOX_WEBRTC_NETWORK, read where the interfaces are enumerated
 * (the parent: StunAddrsRequestParent::RecvGetStunAddrs -> nr_stun_get_addrs):
 *
 *     v4=<private IPv4>;v6=<0|1>;p6=<the /64 prefix of the exit, empty without IPv6>
 *
 * One adapter, as the modal Windows PC has: one private IPv4 address (RFC 1918), and when the machine has IPv6 the
 * temporary global IPv6 address of that adapter: the exit's /64 prefix and a temporary interface identifier drawn at
 * random (RFC 4941, as Windows does). v6=1 without a prefix is no IPv6, never a fallback. Under the default (no camera
 * or microphone permission) a page sees only the number of host names and their ranks; the addresses are
 * <uuid>.local names. Nothing of the host's adapters is read: the OS enumerators are not built.
 *
 * Without a declaration, or with one that does not parse, there is no interface: no host candidate, and an error in
 * the log. Never a fallback to the host.
 *
 * IPv6 is one fact for the whole browser. The same declaration decides network.dns.disableIPv6 in the parent
 * (StealthApplyDeclaredIPFamily, called once from nsIOService::Init): no IPv6 interface declared (v6=0, or v6=1 without
 * a prefix) means no AAAA lookup in any process, so a page is never loaded over an IPv6 address while ICE says there is
 * none. The answer is the parser's own (nr_stealth_declared_has_ipv6), never a second reading of the string.
 *
 * The declared address is the identity of this machine (R3, nr_socket_prsock.cpp): a host socket is bound to the
 * wildcard of its family on a port of the connection and reports the declared address with that port, and a datagram
 * between two of this browser's sockets travels on the kernel's loopback while each end sees the other's declared
 * address, as two sockets of one machine see its LAN address.
 */

#ifndef nr_stealth_network_h__
#define nr_stealth_network_h__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nr_local_addr_;
struct nr_transport_addr_;

/* Parses a declaration into at most `maxaddrs` entries; `iid` is the temporary interface identifier of the IPv6
 * address (8 bytes). 0 and *count = 1 or 2 on success; an error and *count = 0 for a missing or malformed one. */
int nr_stealth_parse_network(const char* decl, const uint8_t iid[8], struct nr_local_addr_* addrs, int maxaddrs,
                             int* count);

/* The IPv6 fact of a declaration, from the parser's own answer: 0 and *has = 1 when nr_stealth_parse_network accepts
 * `decl` and returns an IPv6 interface, *has = 0 when it accepts it and returns none; an error and *has = 0 for a
 * missing or malformed one. Quiet: it writes no log (the interface list is the one that reports a bad declaration). */
int nr_stealth_declared_has_ipv6(const char* decl, int* has);

/* R1: the interface list, from STEALTHFOX_WEBRTC_NETWORK, with this process's temporary identifier. */
int nr_stealth_declared_addrs(struct nr_local_addr_* addrs, int maxaddrs, int* count);

/* R2: the default address of `ip_version` is the first declared entry of that family among `addrs`; R_NOT_FOUND when
 * the declaration has none (the machine has no route in that family). No socket. */
int nr_stealth_default_local_address(int ip_version, struct nr_local_addr_* addrs, int addr_ct,
                                     struct nr_local_addr_* out);

/* The family of the default address to look for, given the family of the page's server: the target's family when the
 * list has an address of it, else the other one (a declared machine without IPv6 reaching a page over IPv6 asks for the
 * family it has); the target's family when the list has neither. */
int nr_stealth_default_family(int target_ip_version, struct nr_local_addr_* addrs, int addr_ct);

/* The interfaces the parent's mDNS responder joins the group on and speaks through: the loopback, "127.0.0.1;"
 * (lib.rs takes a ';'-separated list). The declared address is not the host's: a join on it fails. */
const char* nr_stealth_mdns_interfaces(void);

/* 1 when the ADDRESS of `addr` is the wildcard (0.0.0.0 or ::), whatever the port; 0 otherwise.
 * (nr_transport_addr_is_wildcard also calls port 0 a wildcard.) */
int nr_stealth_addr_is_wildcard_ip(const struct nr_transport_addr_* addr);

#ifdef __cplusplus
}  // extern "C"

#  include <set>

#  include "prnetdb.h"

namespace mozilla {

/* Parent process, once, before any lookup: sets network.dns.disableIPv6 from the IPv6 fact of `aDecl` (the value of
 * STEALTHFOX_WEBRTC_NETWORK). As a DEFAULT value (never written to the profile) and the user value is cleared, so the
 * declaration is the only source. Returns true when it applied; false, and nothing touched, when there is no
 * declaration or it does not parse (a build run by hand keeps Firefox's behaviour). */
bool StealthApplyDeclaredIPFamily(const char* aDecl);

/* The same, for the declaration of this process's environment (STEALTHFOX_WEBRTC_NETWORK). */
bool StealthApplyDeclaredIPFamilyFromEnvironment();

/* R3: the host UDP ports of ONE connection (one ICE context), as Windows hands them out: consecutive in the order the
 * sockets are made (the IPv4 host N, the IPv6 host N+1), from a base drawn at random in the dynamic range 49152-65535,
 * anew for each connection. Not the kernel's ports and not derived from the seed: two visits of one seed must not
 * share them (retail: captures/retail/firefox-159.0-win/two-pc-port-sequence of invisible_WebRTC, each connection its
 * own pair, unrelated to the last). A busy port is skipped, as the kernel skips it; after 65535 it wraps to 49152. */
class StealthHostPorts final {
 public:
  static constexpr uint32_t kFirst = 49152;
  static constexpr uint32_t kLast = 65535;

  /* `aRandom`: a random number from the caller's generator (mozilla::RandomUint64). */
  explicit StealthHostPorts(uint64_t aRandom)
      : mNext(kFirst + static_cast<uint32_t>(aRandom % (kLast - kFirst + 1))) {}

  uint16_t Next() {
    uint32_t port = mNext;
    mNext = port == kLast ? kFirst : port + 1;
    return static_cast<uint16_t>(port);
  }

  /* The first port from here on for which `aTry(port)` is true (the bind worked, or any other reason to keep it), or 0 when
   * none of the 16384 is. The kernel hands out only ports it can bind: Windows keeps ranges of the dynamic range
   * (Hyper-V, WSL, Docker: about one port in ten on a developer PC) for the system, and a bind to one fails with
   * "access denied"; a port another socket holds fails as "in use". Either way the next port is tried, as the kernel
   * would have given another. */
  template <typename F>
  uint16_t NextAvailable(F aTry) {
    for (uint32_t n = 0; n <= kLast - kFirst; ++n) {
      uint16_t port = Next();
      if (aTry(port)) return port;
    }
    return 0;
  }

 private:
  uint32_t mNext;
};

/* R3: the ports this process's declared host sockets are bound to, per family. A datagram TO our declared address and
 * one of these ports goes to the loopback of the family (the declared address is not the host's; the kernel would
 * route it away); one FROM the loopback and one of these ports is reported as coming from our declared address.
 * Nothing else is touched: a server on localhost (a page's STUN server on 127.0.0.1) keeps its address. */
class StealthOwnPorts final {
 public:
  void Add(int aIpVersion, uint16_t aPort) { mPorts.insert(Key(aIpVersion, aPort)); }
  void Remove(int aIpVersion, uint16_t aPort) { mPorts.erase(Key(aIpVersion, aPort)); }
  bool Has(int aIpVersion, uint16_t aPort) const { return mPorts.count(Key(aIpVersion, aPort)) != 0; }

 private:
  static uint32_t Key(int aIpVersion, uint16_t aPort) {
    return (static_cast<uint32_t>(aIpVersion) << 16) | aPort;
  }
  std::set<uint32_t> mPorts;
};

/* R3 for the TCP socket that talks to a TURN or STUN server (WebrtcTCPSocket): the connection's declared address is not an
 * address of this host, so it can only give the FAMILY, never the bind address. The socket binds the wildcard of that
 * family on port 0, so the system hands out a free port and a range it keeps for itself (Windows reserves some of the
 * dynamic range for Hyper-V, WSL, Docker, and refuses a bind to them) can never refuse it. The local port of such a
 * socket is nowhere a page can read: a relay candidate carries the server's mapped address, not the local one, and the TCP
 * host candidate that is announced is the active one, which declares port 9. False when `aDeclaredAddress` does not
 * parse as an address. `aNicerPort` is the port nICEr proposed, only logged (WebrtcTCPSocket log module). */
bool StealthTcpBindAddress(const char* aDeclaredAddress, uint16_t aNicerPort, PRNetAddr* aOut);

}  // namespace mozilla

/* For a socket whose identity is `self` (the declared address and its bound port): the peer of a datagram as the
 * kernel needs it (to) and as ICE must see it (from). Each returns 1 when it rewrote `addr`, 0 when it left it. */
int nr_stealth_own_peer_to_kernel(const struct nr_transport_addr_* self, const mozilla::StealthOwnPorts& own,
                                  struct nr_transport_addr_* addr);
int nr_stealth_own_peer_from_kernel(const struct nr_transport_addr_* self, const mozilla::StealthOwnPorts& own,
                                    struct nr_transport_addr_* addr);

#endif  // __cplusplus

#endif  // nr_stealth_network_h__
