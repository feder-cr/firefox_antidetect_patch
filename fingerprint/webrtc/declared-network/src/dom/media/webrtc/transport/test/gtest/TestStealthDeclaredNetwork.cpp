/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH-HOOK(fingerprint/webrtc/declared-network)
//
// The network a page sees through WebRTC is declared, never read from the host:
//   R1  the interface list (nr_stun_get_addrs) is the declaration of STEALTHFOX_WEBRTC_NETWORK,
//       "v4=<private IPv4>;v6=<0|1>;p6=<the exit's /64 prefix, empty without IPv6>";
//   R2  the default address of a family is the declared address of that family, with no route probe;
//   R3  a host UDP socket takes the next port of its connection (consecutive from a random base in 49152-65535), and
//       a datagram between two sockets of this browser at the declared address travels on the loopback while each
//       end sees the other's declared address;
//   .local: the parent's mDNS responder joins and speaks on the loopback.
// Red on the old code: none of this exists, and nr_stun_get_addrs returns the host's adapters.

#include "gtest/gtest.h"

#include <stdlib.h>
#include <string.h>

// nICEr is C++: its headers are not wrapped in extern "C".
// clang-format off
#include <csi_platform.h>
#include "nr_api.h"
#include "transport_addr.h"
#include "local_addr.h"
#include "stun.h"
#include "addrs.h"
// clang-format on
#include "nr_stealth_network.h"

namespace {

const uint8_t kIid[8] = {0x1c, 0x5a, 0x37, 0x90, 0xe2, 0x4b, 0x08, 0xd1};

int Parse(const char* aDecl, nr_local_addr* aAddrs, int* aCount) {
  memset(aAddrs, 0, 4 * sizeof(nr_local_addr));
  return nr_stealth_parse_network(aDecl, kIid, aAddrs, 4, aCount);
}

std::string Ip(const nr_transport_addr& aAddr) {
  char ip[64];
  EXPECT_EQ(0, nr_transport_addr_get_addrstring(&aAddr, ip, sizeof(ip)));
  return ip;
}

nr_transport_addr Addr(const char* aIp, uint16_t aPort) {
  nr_transport_addr a;
  EXPECT_EQ(0, nr_str_port_to_transport_addr(aIp, nullptr, aPort, IPPROTO_UDP, &a));
  return a;
}

}  // namespace

TEST(StealthDeclaredNetwork, OneIPv4AdapterWithoutIPv6)
{
  nr_local_addr addrs[4];
  int count = -1;
  ASSERT_EQ(0, Parse("v4=192.168.1.23;v6=0;p6=", addrs, &count));
  ASSERT_EQ(1, count);
  EXPECT_EQ(NR_IPV4, addrs[0].addr.ip_version);
  EXPECT_EQ("192.168.1.23", Ip(addrs[0].addr));
  EXPECT_EQ(IPPROTO_UDP, addrs[0].addr.protocol);
  EXPECT_STRNE("", addrs[0].addr.ifname);
}

TEST(StealthDeclaredNetwork, IPv6IsTheExitPrefixAndATemporaryIdentifierOnTheSameAdapter)
{
  nr_local_addr addrs[4];
  int count = -1;
  ASSERT_EQ(0, Parse("v4=10.0.0.7;v6=1;p6=2a01:e0a:1f2:3450::/64", addrs, &count));
  ASSERT_EQ(2, count);
  EXPECT_EQ(NR_IPV4, addrs[0].addr.ip_version);
  EXPECT_EQ(NR_IPV6, addrs[1].addr.ip_version);
  EXPECT_EQ("2a01:e0a:1f2:3450:1c5a:3790:e24b:8d1", Ip(addrs[1].addr));
  EXPECT_STREQ(addrs[0].addr.ifname, addrs[1].addr.ifname);
  EXPECT_TRUE(addrs[1].flags & NR_ADDR_FLAG_TEMPORARY);
  // It survives nICEr's own filters, as a real temporary address does.
  int filtered = count;
  ASSERT_EQ(0, nr_stun_filter_addrs(addrs, 1, 1, &filtered));
  EXPECT_EQ(2, filtered);
}

TEST(StealthDeclaredNetwork, IPv6WithoutAPrefixIsNoIPv6)
{
  nr_local_addr addrs[4];
  int count = -1;
  ASSERT_EQ(0, Parse("v4=192.168.0.14;v6=1;p6=", addrs, &count));
  EXPECT_EQ(1, count);
  ASSERT_EQ(0, Parse("v4=192.168.0.14;v6=0;p6=2a01:e0a:1f2:3450::/64", addrs, &count));
  EXPECT_EQ(1, count);
}

TEST(StealthDeclaredNetwork, OnlyAPrivateIPv4OfALanIsAnAdapter)
{
  nr_local_addr addrs[4];
  const char* good[] = {"v4=10.20.30.40;v6=0;p6=", "v4=172.16.5.9;v6=0;p6=", "v4=172.31.255.7;v6=0;p6=",
                        "v4=192.168.178.21;v6=0;p6="};
  for (const char* decl : good) {
    int count = -1;
    EXPECT_EQ(0, Parse(decl, addrs, &count)) << decl;
    EXPECT_EQ(1, count) << decl;
  }
  const char* bad[] = {"v4=8.8.8.8;v6=0;p6=",       "v4=100.64.1.2;v6=0;p6=",  "v4=127.0.0.1;v6=0;p6=",
                       "v4=169.254.3.4;v6=0;p6=",    "v4=224.0.0.251;v6=0;p6=", "v4=0.1.2.3;v6=0;p6=",
                       "v4=172.32.0.5;v6=0;p6=",     "v4=192.168.1.0;v6=0;p6=", "v4=192.168.1.255;v6=0;p6=",
                       "v4=::1;v6=0;p6="};
  for (const char* decl : bad) {
    int count = -1;
    EXPECT_NE(0, Parse(decl, addrs, &count)) << decl;
    EXPECT_EQ(0, count) << decl;
  }
}

TEST(StealthDeclaredNetwork, APrefixMustBeGlobalUnicast)
{
  nr_local_addr addrs[4];
  const char* bad[] = {"v4=192.168.1.2;v6=1;p6=fe80::/64",       "v4=192.168.1.2;v6=1;p6=2001:db8:1::/64",
                       "v4=192.168.1.2;v6=1;p6=2001:0:53aa::/64", "v4=192.168.1.2;v6=1;p6=2002:c000:204::/64",
                       "v4=192.168.1.2;v6=1;p6=fd12:3456::/64",   "v4=192.168.1.2;v6=1;p6=2a01:e0a::/48",
                       "v4=192.168.1.2;v6=1;p6=10.0.0.1"};
  for (const char* decl : bad) {
    int count = -1;
    EXPECT_NE(0, Parse(decl, addrs, &count)) << decl;
    EXPECT_EQ(0, count) << decl;
  }
}

TEST(StealthDeclaredNetwork, AMalformedDeclarationGivesNoAddress)
{
  nr_local_addr addrs[4];
  const char* bad[] = {nullptr,
                       "",
                       "v4=192.168.1.2;v6=0",
                       "v4=192.168.1.2",
                       "v6=0;p6=",
                       "v4=192.168.1.2;v6=2;p6=",
                       "v4=192.168.1.2;v6=0;p6=;x=1",
                       "v4=192.168.1.2;v4=192.168.1.3;v6=0"};
  for (const char* decl : bad) {
    int count = -1;
    EXPECT_NE(0, Parse(decl, addrs, &count)) << (decl ? decl : "(null)");
    EXPECT_EQ(0, count) << (decl ? decl : "(null)");
  }
}

TEST(StealthDeclaredNetwork, TheInterfaceListIsTheDeclarationOfTheEnvironment)
{
  // Read where the interfaces are enumerated, each time: the gtest sets it, the old code read the host's adapters.
  nr_local_addr addrs[16];
  int count = -1;
#ifdef XP_WIN
  _putenv_s("STEALTHFOX_WEBRTC_NETWORK", "v4=192.168.44.9;v6=0;p6=");
#else
  setenv("STEALTHFOX_WEBRTC_NETWORK", "v4=192.168.44.9;v6=0;p6=", 1);
#endif
  ASSERT_EQ(0, nr_stun_get_addrs(addrs, 16, &count));
  ASSERT_EQ(1, count);
  EXPECT_EQ("192.168.44.9", Ip(addrs[0].addr));
#ifdef XP_WIN
  _putenv_s("STEALTHFOX_WEBRTC_NETWORK", "");
#else
  unsetenv("STEALTHFOX_WEBRTC_NETWORK");
#endif
  EXPECT_NE(0, nr_stun_get_addrs(addrs, 16, &count));
  EXPECT_EQ(0, count);
}

TEST(StealthDeclaredNetwork, TheDefaultAddressOfAFamilyIsItsDeclaredAddress)
{
  nr_local_addr addrs[4];
  int count = 0;
  ASSERT_EQ(0, Parse("v4=192.168.1.23;v6=1;p6=2a01:e0a:1f2:3450::/64", addrs, &count));
  nr_local_addr out;
  memset(&out, 0, sizeof(out));
  ASSERT_EQ(0, nr_stealth_default_local_address(NR_IPV4, addrs, count, &out));
  EXPECT_EQ(0, nr_transport_addr_cmp(&out.addr, &addrs[0].addr, NR_TRANSPORT_ADDR_CMP_MODE_ADDR));
  EXPECT_STREQ(addrs[0].addr.ifname, out.addr.ifname);
  ASSERT_EQ(0, nr_stealth_default_local_address(NR_IPV6, addrs, count, &out));
  EXPECT_EQ(0, nr_transport_addr_cmp(&out.addr, &addrs[1].addr, NR_TRANSPORT_ADDR_CMP_MODE_ADDR));
}

TEST(StealthDeclaredNetwork, AFamilyTheMachineDoesNotHaveHasNoDefaultAddress)
{
  nr_local_addr addrs[4];
  int count = 0;
  ASSERT_EQ(0, Parse("v4=192.168.1.23;v6=0;p6=", addrs, &count));
  nr_local_addr out;
  EXPECT_EQ(R_NOT_FOUND, nr_stealth_default_local_address(NR_IPV6, addrs, count, &out));
  EXPECT_EQ(R_NOT_FOUND, nr_stealth_default_local_address(NR_IPV4, addrs, 0, &out));
}

TEST(StealthDeclaredNetwork, TheResponderJoinsAndSpeaksOnTheLoopback)
{
  // The declared address is not the host's: a join on it fails and the responder never hears its own query.
  EXPECT_STREQ("127.0.0.1;", nr_stealth_mdns_interfaces());
}

TEST(StealthDeclaredNetwork, AConnectionsHostPortsAreConsecutiveFromABaseInTheDynamicRange)
{
  for (uint64_t r : {0ull, 1ull, 16382ull, 16383ull, 0x123456789abcdefull, ~0ull}) {
    mozilla::StealthHostPorts ports(r);
    uint32_t first = ports.Next();
    uint32_t second = ports.Next();
    EXPECT_GE(first, 49152u);
    EXPECT_LE(first, 65535u);
    EXPECT_EQ(first == 65535 ? 49152u : first + 1, second) << r;
  }
  // Two connections draw two bases: nothing ties one connection's ports to another's.
  EXPECT_NE(mozilla::StealthHostPorts(7).Next(), mozilla::StealthHostPorts(9001).Next());
}

TEST(StealthDeclaredNetwork, ADatagramToOurOwnSocketGoesOnTheLoopback)
{
  mozilla::StealthOwnPorts own;
  own.Add(NR_IPV4, 51001);
  own.Add(NR_IPV4, 51003);
  nr_transport_addr self = Addr("192.168.1.23", 51001);
  // To the other connection of this browser, at our declared address: the kernel gets the loopback, same port.
  nr_transport_addr to = Addr("192.168.1.23", 51003);
  EXPECT_EQ(1, nr_stealth_own_peer_to_kernel(&self, own, &to));
  EXPECT_EQ("127.0.0.1", Ip(to));
  uint16_t port = 0;
  EXPECT_EQ(0, nr_transport_addr_get_port(&to, &port));
  EXPECT_EQ(51003, port);
  // Back from it: ICE sees our declared address, so the pair host/host is the one the candidates announced.
  nr_transport_addr from = Addr("127.0.0.1", 51003);
  EXPECT_EQ(1, nr_stealth_own_peer_from_kernel(&self, own, &from));
  EXPECT_EQ("192.168.1.23", Ip(from));
}

TEST(StealthDeclaredNetwork, NothingElseIsRewritten)
{
  mozilla::StealthOwnPorts own;
  own.Add(NR_IPV4, 51001);
  nr_transport_addr self = Addr("192.168.1.23", 51001);
  // A page's STUN server on localhost keeps its address, both ways.
  nr_transport_addr stun = Addr("127.0.0.1", 3478);
  EXPECT_EQ(0, nr_stealth_own_peer_from_kernel(&self, own, &stun));
  EXPECT_EQ("127.0.0.1", Ip(stun));
  // Our declared address on a port that is not one of ours, and a real peer, go as they are.
  nr_transport_addr other = Addr("192.168.1.23", 40000);
  EXPECT_EQ(0, nr_stealth_own_peer_to_kernel(&self, own, &other));
  nr_transport_addr peer = Addr("203.0.113.9", 51001);
  EXPECT_EQ(0, nr_stealth_own_peer_to_kernel(&self, own, &peer));
  EXPECT_EQ("203.0.113.9", Ip(peer));
  // A socket that is not declared (the wildcard) rewrites nothing.
  nr_transport_addr wildcard = Addr("0.0.0.0", 51001);
  nr_transport_addr back = Addr("127.0.0.1", 51001);
  EXPECT_EQ(0, nr_stealth_own_peer_from_kernel(&wildcard, own, &back));
}
