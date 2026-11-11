/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// With IPv6 declared off, a literal IPv6 destination fails the way connect()
// fails on a Windows machine with the IPv6 stack and no IPv6 route. The table
// is the predicate's: which NSPR error (0 = connect as usual) each address gets.
// The suite is StealthDeclaredNetwork, which the candidate's gtest filter runs.

#include "../../base/StealthIPv6Literal.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/net/DNS.h"
#include "nsString.h"
#include "prerr.h"
#include "prio.h"
#include "prnetdb.h"

using namespace mozilla::net;

namespace {

struct Row {
  const char* text;
  PRErrorCode ifDisabled;
};

// What connect() does on Windows for each class, IPv6 declared off:
//  - a global, unique-local or documentation address has no route:
//    WSAENETUNREACH, PR_NETWORK_UNREACHABLE_ERROR;
//  - the unspecified address and a multicast address are WSAEADDRNOTAVAIL,
//    PR_ADDRESS_NOT_AVAILABLE_ERROR;
//  - ::1 connects, and a link-local address is left to the OS (it needs an
//    interface; the answer is the OS's own).
const Row kRows[] = {
    {"2001:db8::1", PR_NETWORK_UNREACHABLE_ERROR},
    {"2606:4700:4700::1111", PR_NETWORK_UNREACHABLE_ERROR},
    {"2a00:1450:4002::68", PR_NETWORK_UNREACHABLE_ERROR},
    {"fc00::1", PR_NETWORK_UNREACHABLE_ERROR},
    {"fd12:3456:789a::1", PR_NETWORK_UNREACHABLE_ERROR},
    {"fec0::1", PR_NETWORK_UNREACHABLE_ERROR},
    {"64:ff9b::808:808", PR_NETWORK_UNREACHABLE_ERROR},
    {"::1", 0},
    {"fe80::1", 0},
    {"febf::1", 0},
    {"ff02::1", PR_ADDRESS_NOT_AVAILABLE_ERROR},
    {"ff05::1:3", PR_ADDRESS_NOT_AVAILABLE_ERROR},
    {"::", PR_ADDRESS_NOT_AVAILABLE_ERROR},
    // IPv4-mapped: IPv4's, left to the socket layer.
    {"::ffff:1.2.3.4", 0},
    {"::ffff:127.0.0.1", 0},
    // IPv4 is not this predicate's.
    {"1.2.3.4", 0},
    {"127.0.0.1", 0},
    {"0.0.0.0", 0},
    {"192.168.1.1", 0},
};

NetAddr Parse(const char* aText) {
  NetAddr addr;
  EXPECT_TRUE(NS_SUCCEEDED(addr.InitFromString(nsDependentCString(aText), 443)))
      << aText;
  return addr;
}

// The declaration the engine derives, restored as it was.
struct DisableIPv6Pref {
  static constexpr const char* kName = "network.dns.disableIPv6";
  bool mHadUser = mozilla::Preferences::HasUserValue(kName);
  bool mUser = mozilla::Preferences::GetBool(kName, false,
                                             mozilla::PrefValueKind::User);
  ~DisableIPv6Pref() {
    if (mHadUser) {
      mozilla::Preferences::SetBool(kName, mUser);
    } else {
      mozilla::Preferences::ClearUser(kName);
    }
  }
};

}  // namespace

TEST(StealthDeclaredNetwork, AnIPv6LiteralFailsLikeAHostWithNoIPv6Route)
{
  for (const Row& row : kRows) {
    EXPECT_EQ(StealthIPv6ConnectError(Parse(row.text), true), row.ifDisabled)
        << row.text;
  }
}

TEST(StealthDeclaredNetwork, NothingIsBlockedWhileIPv6IsDeclared)
{
  for (const Row& row : kRows) {
    EXPECT_EQ(StealthIPv6ConnectError(Parse(row.text), false), 0) << row.text;
  }
}

TEST(StealthDeclaredNetwork, AnAddressThatIsNotIPv6OrIPIsLeftAlone)
{
  NetAddr empty;
  EXPECT_EQ(StealthIPv6ConnectError(empty, true), 0);
  NetAddr local;
  local.raw.family = AF_LOCAL;
  EXPECT_EQ(StealthIPv6ConnectError(local, true), 0);
}

TEST(StealthDeclaredNetwork, TheConnectFailsWithTheRealErrorAndSendsNothing)
{
  DisableIPv6Pref restore;
  ASSERT_TRUE(NS_SUCCEEDED(
      mozilla::Preferences::SetBool("network.dns.disableIPv6", true)));
  NetAddr addr = Parse("2001:db8::1");
  PRNetAddr prAddr;
  NetAddrToPRNetAddr(&addr, &prAddr);
  PRFileDesc* fd = PR_OpenTCPSocket(PR_AF_INET6);
  ASSERT_TRUE(fd);
  PR_SetError(0, 0);
  // Refused before the system is asked: the socket is never connected, so no
  // SYN leaves for the documentation prefix. (The exemption of a destination
  // behind a proxy would be a real connect(): the source test guards it.)
  EXPECT_EQ(StealthIPv6Connect(fd, addr, false, &prAddr, PR_INTERVAL_NO_WAIT),
            PR_FAILURE);
  EXPECT_EQ(PR_GetError(), PR_NETWORK_UNREACHABLE_ERROR);
  PR_Close(fd);
}
