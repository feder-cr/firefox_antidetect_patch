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
#include "nsIIOService.h"
#include "nsIProtocolHandler.h"
#include "nsIProtocolProxyCallback.h"
#include "nsIProtocolProxyService.h"
#include "nsIProxyInfo.h"
#include "nsNetUtil.h"
#include "nsSOCKSUDPIOLayer.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "prerror.h"
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

// What the fake proxy does that a real one may: it takes a round trip (every answer on the control connection waits
// for it), wants a user name and a password (RFC 1929), answers UDP ASSOCIATE with the unspecified address, answers in
// pieces of one byte, or does not answer at all.
struct ProxyOptions {
  uint32_t mDelayMs = 0;
  bool mAuth = false;
  bool mBndUnspecified = false;
  bool mSplit = false;
  bool mSilent = false;
};

constexpr char kProxyUser[] = "user~tcp-test-65535";
constexpr char kProxyPass[] = "not-a-real-password";

// A SOCKS5 server for one client: greets, answers UDP ASSOCIATE with a loopback UDP socket as the relay, and remembers
// the datagrams that reach the relay.
class FakeSocks {
 public:
  explicit FakeSocks(uint32_t aDelayMs = 0) : FakeSocks(ProxyOptions{aDelayMs}) {}

  explicit FakeSocks(const ProxyOptions& aOptions) : mOptions(aOptions) {
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
      mThread.join();
    }
    PR_Close(mListener);
    PR_Close(mRelay);
  }

  uint16_t Port() const { return mPort; }
  bool AssociateSeen() const { return mAssociated; }
  // The client has connected to the proxy.
  bool Connected() const { return mConnected; }
  // The control connection is over: the client closed it, or never came.
  bool ControlClosed() const { return mClosed; }

  // The first datagram the relay received, or the empty string. Remembers who sent it.
  nsCString FirstDatagram() {
    char buf[256];
    int32_t n = PR_RecvFrom(mRelay, buf, sizeof(buf), 0, &mClient,
                            PR_SecondsToInterval(3));
    if (n > 0) mHasClient = true;
    return n > 0 ? nsCString(buf, n) : nsCString();
  }

  // How many datagrams reach the relay, until none comes within aQuietMs.
  uint32_t CountDatagrams(uint32_t aQuietMs) {
    char buf[256];
    PRNetAddr from;
    uint32_t count = 0;
    while (PR_RecvFrom(mRelay, buf, sizeof(buf), 0, &from,
                       PR_MillisecondsToInterval(aQuietMs)) > 0) {
      ++count;
    }
    return count;
  }

  // A datagram to the client as the relay sends it (the header names the server it came from), or from a socket that
  // is not the relay. Needs a datagram of the client received first.
  bool SendToClient(const char* aPayload, bool aFromTheRelay) {
    if (!mHasClient) return false;
    uint8_t frame[64] = {0, 0, 0, 1, 198, 51, 100, 5, 0x4B, 0x66};
    size_t len = std::strlen(aPayload);
    std::memcpy(frame + 10, aPayload, len);
    PRFileDesc* from = aFromTheRelay ? mRelay : PR_NewUDPSocket();
    PRInt32 sent =
        PR_SendTo(from, frame, 10 + len, 0, &mClient, PR_INTERVAL_NO_TIMEOUT);
    if (!aFromTheRelay) PR_Close(from);
    return sent > 0;
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

  void Answer(PRFileDesc* aClient, const uint8_t* aBytes, int32_t aLen) {
    if (mOptions.mDelayMs) {
      PR_Sleep(PR_MillisecondsToInterval(mOptions.mDelayMs));
    }
    if (mOptions.mSplit) {
      for (int32_t i = 0; i < aLen; ++i) {
        PR_Send(aClient, aBytes + i, 1, 0, PR_INTERVAL_NO_TIMEOUT);
        PR_Sleep(PR_MillisecondsToInterval(5));
      }
      return;
    }
    PR_Send(aClient, aBytes, aLen, 0, PR_INTERVAL_NO_TIMEOUT);
  }

  // RFC 1929: [1, ulen, user, plen, pass] -> [1, status].
  bool Authenticate(PRFileDesc* aClient) {
    uint8_t head[2];
    if (!ReadAll(aClient, head, 1)) return false;
    uint8_t ulen;
    if (!ReadAll(aClient, &ulen, 1)) return false;
    uint8_t user[256] = {0};
    if (!ReadAll(aClient, user, ulen)) return false;
    uint8_t plen;
    if (!ReadAll(aClient, &plen, 1)) return false;
    uint8_t pass[256] = {0};
    if (!ReadAll(aClient, pass, plen)) return false;
    bool ok = !std::strcmp(reinterpret_cast<char*>(user), kProxyUser) &&
              !std::strcmp(reinterpret_cast<char*>(pass), kProxyPass);
    const uint8_t status[2] = {0x01, static_cast<uint8_t>(ok ? 0 : 1)};
    Answer(aClient, status, 2);
    return ok;
  }

  void Serve() {
    PRNetAddr peer;
    // Waits for the client for a few seconds, and for no longer than the test.
    PRFileDesc* client = nullptr;
    for (int i = 0; i < 50 && !mStop && !client; ++i) {
      client = PR_Accept(mListener, &peer, PR_MillisecondsToInterval(100));
    }
    if (!client) {
      mClosed = true;
      return;
    }
    mConnected = true;
    uint8_t greeting[2];
    uint8_t methods[255];
    if (ReadAll(client, greeting, sizeof(greeting)) &&
        ReadAll(client, methods, greeting[1])) {
      if (mOptions.mSilent) {
        while (!mStop) PR_Sleep(PR_MillisecondsToInterval(20));
      } else {
        const uint8_t chosen[2] = {0x05,
                                   static_cast<uint8_t>(mOptions.mAuth ? 0x02 : 0x00)};
        Answer(client, chosen, 2);
        if (!mOptions.mAuth || Authenticate(client)) {
          uint8_t associate[10];
          if (ReadAll(client, associate, sizeof(associate))) {
            uint8_t reply[10] = {0x05, 0x00, 0x00, 0x01, 127, 0, 0, 1, 0, 0};
            if (mOptions.mBndUnspecified) {
              std::memset(reply + 4, 0, 4);
            }
            std::memcpy(reply + 8, &mRelayPort, 2);  // already network order
            mAssociated = associate[1] == 0x03;
            Answer(client, reply, sizeof(reply));
            // The control connection stays open for as long as the client keeps it.
            uint8_t b;
            while (!mStop) {
              int32_t n = PR_Recv(client, &b, 1, 0, PR_MillisecondsToInterval(20));
              if (n == 0) break;
              if (n < 0 && PR_GetError() != PR_IO_TIMEOUT_ERROR &&
                  PR_GetError() != PR_WOULD_BLOCK_ERROR) {
                break;
              }
            }
          }
        }
      }
    }
    PR_Close(client);
    mClosed = true;
  }

  ProxyOptions mOptions;
  PRFileDesc* mListener = nullptr;
  PRFileDesc* mRelay = nullptr;
  uint16_t mPort = 0;
  uint16_t mRelayPort = 0;
  PRNetAddr mClient;
  std::atomic<bool> mHasClient{false};
  std::thread mThread;
  std::atomic<bool> mStop{false};
  std::atomic<bool> mAssociated{false};
  std::atomic<bool> mClosed{false};
  std::atomic<bool> mConnected{false};
};

uint32_t ElapsedMs(PRIntervalTime aStart) {
  return PR_IntervalToMilliseconds(PR_IntervalNow() - aStart);
}

// Waits up to aMs for aCondition: the association is made on the socket
// thread, not on the caller's.
template <typename F>
bool WaitFor(F aCondition, uint32_t aMs) {
  PRIntervalTime start = PR_IntervalNow();
  while (!aCondition()) {
    if (ElapsedMs(start) > aMs) return false;
    PR_Sleep(PR_MillisecondsToInterval(10));
  }
  return true;
}

PRNetAddr StunServer() {
  PRNetAddr stun;
  PR_InitializeNetAddr(PR_IpAddrNull, 19302, &stun);
  stun.inet.ip = PR_htonl(0xC6336405);  // 198.51.100.5 (TEST-NET-2)
  return stun;
}

// The port nobody listens on: bind one, learn its number, close it.
uint16_t DeadPort() {
  PRFileDesc* probe = PR_NewTCPSocket();
  PRNetAddr addr = Loopback(0);
  PR_Bind(probe, &addr);
  PR_GetSockName(probe, &addr);
  uint16_t dead = PR_ntohs(addr.inet.port);
  PR_Close(probe);
  return dead;
}

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

  // A datagram to the STUN server goes to the relay, wrapped for it.
  PRNetAddr stun = StunServer();
  const char ping[] = "ping";
  EXPECT_EQ(4, PR_SendTo(fd, ping, 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  nsCString got = proxy.FirstDatagram();
  ASSERT_EQ(got.Length(), 10u + 4u);
  EXPECT_EQ(0, std::memcmp(got.get(), "\x00\x00\x00\x01\xC6\x33\x64\x05\x4B\x66", 10));
  EXPECT_EQ(0, std::memcmp(got.get() + 10, "ping", 4));
  EXPECT_TRUE(proxy.AssociateSeen());
  PR_Close(fd);
}

// The association is made on the socket thread, as a Necko socket does its own
// handshake: the socket that asks for it does not wait for the proxy. A proxy
// that is far away used to hold the thread of the ICE sockets for every round
// trip of every association, one after the other, and the first candidate of
// the page came out seconds late.
TEST(StealthSessionUdpProxy, TheLayerDoesNotWaitForTheProxy)
{
  UdpSession prefs;
  FakeSocks proxy(400);  // two answers: 800 ms before the association is made
  prefs.Arm("127.0.0.1", proxy.Port());

  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  PRIntervalTime start = PR_IntervalNow();
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  uint32_t waited = ElapsedMs(start);
  EXPECT_LT(waited, 300u) << "the socket waited " << waited
                          << " ms for a proxy that answers in 400 ms";
  EXPECT_TRUE(nsSOCKSUDPIOLayerIsOn(fd));

  // What is sent before the association is made is held, and reaches the
  // relay once it is: never sent direct.
  PRNetAddr stun = StunServer();
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  nsCString got = proxy.FirstDatagram();
  ASSERT_EQ(got.Length(), 10u + 4u);
  EXPECT_EQ(0, std::memcmp(got.get() + 10, "ping", 4));
  PR_Close(fd);
}

TEST(StealthSessionUdpProxy, WhatIsHeldBeforeTheAssociationIsMadeIsBounded)
{
  UdpSession prefs;
  FakeSocks proxy(300);
  prefs.Arm("127.0.0.1", proxy.Port());

  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  PRNetAddr stun = StunServer();
  for (int i = 0; i < 40; ++i) {
    // UDP: what cannot be held is dropped, and the sender is not told.
    EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  }
  uint32_t reached = proxy.CountDatagrams(1500);
  EXPECT_GE(reached, 1u);
  EXPECT_LE(reached, 16u);
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

TEST(StealthSessionUdpProxy, ClosingASocketBeforeTheAssociationIsMadeLetsTheProxyGo)
{
  UdpSession prefs;
  FakeSocks proxy(300);
  prefs.Arm("127.0.0.1", proxy.Port());

  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  // The proxy is being talked to (its first answer is 300 ms away) when the
  // socket is closed.
  ASSERT_TRUE(WaitFor([&] { return proxy.Connected(); }, 2000));
  ASSERT_EQ(PR_SUCCESS, PR_Close(fd));
  EXPECT_TRUE(WaitFor([&] { return proxy.ControlClosed(); }, 2000))
      << "the control connection of a socket that was closed stayed open";
}

TEST(StealthSessionUdpProxy, AnArmedSessionWhoseProxyRefusesFailsTheSocket)
{
  UdpSession prefs;
  prefs.Arm("127.0.0.1", DeadPort());
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  // The socket is made at once, with its layer; it is the first send after the
  // proxy said no that fails. It is never sent direct.
  EXPECT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  EXPECT_TRUE(nsSOCKSUDPIOLayerIsOn(fd));
  PRNetAddr stun = StunServer();
  EXPECT_TRUE(WaitFor(
      [&] { return PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT) < 0; },
      3000))
      << "an armed session must not go on with a plain, direct socket";
  PR_Close(fd);
}

TEST(StealthSessionUdpProxy, TheFailureOfOneProxyDoesNotHoldBackAnother)
{
  UdpSession prefs;
  prefs.Arm("127.0.0.1", DeadPort());
  PRFileDesc* first = PR_NewUDPSocket();
  ASSERT_TRUE(first);
  EXPECT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(first));
  PRNetAddr stun = StunServer();
  EXPECT_TRUE(WaitFor(
      [&] { return PR_SendTo(first, "x", 1, 0, &stun, PR_INTERVAL_NO_WAIT) < 0; },
      3000));
  PR_Close(first);

  // The Juggler swaps the proxy. A failure belongs to its socket, not to the
  // process: the new proxy is asked.
  FakeSocks proxy;
  prefs.Arm("127.0.0.1", proxy.Port());
  PRFileDesc* second = PR_NewUDPSocket();
  ASSERT_TRUE(second);
  EXPECT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(second));
  EXPECT_EQ(4, PR_SendTo(second, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  nsCString got = proxy.FirstDatagram();
  EXPECT_EQ(got.Length(), 10u + 4u);
  PR_Close(second);
}
// The proxy of the session is named by a host name: the resolver looks it up, as it does for the proxy of a TCP
// connection, and the connection goes on to the next address of the name when one does not answer ("localhost" is ::1
// and 127.0.0.1; the fake proxy listens on the second).
TEST(StealthSessionUdpProxy, AProxyNamedByHostIsFoundByTheResolver)
{
  UdpSession prefs;
  FakeSocks proxy;
  prefs.Arm("localhost", proxy.Port());
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  PRNetAddr stun = StunServer();
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  nsCString got = proxy.FirstDatagram();
  EXPECT_EQ(got.Length(), 10u + 4u);
  PR_Close(fd);
}

TEST(StealthSessionUdpProxy, TheProxyThatWantsAUserNameAndAPasswordGetsThem)
{
  UdpSession prefs;
  ProxyOptions options;
  options.mAuth = true;
  FakeSocks proxy(options);
  prefs.Arm("127.0.0.1", proxy.Port(), kProxyUser, kProxyPass);
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  PRNetAddr stun = StunServer();
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  EXPECT_EQ(proxy.FirstDatagram().Length(), 10u + 4u);
  PR_Close(fd);
}

TEST(StealthSessionUdpProxy, AWrongPasswordFailsTheSocketAndSendsNothing)
{
  UdpSession prefs;
  ProxyOptions options;
  options.mAuth = true;
  FakeSocks proxy(options);
  prefs.Arm("127.0.0.1", proxy.Port(), kProxyUser, "a-wrong-password");
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  PRNetAddr stun = StunServer();
  EXPECT_TRUE(WaitFor(
      [&] { return PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT) < 0; }, 3000));
  PR_Close(fd);
}

// RFC 1928 section 6: an unspecified BND.ADDR is the address the proxy was reached by.
TEST(StealthSessionUdpProxy, AnUnspecifiedRelayAddressIsTheAddressOfTheProxy)
{
  UdpSession prefs;
  ProxyOptions options;
  options.mBndUnspecified = true;
  FakeSocks proxy(options);
  prefs.Arm("127.0.0.1", proxy.Port());
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  PRNetAddr stun = StunServer();
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  EXPECT_EQ(proxy.FirstDatagram().Length(), 10u + 4u)
      << "the datagram went to 0.0.0.0, not to the proxy";
  PR_Close(fd);
}

TEST(StealthSessionUdpProxy, AnswersInPiecesOfOneByteAreRead)
{
  UdpSession prefs;
  ProxyOptions options;
  options.mSplit = true;
  FakeSocks proxy(options);
  prefs.Arm("127.0.0.1", proxy.Port());
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  PRNetAddr stun = StunServer();
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  EXPECT_EQ(proxy.FirstDatagram().Length(), 10u + 4u);
  PR_Close(fd);
}

// A proxy that does not answer ends the association at the time Necko gives a connection to be made
// (network.http.connection-timeout), not never.
TEST(StealthSessionUdpProxy, AProxyThatDoesNotAnswerFailsTheSocketAtTheTimeOfAConnection)
{
  UdpSession prefs;
  nsCOMPtr<nsIIOService> io = do_GetIOService();
  nsCOMPtr<nsIProtocolHandler> http;
  ASSERT_TRUE(io);
  ASSERT_TRUE(NS_SUCCEEDED(io->GetProtocolHandler("http", getter_AddRefs(http))));
  Preferences::SetInt("network.http.connection-timeout", 1);
  ProxyOptions options;
  options.mSilent = true;
  FakeSocks proxy(options);
  prefs.Arm("127.0.0.1", proxy.Port());
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  PRNetAddr stun = StunServer();
  EXPECT_TRUE(WaitFor(
      [&] { return PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT) < 0; }, 4000))
      << "a proxy that never answers kept the socket waiting";
  PR_Close(fd);
  Preferences::ClearUser("network.http.connection-timeout");
}

TEST(StealthSessionUdpProxy, ASocketReadsNothingBeforeTheAssociationAndFailsAfterAFailure)
{
  UdpSession prefs;
  {
    FakeSocks proxy(300);
    prefs.Arm("127.0.0.1", proxy.Port());
    PRFileDesc* fd = PR_NewUDPSocket();
    ASSERT_TRUE(fd);
    ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
    char buf[64];
    PRNetAddr from;
    EXPECT_EQ(-1, PR_RecvFrom(fd, buf, sizeof(buf), 0, &from, PR_INTERVAL_NO_WAIT));
    EXPECT_EQ(PR_WOULD_BLOCK_ERROR, PR_GetError());
    PR_Close(fd);
  }
  prefs.Arm("127.0.0.1", DeadPort());
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  PRNetAddr stun = StunServer();
  ASSERT_TRUE(WaitFor(
      [&] { return PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT) < 0; }, 3000));
  char buf[64];
  PRNetAddr from;
  EXPECT_EQ(-1, PR_RecvFrom(fd, buf, sizeof(buf), 0, &from, PR_INTERVAL_NO_WAIT));
  EXPECT_NE(PR_WOULD_BLOCK_ERROR, PR_GetError()) << "a failed association is not 'try again'";
  PR_Close(fd);
}

// RFC 1928 section 7: the datagrams of the relay are the ones the application is given.
TEST(StealthSessionUdpProxy, OnlyTheDatagramsOfTheRelayAreHandedToTheApplication)
{
  UdpSession prefs;
  FakeSocks proxy;
  prefs.Arm("127.0.0.1", proxy.Port());
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  PRNetAddr stun = StunServer();
  ASSERT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  ASSERT_EQ(proxy.FirstDatagram().Length(), 14u);  // the relay knows the client now
  ASSERT_TRUE(proxy.SendToClient("evil", /* aFromTheRelay = */ false));
  ASSERT_TRUE(proxy.SendToClient("good", /* aFromTheRelay = */ true));
  nsCString got;
  EXPECT_TRUE(WaitFor(
      [&] {
        char buf[64];
        PRNetAddr from;
        int32_t n = PR_RecvFrom(fd, buf, sizeof(buf), 0, &from, PR_INTERVAL_NO_WAIT);
        if (n > 0) got.Assign(buf, n);
        return n > 0;
      },
      2000));
  EXPECT_TRUE(got.EqualsLiteral("good")) << "got '" << got.get() << "'";
  PR_Close(fd);
}

// Nothing a socket with the layer sends is sent direct: not what is held, not what is sent after the proxy refused. A
// receiver stands where the datagram was addressed to.
TEST(StealthSessionUdpProxy, NothingIsEverSentDirect)
{
  UdpSession prefs;
  PRFileDesc* receiver = PR_NewUDPSocket();
  PRNetAddr where = Loopback(0);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(receiver, &where));
  PR_GetSockName(receiver, &where);
  char buf[64];
  PRNetAddr from;
  {
    FakeSocks proxy(300);  // the datagram is held, then goes to the relay
    prefs.Arm("127.0.0.1", proxy.Port());
    PRFileDesc* fd = PR_NewUDPSocket();
    ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
    EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &where, PR_INTERVAL_NO_WAIT));
    EXPECT_EQ(proxy.FirstDatagram().Length(), 14u);
    PR_Close(fd);
  }
  prefs.Arm("127.0.0.1", DeadPort());  // the proxy refuses
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAttachSession(fd));
  (void)PR_SendTo(fd, "ping", 4, 0, &where, PR_INTERVAL_NO_WAIT);
  PR_Sleep(PR_MillisecondsToInterval(300));
  (void)PR_SendTo(fd, "ping", 4, 0, &where, PR_INTERVAL_NO_WAIT);
  PR_Close(fd);
  EXPECT_EQ(-1, PR_RecvFrom(receiver, buf, sizeof(buf), 0, &from, PR_MillisecondsToInterval(300)))
      << "a datagram arrived at its destination without the proxy";
  PR_Close(receiver);
}

}  // namespace mozilla::net
