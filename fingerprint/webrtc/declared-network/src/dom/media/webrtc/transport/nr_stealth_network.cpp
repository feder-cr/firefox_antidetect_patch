/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* STEALTH-HOOK(fingerprint/webrtc/declared-network): see nr_stealth_network.h. */

#include "nr_stealth_network.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mozilla/RandomNum.h"

// nICEr is C++: its headers are not wrapped in extern "C".
// clang-format off
#include <csi_platform.h>
#include "nr_api.h"
#include "transport_addr.h"
#include "local_addr.h"
#include "stun_util.h"
// clang-format on

namespace {

constexpr int kMaxDeclared = 2;

/* What the declared adapter is called inside nICEr: a key for the interface prioritizer and for the neighbour lookup
 * of the other family (nr_ice_get_assoc_interface_address), never readable by a page. */
constexpr char kIfname[] = "declared0";

/* A wired gigabit adapter: with one adapter, type and speed change no order a page can read
 * (nrinterfaceprioritizer.cpp), so they are the same for everyone. */
constexpr int kIfType = NR_INTERFACE_TYPE_WIRED;
constexpr int kIfSpeedKbps = 1000000;

int Fill(const char* aIp, int aFlags, nr_local_addr* aOut) {
  memset(aOut, 0, sizeof(*aOut));
  if (nr_str_port_to_transport_addr(aIp, nullptr, 0, IPPROTO_UDP, &aOut->addr)) {
    return R_BAD_DATA;
  }
  (void)strlcpy(aOut->addr.ifname, kIfname, sizeof(aOut->addr.ifname));
  aOut->interface.type = kIfType;
  aOut->interface.estimated_speed = kIfSpeedKbps;
  aOut->flags = aFlags;
  return 0;
}

/* A private IPv4 address of a home or office LAN: 10/8, 172.16/12, 192.168/16, and not the network's own address nor
 * its broadcast. Public, CGNAT (100.64/10), loopback, link-local, multicast and 0.x addresses are refused. */
bool IsPlausiblePrivateV4(const nr_transport_addr& aAddr) {
  if (aAddr.ip_version != NR_IPV4) {
    return false;
  }
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&aAddr.u.addr4.sin_addr.s_addr);
  bool rfc1918 = b[0] == 10 || (b[0] == 172 && (b[1] & 0xf0) == 16) || (b[0] == 192 && b[1] == 168);
  return rfc1918 && b[3] != 0 && b[3] != 255;
}

/* The /64 prefix of a global unicast IPv6 address (2000::/3), not Teredo (2001::/32), 6to4 (2002::/16) nor
 * documentation (2001:db8::/32): the exit's prefix the core measured, written "<address>/64" or "<address>". */
bool PrefixOf(const char* aP6, uint8_t aOut[8]) {
  char buf[64];
  size_t len = strcspn(aP6, "/");
  if (len == 0 || len >= sizeof(buf)) {
    return false;
  }
  memcpy(buf, aP6, len);
  buf[len] = '\0';
  if (aP6[len] && strcmp(aP6 + len, "/64") != 0) {
    return false;
  }
  nr_transport_addr a;
  if (nr_str_port_to_transport_addr(buf, nullptr, 0, IPPROTO_UDP, &a) || a.ip_version != NR_IPV6) {
    return false;
  }
  const uint8_t* s = a.u.addr6.sin6_addr.s6_addr;
  if ((s[0] & 0xe0) != 0x20) return false;                                         // not global unicast
  if (s[0] == 0x20 && s[1] == 0x01 && s[2] == 0x00 && s[3] == 0x00) return false;  // Teredo
  if (s[0] == 0x20 && s[1] == 0x02) return false;                                  // 6to4
  if (s[0] == 0x20 && s[1] == 0x01 && s[2] == 0x0d && s[3] == 0xb8) return false;  // documentation
  memcpy(aOut, s, 8);
  return true;
}

int Fail(const char* aDecl, const char* aWhy, int* aCount) {
  *aCount = 0;
  r_log(NR_LOG_STUN, LOG_ERR,
        "Stealth: no declared network (%s): STEALTHFOX_WEBRTC_NETWORK %s; no interface, no host candidate", aWhy,
        aDecl ? "does not parse" : "is not set");
  return R_NOT_FOUND;
}

/* The value of field `aKey` of a "k=v;k=v;k=v" declaration into `aOut` (it may be empty); false unless the field is
 * there exactly once. */
bool Field(const char* aDecl, const char* aKey, char* aOut, size_t aLen) {
  size_t klen = strlen(aKey);
  int found = 0;
  for (const char* p = aDecl;;) {
    const char* end = strchr(p, ';');
    size_t flen = end ? static_cast<size_t>(end - p) : strlen(p);
    if (flen > klen && !strncmp(p, aKey, klen) && p[klen] == '=') {
      size_t vlen = flen - klen - 1;
      if (vlen >= aLen) return false;
      memcpy(aOut, p + klen + 1, vlen);
      aOut[vlen] = '\0';
      ++found;
    }
    if (!end) break;
    p = end + 1;
  }
  return found == 1;
}

/* This process's temporary interface identifier (RFC 4941, what Windows uses): random, universal/local bit clear, never
 * an EUI-64 one (no ff:fe in the middle, which nICEr filters as MAC-based). Drawn once: the adapter keeps its address
 * while the browser runs. */
const uint8_t* ProcessIid() {
  static const struct Iid {
    uint8_t b[8];
    Iid() {
      uint64_t r = mozilla::RandomUint64OrDie();
      memcpy(b, &r, 8);
      b[0] &= ~0x02;
      if (b[3] == 0xff && b[4] == 0xfe) b[4] = 0xfd;
    }
  } sIid;
  return sIid.b;
}

}  // namespace

extern "C" int nr_stealth_parse_network(const char* aDecl, const uint8_t aIid[8], nr_local_addr* aAddrs,
                                        int aMaxAddrs, int* aCount) {
  *aCount = 0;
  if (!aDecl || !*aDecl) {
    return Fail(nullptr, "missing", aCount);
  }
  int fields = 1;
  for (const char* p = aDecl; *p; ++p) fields += *p == ';';
  char v4[64], v6[8], p6[64];
  if (fields != 3 || !Field(aDecl, "v4", v4, sizeof(v4)) || !Field(aDecl, "v6", v6, sizeof(v6)) ||
      !Field(aDecl, "p6", p6, sizeof(p6)) || (strcmp(v6, "0") && strcmp(v6, "1"))) {
    return Fail(aDecl, "malformed", aCount);
  }
  nr_local_addr entries[kMaxDeclared];
  if (Fill(v4, 0, &entries[0]) || !IsPlausiblePrivateV4(entries[0].addr)) {
    return Fail(aDecl, "not a private IPv4 address of a LAN", aCount);
  }
  int n = 1;
  if (!strcmp(v6, "1") && *p6) {
    uint8_t prefix[8];
    if (!PrefixOf(p6, prefix)) {
      return Fail(aDecl, "p6 is not the /64 prefix of a global IPv6 address", aCount);
    }
    char ip6[64];
    snprintf(ip6, sizeof(ip6), "%x:%x:%x:%x:%x:%x:%x:%x", (prefix[0] << 8) | prefix[1], (prefix[2] << 8) | prefix[3],
             (prefix[4] << 8) | prefix[5], (prefix[6] << 8) | prefix[7], (aIid[0] << 8) | aIid[1],
             (aIid[2] << 8) | aIid[3], (aIid[4] << 8) | aIid[5], (aIid[6] << 8) | aIid[7]);
    if (Fill(ip6, NR_ADDR_FLAG_TEMPORARY, &entries[1])) {
      return Fail(aDecl, "IPv6 address", aCount);
    }
    n = 2;
  }
  if (aMaxAddrs < n) {
    return Fail(aDecl, "no room", aCount);
  }
  for (int i = 0; i < n; ++i) {
    if (nr_local_addr_copy(&aAddrs[i], &entries[i])) {
      return Fail(aDecl, "copy", aCount);
    }
  }
  *aCount = n;
  return 0;
}

extern "C" int nr_stealth_declared_addrs(nr_local_addr* aAddrs, int aMaxAddrs, int* aCount) {
  return nr_stealth_parse_network(getenv("STEALTHFOX_WEBRTC_NETWORK"), ProcessIid(), aAddrs, aMaxAddrs, aCount);
}

extern "C" int nr_stealth_default_local_address(int aIpVersion, nr_local_addr* aAddrs, int aAddrCt,
                                                nr_local_addr* aOut) {
  for (int i = 0; i < aAddrCt; ++i) {
    if (aAddrs[i].addr.ip_version == aIpVersion) {
      return nr_local_addr_copy(aOut, &aAddrs[i]);
    }
  }
  return R_NOT_FOUND;
}

extern "C" const char* nr_stealth_mdns_interfaces(void) { return "127.0.0.1;"; }

extern "C" int nr_stealth_addr_is_wildcard_ip(const nr_transport_addr* aAddr) {
  static const unsigned char kZero6[16] = {0};
  switch (aAddr->ip_version) {
    case NR_IPV4:
      return aAddr->u.addr4.sin_addr.s_addr == INADDR_ANY;
    case NR_IPV6:
      return !memcmp(aAddr->u.addr6.sin6_addr.s6_addr, kZero6, sizeof(kZero6));
    default:
      return 0;
  }
}

namespace {

/* `aAddr` with the address `aIp` and its own port and protocol. */
int WithAddress(const char* aIp, nr_transport_addr* aAddr) {
  uint16_t port;
  nr_transport_addr out;
  if (nr_transport_addr_get_port(aAddr, &port) ||
      nr_str_port_to_transport_addr(aIp, nullptr, port, aAddr->protocol, &out)) {
    return 0;
  }
  return !nr_transport_addr_copy(aAddr, &out);
}

bool IsOwnPeer(const nr_transport_addr* aSelf, const mozilla::StealthOwnPorts& aOwn,
               const nr_transport_addr* aAddr) {
  uint16_t port;
  return aSelf->ip_version == aAddr->ip_version && !nr_stealth_addr_is_wildcard_ip(aSelf) &&
         !nr_transport_addr_is_loopback(aSelf) && !nr_transport_addr_get_port(aAddr, &port) &&
         aOwn.Has(aAddr->ip_version, port);
}

}  // namespace

int nr_stealth_own_peer_to_kernel(const nr_transport_addr* aSelf, const mozilla::StealthOwnPorts& aOwn,
                                  nr_transport_addr* aAddr) {
  if (!IsOwnPeer(aSelf, aOwn, aAddr) ||
      nr_transport_addr_cmp(aSelf, aAddr, NR_TRANSPORT_ADDR_CMP_MODE_ADDR)) {
    return 0;
  }
  return WithAddress(aAddr->ip_version == NR_IPV4 ? "127.0.0.1" : "::1", aAddr);
}

int nr_stealth_own_peer_from_kernel(const nr_transport_addr* aSelf, const mozilla::StealthOwnPorts& aOwn,
                                    nr_transport_addr* aAddr) {
  if (!IsOwnPeer(aSelf, aOwn, aAddr) || !nr_transport_addr_is_loopback(aAddr)) {
    return 0;
  }
  char self[64];
  if (nr_transport_addr_get_addrstring(aSelf, self, sizeof(self))) {
    return 0;
  }
  return WithAddress(self, aAddr);
}
