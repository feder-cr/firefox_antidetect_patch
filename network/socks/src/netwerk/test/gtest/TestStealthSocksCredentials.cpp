/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <atomic>
#include <cstring>
#include <thread>

#include "../../base/StealthSocksCredentials.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsICancelable.h"
#include "nsIChannel.h"
#include "nsIContentPolicy.h"
#include "nsILoadInfo.h"
#include "nsIPrefBranch.h"
#include "nsIProtocolProxyCallback.h"
#include "nsIProtocolProxyService.h"
#include "nsIProxyInfo.h"
#include "nsNetUtil.h"
#include "nsSOCKSUDPIOLayer.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "prio.h"
#include "prnetdb.h"
#include "private/pprio.h"

#ifdef XP_WIN
#  include <winsock2.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#endif

namespace mozilla::net {

namespace {

constexpr char kUser[] = "network.proxy.socks_username";
constexpr char kPass[] = "network.proxy.socks_password";

// Puts every proxy pref the tests touch back to its default.
class ProxyPrefsReset {
 public:
  ~ProxyPrefsReset() {
    for (const char* pref :
         {"network.proxy.type", "network.proxy.socks",
          "network.proxy.socks_port", "network.proxy.socks_version",
          "network.proxy.http", "network.proxy.http_port", kUser, kPass}) {
      Preferences::ClearUser(pref);
    }
  }
};

class CapturingCallback final : public nsIProtocolProxyCallback {
 public:
  NS_DECL_ISUPPORTS
  NS_IMETHOD OnProxyAvailable(nsICancelable*, nsIChannel*, nsIProxyInfo* aInfo,
                              nsresult aStatus) override {
    mDone = true;
    mStatus = aStatus;
    mInfo = aInfo;
    return NS_OK;
  }
  bool mDone = false;
  nsresult mStatus = NS_OK;
  nsCOMPtr<nsIProxyInfo> mInfo;

 private:
  ~CapturingCallback() = default;
};
NS_IMPL_ISUPPORTS(CapturingCallback, nsIProtocolProxyCallback)

}  // namespace

// The helper alone: guards, whitespace, which member a pref name reaches.
TEST(TestStealthSocksCredentials, PrefsChanged)
{
  ProxyPrefsReset reset;
  nsCOMPtr<nsIPrefBranch> branch = do_GetService(NS_PREFSERVICE_CONTRACTID);
  ASSERT_TRUE(branch);
  ASSERT_EQ(Preferences::SetCString(kUser, " us er\t"), NS_OK);
  ASSERT_EQ(Preferences::SetCString(kPass, "p a\nss "), NS_OK);

  nsCString user("keep-user"), pass("keep-pass");

  // A pref that is neither of ours: nothing is read.
  StealthSocksPrefsChanged(branch, "network.proxy.socks", user, pass);
  EXPECT_TRUE(user.EqualsLiteral("keep-user"));
  EXPECT_TRUE(pass.EqualsLiteral("keep-pass"));

  // Only the username changed: only the username is read, whitespace removed.
  StealthSocksPrefsChanged(branch, kUser, user, pass);
  EXPECT_TRUE(user.EqualsLiteral("user"));
  EXPECT_TRUE(pass.EqualsLiteral("keep-pass"));

  // Only the password changed.
  user.AssignLiteral("keep-user");
  StealthSocksPrefsChanged(branch, kPass, user, pass);
  EXPECT_TRUE(user.EqualsLiteral("keep-user"));
  EXPECT_TRUE(pass.EqualsLiteral("pass"));

  // nullptr (startup): both are read.
  user.AssignLiteral("x");
  pass.AssignLiteral("y");
  StealthSocksPrefsChanged(branch, nullptr, user, pass);
  EXPECT_TRUE(user.EqualsLiteral("user"));
  EXPECT_TRUE(pass.EqualsLiteral("pass"));

  // Back to the default (empty string): the members are emptied.
  Preferences::ClearUser(kUser);
  Preferences::ClearUser(kPass);
  StealthSocksPrefsChanged(branch, nullptr, user, pass);
  EXPECT_TRUE(user.IsEmpty());
  EXPECT_TRUE(pass.IsEmpty());
}

// End to end through the public service: the pref observer feeds the members
// and the resolved proxy info carries them.
TEST(TestStealthSocksCredentials, ReachResolvedProxyInfo)
{
  ProxyPrefsReset reset;
  nsCOMPtr<nsIProtocolProxyService> ps =
      do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
  ASSERT_TRUE(ps);

  auto Resolve = [&](const char* aSpec) -> nsCOMPtr<nsIProxyInfo> {
    nsCOMPtr<nsIURI> uri;
    EXPECT_EQ(NS_NewURI(getter_AddRefs(uri), aSpec), NS_OK);
    nsCOMPtr<nsIChannel> channel;
    EXPECT_EQ(
        NS_NewChannel(getter_AddRefs(channel), uri,
                      nsContentUtils::GetSystemPrincipal(),
                      nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
                      nsIContentPolicy::TYPE_OTHER),
        NS_OK);
    RefPtr<CapturingCallback> cb = new CapturingCallback();
    nsCOMPtr<nsICancelable> cancel;
    EXPECT_EQ(ps->AsyncResolve(channel, 0, cb, nullptr, getter_AddRefs(cancel)),
              NS_OK);
    EXPECT_TRUE(SpinEventLoopUntil("TestStealthSocksCredentials"_ns,
                                   [&] { return cb->mDone; }));
    return cb->mInfo;
  };

  ASSERT_EQ(Preferences::SetInt("network.proxy.type", 1), NS_OK);
  ASSERT_EQ(Preferences::SetCString("network.proxy.socks", "127.0.0.1"), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.socks_port", 1080), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.socks_version", 5), NS_OK);
  ASSERT_EQ(Preferences::SetCString(kUser, "alice"), NS_OK);
  ASSERT_EQ(Preferences::SetCString(kPass, " s3 cret "), NS_OK);

  nsCOMPtr<nsIProxyInfo> info = Resolve("http://example.com/");
  ASSERT_TRUE(info);
  nsAutoCString type, user, pass;
  info->GetType(type);
  info->GetUsername(user);
  info->GetPassword(pass);
  EXPECT_TRUE(type.EqualsLiteral("socks"));
  EXPECT_TRUE(user.EqualsLiteral("alice"));
  EXPECT_TRUE(pass.EqualsLiteral("s3cret"));

  // The single NewProxyInfo_Internal call in Resolve_Internal serves the HTTP
  // branch too: with no SOCKS proxy the stored credentials ride on the http
  // proxy info.
  ASSERT_EQ(Preferences::SetCString("network.proxy.socks", ""), NS_OK);
  ASSERT_EQ(Preferences::SetCString("network.proxy.http", "127.0.0.1"), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.http_port", 8080), NS_OK);
  info = Resolve("http://example.com/");
  ASSERT_TRUE(info);
  info->GetType(type);
  info->GetUsername(user);
  EXPECT_TRUE(type.EqualsLiteral("http"));
  EXPECT_TRUE(user.EqualsLiteral("alice"));
}

// ---------------------------------------------------------------------------
// The SOCKS5 UDP layer (network/proxy-udp) takes the proxy of the SESSION and
// the core's verdict network.proxy.socks_remote_udp, not network.proxy.*, which
// a Juggler session never writes. A fake SOCKS5 server on the loopback stands
// for the proxy. They live in this file because it is already listed in
// netwerk/test/gtest/moz.build as ours: a second file would be a second line
// in a Mozilla build file.

namespace {

constexpr char kVerdict[] = "network.proxy.socks_remote_udp";
constexpr char kArmed[] = "zoom.stealth.udp_proxy.armed";

// The proxy of the session as the parent or the socket process hold it, and the
// core's verdict. Put back whatever a test did.
class UdpSession {
 public:
  ~UdpSession() {
    StealthSessionUdpProxySet(""_ns, 0, ""_ns, ""_ns);
    Preferences::ClearUser(kVerdict);
    Preferences::ClearUser(kArmed);
  }
  void Arm(const char* aHost, int32_t aPort, const char* aUser = "",
           const char* aPass = "") {
    StealthSessionUdpProxySet(nsDependentCString(aHost), aPort,
                              nsDependentCString(aUser),
                              nsDependentCString(aPass));
    Preferences::SetBool(kVerdict, true);
  }
};

bool NativeSocketIsOpen(PROsfd aFd) {
#ifdef XP_WIN
  int type = 0;
  int len = sizeof(type);
  return getsockopt(static_cast<SOCKET>(aFd), SOL_SOCKET, SO_TYPE,
                    reinterpret_cast<char*>(&type), &len) == 0;
#else
  return fcntl(static_cast<int>(aFd), F_GETFD) != -1 || errno != EBADF;
#endif
}

PRNetAddr Loopback(uint16_t aPort) {
  PRNetAddr addr;
  PR_InitializeNetAddr(PR_IpAddrLoopback, aPort, &addr);
  return addr;
}

// A SOCKS5 server for one client: greets with no authentication, answers UDP
// ASSOCIATE with a loopback UDP socket as the relay, and remembers the first
// datagram that reaches the relay.
class FakeSocks {
 public:
  FakeSocks() {
    mListener = PR_NewTCPSocket();
    mRelay = PR_NewUDPSocket();
    PRNetAddr addr = Loopback(0);
    PR_Bind(mListener, &addr);
    PR_Listen(mListener, 1);
    PR_GetSockName(mListener, &addr);
    mPort = PR_ntohs(addr.inet.port);
    PRNetAddr relay = Loopback(0);
    PR_Bind(mRelay, &relay);
    PR_GetSockName(mRelay, &relay);
    mRelayPort = relay.inet.port;
    mThread = std::thread([this] { Serve(); });
  }

  ~FakeSocks() {
    mStop = true;
    if (mThread.joinable()) {
      mThread.join();  // PR_Accept gives up on its own after a few seconds
    }
    PR_Close(mListener);
    PR_Close(mRelay);
  }

  uint16_t Port() const { return mPort; }
  bool AssociateSeen() const { return mAssociated; }

  // The first datagram the relay received, or the empty string.
  nsCString FirstDatagram() {
    char buf[256];
    PRNetAddr from;
    int32_t n = PR_RecvFrom(mRelay, buf, sizeof(buf), 0, &from,
                            PR_SecondsToInterval(3));
    return n > 0 ? nsCString(buf, n) : nsCString();
  }

 private:
  bool ReadAll(PRFileDesc* aFd, uint8_t* aBuf, size_t aLen) {
    size_t got = 0;
    while (got < aLen) {
      int32_t n = PR_Recv(aFd, aBuf + got, aLen - got, 0,
                          PR_SecondsToInterval(4));
      if (n <= 0) return false;
      got += n;
    }
    return true;
  }

  void Serve() {
    PRNetAddr peer;
    PRFileDesc* client =
        PR_Accept(mListener, &peer, PR_SecondsToInterval(5));
    if (!client) return;
    uint8_t greeting[3];
    const uint8_t noAuth[2] = {0x05, 0x00};
    uint8_t associate[10];
    if (ReadAll(client, greeting, sizeof(greeting)) &&
        PR_Send(client, noAuth, 2, 0, PR_INTERVAL_NO_TIMEOUT) == 2 &&
        ReadAll(client, associate, sizeof(associate))) {
      uint8_t reply[10] = {0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0, 0};
      std::memcpy(reply + 8, &mRelayPort, 2);  // already network order
      mAssociated = associate[1] == 0x03;
      PR_Send(client, reply, sizeof(reply), 0, PR_INTERVAL_NO_TIMEOUT);
      // The control connection stays open for as long as the test needs.
      while (!mStop) {
        PR_Sleep(PR_MillisecondsToInterval(20));
      }
    }
    PR_Close(client);
  }

  PRFileDesc* mListener = nullptr;
  PRFileDesc* mRelay = nullptr;
  uint16_t mPort = 0;
  uint16_t mRelayPort = 0;
  std::thread mThread;
  std::atomic<bool> mStop{false};
  std::atomic<bool> mAssociated{false};
};

}  // namespace

TEST(StealthSessionUdpProxy, ArmedNeedsTheVerdictAndTheProxyOfTheSession)
{
  UdpSession prefs;
  nsAutoCString host, user, pass;
  int32_t port = 0;

  // The proxy alone is not the verdict.
  StealthSessionUdpProxySet("127.0.0.1"_ns, 1080, ""_ns, ""_ns);
  EXPECT_FALSE(StealthSocksUdpRouted());

  // The verdict alone is not a proxy, and neither is network.proxy.*, nor the
  // bool a content process reads: this process holds none.
  StealthSessionUdpProxySet(""_ns, 0, ""_ns, ""_ns);
  Preferences::SetBool(kVerdict, true);
  Preferences::SetCString("network.proxy.socks", "10.0.0.1");
  Preferences::SetInt("network.proxy.socks_port", 1080);
  Preferences::SetBool(kArmed, true);
  EXPECT_FALSE(StealthSocksUdpRouted());
  Preferences::ClearUser("network.proxy.socks");
  Preferences::ClearUser("network.proxy.socks_port");

  // A bad endpoint clears what was held.
  prefs.Arm("203.0.113.7", 1080, "alice", "s3cret");
  ASSERT_TRUE(StealthSocksUdpRouted());
  StealthSessionUdpProxySet("203.0.113.7"_ns, 0, ""_ns, ""_ns);
  EXPECT_FALSE(StealthSocksUdpRouted());

  prefs.Arm("203.0.113.7", 1080, "alice", "s3cret");
  EXPECT_TRUE(StealthSocksUdpRouted());
  ASSERT_TRUE(StealthSessionUdpProxy(host, port, user, pass));
  EXPECT_TRUE(host.EqualsLiteral("203.0.113.7"));
  EXPECT_EQ(port, 1080);
  EXPECT_TRUE(user.EqualsLiteral("alice"));
  EXPECT_TRUE(pass.EqualsLiteral("s3cret"));

  Preferences::SetBool(kVerdict, false);
  EXPECT_FALSE(StealthSocksUdpRouted());
}

TEST(StealthSessionUdpProxy, ASessionThatIsNotArmedLeavesTheSocketPlain)
{
  UdpSession prefs;
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  EXPECT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  EXPECT_FALSE(nsSOCKSUDPIOLayerIsOn(fd));
  PR_Close(fd);
}

TEST(StealthSessionUdpProxy, TheLayerConnectsToTheProxyOfTheSession)
{
  UdpSession prefs;
  FakeSocks proxy;
  prefs.Arm("127.0.0.1", proxy.Port());

  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  EXPECT_TRUE(nsSOCKSUDPIOLayerIsOn(fd));
  EXPECT_TRUE(proxy.AssociateSeen());

  // A datagram to the STUN server goes to the relay, wrapped for it.
  PRNetAddr stun;
  PR_InitializeNetAddr(PR_IpAddrNull, 19302, &stun);
  stun.inet.ip = PR_htonl(0xC6336405);  // 198.51.100.5 (TEST-NET-2)
  const char ping[] = "ping";
  EXPECT_EQ(4, PR_SendTo(fd, ping, 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  nsCString got = proxy.FirstDatagram();
  ASSERT_EQ(got.Length(), 10u + 4u);
  EXPECT_EQ(0, std::memcmp(got.get(), "\x00\x00\x00\x01\x4A\x7D\xFA\x81\x4B\x66", 10));
  EXPECT_EQ(0, std::memcmp(got.get() + 10, "ping", 4));
  PR_Close(fd);
}

TEST(StealthSessionUdpProxy, ClosingASocketWithTheLayerClosesTheSocketUnderIt)
{
  UdpSession prefs;
  FakeSocks proxy;
  prefs.Arm("127.0.0.1", proxy.Port());

  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  PROsfd native = PR_FileDesc2NativeHandle(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  ASSERT_TRUE(nsSOCKSUDPIOLayerIsOn(fd));
  ASSERT_TRUE(NativeSocketIsOpen(native));
  ASSERT_EQ(PR_SUCCESS, PR_Close(fd));
  EXPECT_FALSE(NativeSocketIsOpen(native))
      << "the layer freed itself and left the UDP socket open";
}

TEST(StealthSessionUdpProxy, AnArmedSessionWhoseProxyRefusesFailsTheSocket)
{
  UdpSession prefs;
  // A port nobody listens on: bind one, learn its number, close it.
  PRFileDesc* probe = PR_NewTCPSocket();
  PRNetAddr addr = Loopback(0);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(probe, &addr));
  PR_GetSockName(probe, &addr);
  uint16_t dead = PR_ntohs(addr.inet.port);
  PR_Close(probe);

  prefs.Arm("127.0.0.1", dead);
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  EXPECT_TRUE(NS_FAILED(nsSOCKSUDPIOLayerAttachSession(fd)))
      << "an armed session must not go on with a plain, direct socket";
  EXPECT_FALSE(nsSOCKSUDPIOLayerIsOn(fd));
  PR_Close(fd);
}

TEST(StealthSessionUdpProxy, TheFailureOfOneProxyDoesNotHoldBackAnother)
{
  UdpSession prefs;
  PRFileDesc* probe = PR_NewTCPSocket();
  PRNetAddr addr = Loopback(0);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(probe, &addr));
  PR_GetSockName(probe, &addr);
  uint16_t dead = PR_ntohs(addr.inet.port);
  PR_Close(probe);

  prefs.Arm("127.0.0.1", dead);
  PRFileDesc* first = PR_NewUDPSocket();
  EXPECT_TRUE(NS_FAILED(nsSOCKSUDPIOLayerAttachSession(first)));
  PR_Close(first);

  // The Juggler swaps the proxy. The old failure was a flag for the whole
  // process, and the new proxy was refused without being asked.
  FakeSocks proxy;
  prefs.Arm("127.0.0.1", proxy.Port());
  PRFileDesc* second = PR_NewUDPSocket();
  EXPECT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(second));
  EXPECT_TRUE(nsSOCKSUDPIOLayerIsOn(second));
  PR_Close(second);
}
}  // namespace mozilla::net
