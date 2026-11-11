/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "../../base/StealthSocksCredentials.h"
#include "gtest/gtest.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/NullPrincipal.h"
#include "mozilla/Preferences.h"
#include "mozilla/net/DNS.h"
#include "mozilla/ipc/BackgroundUtils.h"
#include "mozilla/net/NeckoChannelParams.h"
#include "nsIPrincipal.h"
#include "nsIEventTarget.h"
#include "nsISocketTransportService.h"
#include "nsThreadUtils.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "nsCOMPtr.h"
#include "nsComponentManagerUtils.h"
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
#include "nsIUDPSocket.h"
#include "nsNetUtil.h"
#include "StealthPcUdpProxy.h"
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

// netwerk/socket/nsSOCKSIOLayer.h is not exported: the one function the SOCKS5 connection of a context calls.
nsresult nsSOCKSIOLayerAddToSocket(int32_t family, const char* host, int32_t port, nsIProxyInfo* proxyInfo,
                                   int32_t socksVersion, uint32_t flags, uint32_t tlsFlags, PRFileDesc* fd);

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
// The SOCKS5 UDP layer takes the proxy of the PeerConnection the socket belongs to (PcUdpProxy: what Firefox answers for
// the document). A fake SOCKS5 server on the loopback stands for the proxy. They live in this file because it is already
// listed in netwerk/test/gtest/moz.build as ours: a second file would be a second line in a Mozilla build file.

namespace {

// The proxy of the PeerConnection of a test, and the layer put on a UDP socket for it.
class UdpSession {
 public:
  void Arm(const char* aHost, int32_t aPort, const char* aUser = "", const char* aPass = "") {
    PcUdpProxy::Socks5Info info;
    info.mHost = aHost;
    info.mPort = aPort;
    info.mUser = aUser;
    info.mPass = aPass;
    mProxy = PcUdpProxy::Known(PcUdpProxy::Kind::Socks5, info);
  }
  void NoProxy() { mProxy = PcUdpProxy::Known(PcUdpProxy::Kind::Direct); }
  void ProxyWithoutUdp() { mProxy = PcUdpProxy::Known(PcUdpProxy::Kind::Blocked); }
  nsresult Attach(PRFileDesc* aFd) { return nsSOCKSUDPIOLayerAddToSocket(aFd, mProxy); }

 private:
  RefPtr<PcUdpProxy> mProxy;
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
PRNetAddr HostAddress(uint16_t aPort);

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
    if (!aFromTheRelay) {
      // Someone else on the network, not the machine itself: a datagram of the loopback is handed over as it is.
      PRNetAddr other = HostAddress(0);
      PR_Bind(from, &other);
    }
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

bool IsLoopbackForTest(const PRNetAddr& aAddr) { return (PR_ntohl(aAddr.inet.ip) >> 24) == 127; }

// The address of this host that is not loopback: where a datagram that is NOT to the machine itself can be received.
PRNetAddr HostAddress(uint16_t aPort) {
  PRNetAddr probe;
  PR_InitializeNetAddr(PR_IpAddrNull, 9, &probe);
  probe.inet.ip = PR_htonl(0xC0000201);  // 192.0.2.1: nothing is sent
  PRFileDesc* s = PR_NewUDPSocket();
  PRNetAddr mine;
  PR_InitializeNetAddr(PR_IpAddrAny, 0, &mine);
  if (PR_Connect(s, &probe, PR_INTERVAL_NO_WAIT) == PR_SUCCESS) {
    PR_GetSockName(s, &mine);
  }
  PR_Close(s);
  mine.inet.port = PR_htons(aPort);
  return mine;
}

// The sockets of the engine (nsUDPSocket, NrSocket) are non-blocking: with nothing to read they answer WOULD_BLOCK, where a
// blocking one answers a timeout.
PRFileDesc* NonBlockingUdpSocket() {
  PRFileDesc* fd = PR_NewUDPSocket();
  if (fd) {
    PRSocketOptionData opt;
    opt.option = PR_SockOpt_Nonblocking;
    opt.value.non_blocking = PR_TRUE;
    PR_SetSocketOption(fd, &opt);
  }
  return fd;
}

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

// A PeerConnection with no proxy is a machine with none: the socket sends where it is told to.
TEST(StealthSessionUdpProxy, APeerConnectionWithNoProxyLeavesTheSocketPlain)
{
  UdpSession prefs;
  prefs.NoProxy();
  PRFileDesc* receiver = PR_NewUDPSocket();
  PRNetAddr where = Loopback(0);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(receiver, &where));
  PR_GetSockName(receiver, &where);
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &where, PR_INTERVAL_NO_WAIT));
  char buf[64];
  PRNetAddr from;
  EXPECT_EQ(4, PR_RecvFrom(receiver, buf, sizeof(buf), 0, &from, PR_SecondsToInterval(2)));
  PR_Close(fd);
  PR_Close(receiver);
}

// A proxy that carries no UDP (HTTP, HTTPS, SOCKS4) is a router that drops it: the send succeeds, nothing leaves.
TEST(StealthSessionUdpProxy, AProxyThatCarriesNoUdpIsARouterThatDropsIt)
{
  UdpSession prefs;
  prefs.ProxyWithoutUdp();
  PRFileDesc* receiver = PR_NewUDPSocket();
  PRNetAddr any;
  PR_InitializeNetAddr(PR_IpAddrAny, 0, &any);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(receiver, &any));
  PR_GetSockName(receiver, &any);
  PRNetAddr where = HostAddress(PR_ntohs(any.inet.port));  // not loopback: that one goes direct
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
  EXPECT_TRUE(WaitFor([&] { return nsSOCKSUDPIOLayerHasFailed(fd); }, 2000));
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &where, PR_INTERVAL_NO_WAIT));
  char buf[64];
  PRNetAddr from;
  EXPECT_EQ(-1, PR_RecvFrom(receiver, buf, sizeof(buf), 0, &from, PR_MillisecondsToInterval(400)));
  PR_Close(fd);
  PR_Close(receiver);
}

TEST(StealthSessionUdpProxy, TheLayerConnectsToTheProxyOfThePeerConnection)
{
  UdpSession prefs;
  FakeSocks proxy;
  prefs.Arm("127.0.0.1", proxy.Port());

  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
  // proxy said no is dropped, as behind a router that blocks UDP. It is never sent direct.
  EXPECT_EQ(NS_OK, prefs.Attach(fd));
  EXPECT_TRUE(nsSOCKSUDPIOLayerIsOn(fd));
  PRNetAddr stun = StunServer();
  EXPECT_TRUE(WaitFor(
      [&] { return nsSOCKSUDPIOLayerHasFailed(fd); },
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
  EXPECT_EQ(NS_OK, prefs.Attach(first));
  PRNetAddr stun = StunServer();
  EXPECT_TRUE(WaitFor(
      [&] { return nsSOCKSUDPIOLayerHasFailed(first); },
      3000));
  PR_Close(first);

  // The Juggler swaps the proxy. A failure belongs to its socket, not to the
  // process: the new proxy is asked.
  FakeSocks proxy;
  prefs.Arm("127.0.0.1", proxy.Port());
  PRFileDesc* second = PR_NewUDPSocket();
  ASSERT_TRUE(second);
  EXPECT_EQ(NS_OK, prefs.Attach(second));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
  PRNetAddr stun = StunServer();
  EXPECT_TRUE(WaitFor(
      [&] { return nsSOCKSUDPIOLayerHasFailed(fd); }, 3000));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
  PRNetAddr stun = StunServer();
  EXPECT_TRUE(WaitFor(
      [&] { return nsSOCKSUDPIOLayerHasFailed(fd); }, 4000))
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
    PRFileDesc* fd = NonBlockingUdpSocket();
    ASSERT_TRUE(fd);
    ASSERT_EQ(NS_OK, prefs.Attach(fd));
    char buf[64];
    PRNetAddr from;
    EXPECT_EQ(-1, PR_RecvFrom(fd, buf, sizeof(buf), 0, &from, PR_INTERVAL_NO_WAIT));
    EXPECT_EQ(PR_WOULD_BLOCK_ERROR, PR_GetError());
    PR_Close(fd);
  }
  prefs.Arm("127.0.0.1", DeadPort());
  PRFileDesc* fd = NonBlockingUdpSocket();
  ASSERT_TRUE(fd);
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
  PRNetAddr stun = StunServer();
  ASSERT_TRUE(WaitFor(
      [&] { return nsSOCKSUDPIOLayerHasFailed(fd); }, 3000));
  char buf[64];
  PRNetAddr from;
  EXPECT_EQ(-1, PR_RecvFrom(fd, buf, sizeof(buf), 0, &from, PR_INTERVAL_NO_WAIT));
  EXPECT_EQ(PR_WOULD_BLOCK_ERROR, PR_GetError()) << "a network that blocks UDP answers nothing";
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
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
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
// receiver stands where the datagram was addressed to (an address of the host that is not loopback).
TEST(StealthSessionUdpProxy, NothingIsEverSentDirect)
{
  UdpSession prefs;
  PRFileDesc* receiver = PR_NewUDPSocket();
  PRNetAddr any = Loopback(0);
  PR_InitializeNetAddr(PR_IpAddrAny, 0, &any);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(receiver, &any));
  PR_GetSockName(receiver, &any);
  PRNetAddr where = HostAddress(PR_ntohs(any.inet.port));
  ASSERT_FALSE(IsLoopbackForTest(where)) << "this host has no address but loopback";
  char buf[64];
  PRNetAddr from;
  {
    FakeSocks proxy(300);  // the datagram is held, then goes to the relay
    prefs.Arm("127.0.0.1", proxy.Port());
    PRFileDesc* fd = PR_NewUDPSocket();
    ASSERT_EQ(NS_OK, prefs.Attach(fd));
    EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &where, PR_INTERVAL_NO_WAIT));
    EXPECT_EQ(proxy.FirstDatagram().Length(), 14u);
    PR_Close(fd);
  }
  prefs.Arm("127.0.0.1", DeadPort());  // the proxy refuses
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
  (void)PR_SendTo(fd, "ping", 4, 0, &where, PR_INTERVAL_NO_WAIT);
  ASSERT_TRUE(WaitFor([&] { return nsSOCKSUDPIOLayerHasFailed(fd); }, 3000));
  // After the failure a send succeeds and the datagram goes nowhere: no error, no answer.
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &where, PR_INTERVAL_NO_WAIT));
  PR_Close(fd);
  EXPECT_EQ(-1, PR_RecvFrom(receiver, buf, sizeof(buf), 0, &from, PR_MillisecondsToInterval(300)))
      << "a datagram arrived at its destination without the proxy";
  PR_Close(receiver);
}

// To the machine itself the datagram goes direct, whatever the proxy (Firefox does the same for localhost), and what comes
// from the machine itself is handed over as it is; to anywhere else it goes by the relay.
TEST(StealthSessionUdpProxy, TheMachineItselfIsReachedDirectAndTheRestByTheRelay)
{
  UdpSession prefs;
  FakeSocks proxy;
  prefs.Arm("127.0.0.1", proxy.Port());
  PRFileDesc* receiver = PR_NewUDPSocket();
  PRNetAddr where = Loopback(0);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(receiver, &where));
  PR_GetSockName(receiver, &where);
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
  PRNetAddr stun = StunServer();
  ASSERT_EQ(4, PR_SendTo(fd, "pong", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  ASSERT_EQ(proxy.FirstDatagram().Length(), 14u);  // a public destination: by the relay
  ASSERT_EQ(5, PR_SendTo(fd, "local", 5, 0, &where, PR_INTERVAL_NO_WAIT));
  char buf[64];
  PRNetAddr from;
  EXPECT_EQ(5, PR_RecvFrom(receiver, buf, sizeof(buf), 0, &from, PR_SecondsToInterval(2)))
      << "a datagram to the machine itself did not arrive direct";
  EXPECT_EQ(0u, proxy.CountDatagrams(300)) << "a datagram to the machine itself went by the relay";
  // and back: the machine answers direct, the layer hands it over unframed
  PRNetAddr mine;
  PR_GetSockName(fd, &mine);
  // The socket is bound to the wildcard address by its first send: Linux sends to 0.0.0.0 as to this machine, Windows refuses
  // it, so the answer is addressed to the loopback the socket was reached by.
  mine.inet.ip = PR_htonl(0x7F000001);
  ASSERT_EQ(2, PR_SendTo(receiver, "ok", 2, 0, &mine, PR_INTERVAL_NO_WAIT));
  nsCString got;
  EXPECT_TRUE(WaitFor(
      [&] {
        PRNetAddr src;
        int32_t n = PR_RecvFrom(fd, buf, sizeof(buf), 0, &src, PR_INTERVAL_NO_WAIT);
        if (n > 0) got.Assign(buf, n);
        return n > 0;
      },
      2000));
  EXPECT_TRUE(got.EqualsLiteral("ok")) << "got '" << got.get() << "'";
  PR_Close(fd);
  PR_Close(receiver);
}

// ── The pool of associations (appended to TestStealthSocksCredentials.cpp) ──────────────────────────────────────────
//
// Necko opens a SOCKS5 connection for a context: that is where the proxy is learned, and the associations the ICE
// sockets of the context will need are made then (two of them, as many as a gather's first sockets), so a socket takes
// one that is made, or being made, instead of paying the handshake round trips itself. A socket's association is the
// only one that is ever asked of the proxy by the layer: the pool is where they are made.

namespace {

// A SOCKS5 server for any number of clients: every control connection gets its own thread, every ASSOCIATE answers with
// the same loopback UDP relay. It counts what the proxy saw.
struct PoolProxyOptions {
  uint32_t mDelayMs = 0;         // every answer on a control connection waits for it
  bool mRefuse = false;          // UDP ASSOCIATE is answered "command not supported"
  uint32_t mCloseFirst = 0;      // the control connections of the first N clients are closed by the proxy once associated
};

class PoolSocks {
 public:
  explicit PoolSocks(const PoolProxyOptions& aOptions = PoolProxyOptions()) : mOptions(aOptions) {
    mListener = PR_NewTCPSocket();
    mRelay = PR_NewUDPSocket();
    PRNetAddr addr = Loopback(0);
    PR_Bind(mListener, &addr);
    PR_Listen(mListener, 16);
    PR_GetSockName(mListener, &addr);
    mPort = PR_ntohs(addr.inet.port);
    PRNetAddr relay = Loopback(0);
    PR_Bind(mRelay, &relay);
    PR_GetSockName(mRelay, &relay);
    mRelayPort = relay.inet.port;
    mAcceptor = std::thread([this] { Accept(); });
  }

  ~PoolSocks() {
    mStop = true;
    mAcceptor.join();
    for (std::thread& t : mClients) {
      t.join();
    }
    PR_Close(mListener);
    PR_Close(mRelay);
  }

  uint16_t Port() const { return mPort; }
  uint32_t Connections() const { return mConnections; }
  uint32_t Associates() const { return mAssociates; }  // UDP ASSOCIATE requests read
  uint32_t Answered() const { return mAnswered; }      // the ones answered with a relay

  // The first datagram the relay gets within aMs, or the empty string. Returns when it arrived.
  nsCString NextDatagram(uint32_t aMs, PRIntervalTime* aAt = nullptr) {
    char buf[256];
    int32_t n = PR_RecvFrom(mRelay, buf, sizeof(buf), 0, &mClient, PR_MillisecondsToInterval(aMs));
    if (aAt) *aAt = PR_IntervalNow();
    return n > 0 ? nsCString(buf, n) : nsCString();
  }

 private:
  bool ReadAll(PRFileDesc* aFd, uint8_t* aBuf, size_t aLen) {
    size_t got = 0;
    while (got < aLen) {
      int32_t n = PR_Recv(aFd, aBuf + got, aLen - got, 0, PR_MillisecondsToInterval(100));
      if (n == 0) return false;
      if (n < 0) {
        if (PR_GetError() == PR_IO_TIMEOUT_ERROR && !mStop) continue;
        return false;
      }
      got += n;
    }
    return true;
  }

  void Answer(PRFileDesc* aClient, const uint8_t* aBytes, int32_t aLen) {
    if (mOptions.mDelayMs) PR_Sleep(PR_MillisecondsToInterval(mOptions.mDelayMs));
    PR_Send(aClient, aBytes, aLen, 0, PR_INTERVAL_NO_TIMEOUT);
  }

  void Accept() {
    while (!mStop) {
      PRNetAddr peer;
      PRFileDesc* client = PR_Accept(mListener, &peer, PR_MillisecondsToInterval(50));
      if (!client) continue;
      uint32_t index = mConnections++;
      mClients.emplace_back([this, client, index] { Serve(client, index); });
    }
  }

  void Serve(PRFileDesc* aClient, uint32_t aIndex) {
    uint8_t greeting[2];
    uint8_t methods[255];
    if (ReadAll(aClient, greeting, 2) && ReadAll(aClient, methods, greeting[1])) {
      const uint8_t chosen[2] = {0x05, 0x00};
      Answer(aClient, chosen, 2);
      uint8_t associate[10];
      if (ReadAll(aClient, associate, sizeof(associate))) {
        ++mAssociates;
        uint8_t reply[10] = {0x05, static_cast<uint8_t>(mOptions.mRefuse ? 0x07 : 0x00), 0x00, 0x01, 127, 0, 0, 1, 0, 0};
        std::memcpy(reply + 8, &mRelayPort, 2);  // already in network order
        Answer(aClient, reply, sizeof(reply));
        if (!mOptions.mRefuse) {
          ++mAnswered;
          if (aIndex < mOptions.mCloseFirst) {
            PR_Sleep(PR_MillisecondsToInterval(100));  // the relay is up for a moment, then the proxy ends it
          } else {
            uint8_t b;
            while (!mStop) {
              int32_t n = PR_Recv(aClient, &b, 1, 0, PR_MillisecondsToInterval(20));
              if (n == 0) break;
              if (n < 0 && PR_GetError() != PR_IO_TIMEOUT_ERROR && PR_GetError() != PR_WOULD_BLOCK_ERROR) break;
            }
          }
        }
      }
    }
    PR_Close(aClient);
  }

  PoolProxyOptions mOptions;
  PRFileDesc* mListener = nullptr;
  PRFileDesc* mRelay = nullptr;
  uint16_t mPort = 0;
  uint16_t mRelayPort = 0;
  PRNetAddr mClient;
  std::thread mAcceptor;
  std::vector<std::thread> mClients;  // only the acceptor adds, the destructor joins after the acceptor is gone
  std::atomic<bool> mStop{false};
  std::atomic<uint32_t> mConnections{0};
  std::atomic<uint32_t> mAssociates{0};
  std::atomic<uint32_t> mAnswered{0};
};

void Warm(uint16_t aPort) {
  nsSOCKSUDPIOLayerWarm("127.0.0.1"_ns, aPort, ""_ns, ""_ns);
}

RefPtr<PcUdpProxy> Socks5(uint16_t aPort) {
  PcUdpProxy::Socks5Info info;
  info.mHost.AssignLiteral("127.0.0.1");
  info.mPort = aPort;
  return PcUdpProxy::Known(PcUdpProxy::Kind::Socks5, info);
}

}  // namespace

TEST(StealthSocksUdpPool, WarmingMakesTwoAssociationsBeforeAnySocketAsksForOne)
{
  PoolSocks proxy;
  Warm(proxy.Port());
  EXPECT_TRUE(WaitFor([&] { return proxy.Answered() == 2; }, 3000)) << "answered " << proxy.Answered();
  PR_Sleep(PR_MillisecondsToInterval(300));
  EXPECT_EQ(2u, proxy.Associates()) << "warming asks the proxy for exactly the two it keeps";
  // Warming again (every SOCKS5 connection of the context does) asks for nothing while the two are there.
  Warm(proxy.Port());
  Warm(proxy.Port());
  PR_Sleep(PR_MillisecondsToInterval(300));
  EXPECT_EQ(2u, proxy.Associates());
}

TEST(StealthSocksUdpPool, ASocketTakesAnAssociationThatIsAlreadyMade)
{
  PoolProxyOptions slow;
  slow.mDelayMs = 300;  // the greeting and the ASSOCIATE each wait 300 ms: 600 ms for an association made on demand
  PoolSocks proxy(slow);
  Warm(proxy.Port());
  ASSERT_TRUE(WaitFor([&] { return proxy.Answered() == 2; }, 4000));

  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  PRNetAddr stun = StunServer();
  PRIntervalTime start = PR_IntervalNow();
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAddToSocket(fd, Socks5(proxy.Port()).get()));
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  PRIntervalTime at;
  nsCString got = proxy.NextDatagram(3000, &at);
  ASSERT_EQ(got.Length(), 14u);
  EXPECT_LT(PR_IntervalToMilliseconds(at - start), 200u)
      << "the datagram waited for an association that was already made";
  PR_Close(fd);
}

TEST(StealthSocksUdpPool, WhatIsTakenIsReplacedSoTheNextSocketFindsOneToo)
{
  PoolSocks proxy;
  Warm(proxy.Port());
  ASSERT_TRUE(WaitFor([&] { return proxy.Answered() == 2; }, 3000));
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAddToSocket(fd, Socks5(proxy.Port()).get()));
  EXPECT_TRUE(WaitFor([&] { return proxy.Answered() == 3; }, 3000)) << "the one taken was not made again";
  PR_Sleep(PR_MillisecondsToInterval(300));
  EXPECT_EQ(3u, proxy.Associates()) << "two are kept, not more";
  PR_Close(fd);
}

TEST(StealthSocksUdpPool, ASocketThatFindsNoneMakesItself)
{
  // Nothing warmed this proxy: the socket asks for its association, and only one is made.
  PoolSocks proxy;
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAddToSocket(fd, Socks5(proxy.Port()).get()));
  PRNetAddr stun = StunServer();
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  EXPECT_EQ(proxy.NextDatagram(3000).Length(), 14u);
  PR_Sleep(PR_MillisecondsToInterval(300));
  EXPECT_EQ(1u, proxy.Associates());
  PR_Close(fd);
}

TEST(StealthSocksUdpPool, AssociationsOfOneProxyAreNotHandedToAnother)
{
  PoolSocks a;
  PoolSocks b;
  Warm(a.Port());
  ASSERT_TRUE(WaitFor([&] { return a.Answered() == 2; }, 3000));
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAddToSocket(fd, Socks5(b.Port()).get()));
  PRNetAddr stun = StunServer();
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  EXPECT_EQ(b.NextDatagram(3000).Length(), 14u) << "the datagram went through the other proxy's relay";
  EXPECT_EQ(2u, a.Associates());
  PR_Close(fd);
}

TEST(StealthSocksUdpPool, AProxyThatRefusedIsNotAskedAgainByWarmingOnlyBySockets)
{
  PoolProxyOptions refuse;
  refuse.mRefuse = true;
  PoolSocks proxy(refuse);
  Warm(proxy.Port());
  ASSERT_TRUE(WaitFor([&] { return proxy.Associates() == 2; }, 3000));
  PR_Sleep(PR_MillisecondsToInterval(300));
  uint32_t asked = proxy.Associates();
  Warm(proxy.Port());  // the next SOCKS5 connection of the context
  Warm(proxy.Port());
  PR_Sleep(PR_MillisecondsToInterval(300));
  EXPECT_EQ(asked, proxy.Associates()) << "warming asks a proxy that said no again and again";

  // A socket that needs one asks, finds the proxy still says no, and fails: nothing is sent, nothing direct.
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAddToSocket(fd, Socks5(proxy.Port()).get()));
  PRNetAddr stun = StunServer();
  EXPECT_TRUE(WaitFor([&] { return nsSOCKSUDPIOLayerHasFailed(fd); }, 3000));
  EXPECT_EQ(asked + 1, proxy.Associates()) << "a socket's own need is one ASSOCIATE, not a retry loop";
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  PR_Close(fd);
}

TEST(StealthSocksUdpPool, AnAssociationWhoseControlConnectionWasClosedIsNotHandedOut)
{
  PoolProxyOptions closing;
  closing.mCloseFirst = 2;  // the proxy ends the two warmed ones while they wait (an idle timeout)
  PoolSocks proxy(closing);
  Warm(proxy.Port());
  ASSERT_TRUE(WaitFor([&] { return proxy.Answered() == 2; }, 3000));
  PR_Sleep(PR_MillisecondsToInterval(500));  // closed by now; the control connection is watched

  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAddToSocket(fd, Socks5(proxy.Port()).get()));
  PRNetAddr stun = StunServer();
  EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  EXPECT_EQ(proxy.NextDatagram(3000).Length(), 14u) << "the socket was given an association the proxy had ended";
  EXPECT_FALSE(nsSOCKSUDPIOLayerHasFailed(fd));
  EXPECT_GE(proxy.Associates(), 3u);
  PR_Close(fd);
}

// The pool starts associations with its lock held; a start that cannot be made (no socket transport service: a browser that is
// shutting down) used to take the same lock again to tell the pool that the proxy had refused: a thread that waits for itself.
TEST(StealthSocksUdpPool, AStartThatCannotBeMadeDoesNotTakeThePoolLockTwice)
{
  PoolSocks proxy;
  nsSOCKSUDPIOLayerNoSocketThreadForTests(true);
  struct Restore {
    ~Restore() { nsSOCKSUDPIOLayerNoSocketThreadForTests(false); }
  } restore;
  std::atomic<bool> done{false};
  std::thread worker([&] {
    Warm(proxy.Port());                         // PoolFill starts two associations under the lock
    PRFileDesc* fd = PR_NewUDPSocket();         // Take starts one under the lock
    EXPECT_EQ(NS_OK, nsSOCKSUDPIOLayerAddToSocket(fd, Socks5(proxy.Port()).get()));
    EXPECT_TRUE(WaitFor([&] { return nsSOCKSUDPIOLayerHasFailed(fd); }, 2000)) << "the socket of an association that cannot start fails";
    PR_Close(fd);
    done = true;
  });
  bool finished = WaitFor([&] { return done.load(); }, 5000);
  if (!finished) {
    worker.detach();  // it is waiting for a lock it holds: the test cannot join it
  } else {
    worker.join();
  }
  EXPECT_TRUE(finished) << "the pool lock was taken twice by one thread";
  EXPECT_EQ(0u, proxy.Associates());
}

TEST(StealthSocksUdpPool, SixSocketsAtOnceEachGetAnAssociation)
{
  PoolSocks proxy;
  Warm(proxy.Port());
  ASSERT_TRUE(WaitFor([&] { return proxy.Answered() == 2; }, 3000));
  PRFileDesc* fds[6];
  PRNetAddr stun = StunServer();
  for (PRFileDesc*& fd : fds) {
    fd = PR_NewUDPSocket();
    ASSERT_EQ(NS_OK, nsSOCKSUDPIOLayerAddToSocket(fd, Socks5(proxy.Port()).get()));
    EXPECT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &stun, PR_INTERVAL_NO_WAIT));
  }
  uint32_t arrived = 0;
  while (arrived < 6 && proxy.NextDatagram(3000).Length() == 14u) ++arrived;
  EXPECT_EQ(6u, arrived);
  for (PRFileDesc* fd : fds) PR_Close(fd);
}

TEST(StealthSocksUdpPool, TheSocks5ConnectionOfAContextWarmsThePool)
{
  // Where Firefox learns the proxy: the SOCKS5 layer of a connection.
  PoolSocks proxy;
  nsCOMPtr<nsIProtocolProxyService> pps = do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
  ASSERT_TRUE(pps);
  nsCOMPtr<nsIProxyInfo> info;
  ASSERT_EQ(NS_OK, pps->NewProxyInfo("socks"_ns, "127.0.0.1"_ns, proxy.Port(), ""_ns, ""_ns, 0, UINT32_MAX, nullptr,
                                     getter_AddRefs(info)));
  PRFileDesc* tcp = PR_OpenTCPSocket(PR_AF_INET);
  ASSERT_TRUE(tcp);
  ASSERT_EQ(NS_OK, nsSOCKSIOLayerAddToSocket(PR_AF_INET, "example.test", 443, info, 5, 0, 0, tcp));
  EXPECT_TRUE(WaitFor([&] { return proxy.Answered() == 2; }, 3000))
      << "the SOCKS5 connection of the context did not make the associations: answered " << proxy.Answered();
  PR_Close(tcp);
}

// An HTTP/3 connection's UDP socket (nsIUDPSocket) goes by the SOCKS5 proxy of the connection: the nsIProxyInfo it was
// made with, as the layer of the ICE sockets, with the same associations.
static already_AddRefed<nsIProxyInfo> Socks5ProxyInfo(const char* aHost, int32_t aPort) {
  nsCOMPtr<nsIProtocolProxyService> pps = do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
  nsCOMPtr<nsIProxyInfo> info;
  if (!pps || NS_FAILED(pps->NewProxyInfoWithAuth("socks"_ns, nsDependentCString(aHost), aPort, ""_ns, ""_ns, ""_ns,
                                                 ""_ns, 0, UINT32_MAX, nullptr, getter_AddRefs(info)))) {
    return nullptr;
  }
  return info.forget();
}

TEST(StealthSessionUdpProxy, AnHttp3SocketGoesByTheSocks5ProxyOfItsConnection)
{
  FakeSocks proxy;
  nsCOMPtr<nsIProxyInfo> info = Socks5ProxyInfo("127.0.0.1", proxy.Port());
  ASSERT_TRUE(info);
  nsCOMPtr<nsIUDPSocket> socket = do_CreateInstance("@mozilla.org/network/udp-socket;1");
  ASSERT_TRUE(socket);
  ASSERT_EQ(NS_OK, socket->SetSocksProxy(info));
  NetAddr local;
  local.raw.family = AF_INET;
  ASSERT_EQ(NS_OK, socket->InitWithAddress(&local, nullptr, false, 1));
  PRNetAddr stun = StunServer();
  NetAddr to(&stun);
  uint32_t sent = 0;
  const uint8_t ping[4] = {'p', 'i', 'n', 'g'};
  ASSERT_EQ(NS_OK, socket->SendWithAddress(&to, ping, 4, &sent));
  nsCString got = proxy.FirstDatagram();
  ASSERT_EQ(got.Length(), 10u + 4u) << "the datagram of the socket did not go to the relay";
  EXPECT_EQ(0, std::memcmp(got.get() + 10, "ping", 4));
  socket->Close();
}

TEST(StealthSessionUdpProxy, AnHttp3SocketBehindAProxyThatRefusesSendsNothing)
{
  PRFileDesc* receiver = PR_NewUDPSocket();
  PRNetAddr any;
  PR_InitializeNetAddr(PR_IpAddrAny, 0, &any);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(receiver, &any));
  PR_GetSockName(receiver, &any);
  PRNetAddr where = HostAddress(PR_ntohs(any.inet.port));
  nsCOMPtr<nsIProxyInfo> info = Socks5ProxyInfo("127.0.0.1", DeadPort());
  ASSERT_TRUE(info);
  nsCOMPtr<nsIUDPSocket> socket = do_CreateInstance("@mozilla.org/network/udp-socket;1");
  ASSERT_EQ(NS_OK, socket->SetSocksProxy(info));
  NetAddr local;
  local.raw.family = AF_INET;
  ASSERT_EQ(NS_OK, socket->InitWithAddress(&local, nullptr, false, 1));
  NetAddr to(&where);
  uint32_t sent = 0;
  const uint8_t ping[4] = {'p', 'i', 'n', 'g'};
  (void)socket->SendWithAddress(&to, ping, 4, &sent);
  PR_Sleep(PR_MillisecondsToInterval(500));
  (void)socket->SendWithAddress(&to, ping, 4, &sent);
  char buf[64];
  PRNetAddr from;
  EXPECT_EQ(-1, PR_RecvFrom(receiver, buf, sizeof(buf), 0, &from, PR_MillisecondsToInterval(300)))
      << "a datagram of an HTTP/3 socket arrived without the proxy";
  socket->Close();
  PR_Close(receiver);
}
// The proxy of a PeerConnection is what Firefox answers for the browser context, not for the page's own address. A page on
// the loopback, a file:, or a sandboxed document (null principal) has none of its own, and "no proxy for this address" must not
// become "this PeerConnection has none" while the session has a SOCKS5 proxy: its STUN and TURN would leave direct.
TEST(StealthSessionUdpProxy, ADocumentThatHasNoAddressOfItsOwnFollowsTheSessionProxy)
{
  ProxyPrefsReset reset;
  ASSERT_EQ(Preferences::SetInt("network.proxy.type", 1), NS_OK);
  ASSERT_EQ(Preferences::SetCString("network.proxy.socks", "192.0.2.7"), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.socks_port", 1080), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.socks_version", 5), NS_OK);
  nsTArray<nsCOMPtr<nsIPrincipal>> principals;
  for (const char* spec : {"http://127.0.0.1:8080/", "http://localhost/", "file:///tmp/page.html", "https://example.com/"}) {
    nsCOMPtr<nsIURI> uri;
    ASSERT_EQ(NS_OK, NS_NewURI(getter_AddRefs(uri), nsDependentCString(spec)));
    principals.AppendElement(BasePrincipal::CreateContentPrincipal(uri, OriginAttributes()));
  }
  principals.AppendElement(NullPrincipal::CreateWithoutOriginAttributes());
  for (nsIPrincipal* principal : principals) {
    ASSERT_TRUE(principal);
    nsAutoCString origin;
    principal->GetOrigin(origin);
    nsCOMPtr<nsIURI> uri;
    ASSERT_EQ(NS_OK, NS_NewURI(getter_AddRefs(uri), "https://example.net/"_ns));
    nsCOMPtr<nsIChannel> channel;
    ASSERT_EQ(NS_OK, NS_NewChannel(getter_AddRefs(channel), uri, principal,
                                   nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL, nsIContentPolicy::TYPE_OTHER));
    Maybe<LoadInfoArgs> args;
    ASSERT_EQ(NS_OK, ipc::LoadInfoToLoadInfoArgs(channel->LoadInfo(), &args));
    ASSERT_TRUE(args.isSome());
    RefPtr<PcUdpProxy> proxy = PcUdpProxy::Lookup(*args);
    ASSERT_TRUE(SpinEventLoopUntil("StealthSessionUdpProxy"_ns,
                                   [&] { return proxy->GetKind() != PcUdpProxy::Kind::Resolving; }));
    PcUdpProxy::Socks5Info info;
    EXPECT_EQ(PcUdpProxy::Kind::Socks5, proxy->GetKind(&info))
        << "the PeerConnection of a document at " << origin.get() << " has no proxy while the session has a SOCKS5 one";
    EXPECT_TRUE(info.mHost.EqualsLiteral("192.0.2.7")) << origin.get();
    EXPECT_EQ(1080, info.mPort) << origin.get();
  }
}

// The OS handle of a socket with the SOCKS UDP layer is not given: what writes on it (neqo's own I/O) would go direct.
TEST(StealthSessionUdpProxy, ASocketWithTheLayerGivesNoOsHandle)
{
  nsCOMPtr<nsIProxyInfo> info = Socks5ProxyInfo("127.0.0.1", DeadPort());
  ASSERT_TRUE(info);
  nsCOMPtr<nsIUDPSocket> socket = do_CreateInstance("@mozilla.org/network/udp-socket;1");
  ASSERT_EQ(NS_OK, socket->SetSocksProxy(info));
  NetAddr local;
  local.raw.family = AF_INET;
  ASSERT_EQ(NS_OK, socket->InitWithAddress(&local, nullptr, false, 1));
  nsCOMPtr<nsIEventTarget> sts = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID);
  ASSERT_TRUE(sts);
  std::atomic<bool> done{false};
  std::atomic<int64_t> handle{0};
  ASSERT_EQ(NS_OK, sts->Dispatch(NS_NewRunnableFunction("GetFileDescriptor", [&] {
              handle = socket->GetFileDescriptor();
              done = true;
            })));
  ASSERT_TRUE(SpinEventLoopUntil("StealthSessionUdpProxy"_ns, [&] { return done.load(); }));
  EXPECT_EQ(-1, handle.load()) << "a socket with the SOCKS UDP layer gave its OS handle";
  socket->Close();
}
// RFC 1928 section 7: the destination of a datagram may be IPv6 whatever the family of the relay. An IPv6 socket sends an
// IPv6 destination through an IPv4 relay: the frame says ATYP 4, the datagram reaches the relay by its IPv4-mapped address.
TEST(StealthSessionUdpProxy, AnIpv6DestinationGoesThroughAnIpv4Relay)
{
  UdpSession prefs;
  FakeSocks proxy;
  prefs.Arm("127.0.0.1", proxy.Port());
  PRFileDesc* fd = PR_OpenUDPSocket(PR_AF_INET6);
  if (!fd) {
    GTEST_SKIP() << "no IPv6 socket on this host";
  }
  PRNetAddr any;
  PR_SetNetAddr(PR_IpAddrAny, PR_AF_INET6, 0, &any);
  ASSERT_EQ(PR_SUCCESS, PR_Bind(fd, &any));
  ASSERT_EQ(NS_OK, prefs.Attach(fd));
  PRNetAddr to;
  ASSERT_EQ(PR_SUCCESS, PR_StringToNetAddr("2001:db8::1", &to));
  to.ipv6.port = PR_htons(3478);
  ASSERT_EQ(4, PR_SendTo(fd, "ping", 4, 0, &to, PR_INTERVAL_NO_WAIT));
  nsCString got = proxy.FirstDatagram();
  ASSERT_EQ(got.Length(), 4u + 16u + 2u + 4u) << "the datagram of an IPv6 destination did not reach the IPv4 relay";
  EXPECT_EQ(0x04, static_cast<uint8_t>(got[3])) << "ATYP of an IPv6 destination";
  EXPECT_EQ(0, std::memcmp(got.get() + 22, "", 2)) << "port 3478";
  EXPECT_EQ(0, std::memcmp(got.get() + 24, "ping", 4));
  PR_Close(fd);
}

// A socket without the layer is no business of the layer: asking whether it has failed is an answer, not a crash.
TEST(StealthSessionUdpProxy, AskingAPlainSocketIfItHasFailedIsFalse)
{
  PRFileDesc* fd = PR_NewUDPSocket();
  ASSERT_TRUE(fd);
  EXPECT_FALSE(nsSOCKSUDPIOLayerHasFailed(fd));
  EXPECT_FALSE(nsSOCKSUDPIOLayerHasFailed(nullptr));
  PR_Close(fd);
}
// What the lookup says for each kind of proxy of the session: no proxy is direct; SOCKS5 carries UDP; every other kind (HTTP,
// SOCKS4) carries none, so the UDP of the PeerConnection goes nowhere (a router that drops it), never direct.
TEST(StealthSessionUdpProxy, TheKindOfProxyDecidesWhereTheUdpGoes)
{
  ProxyPrefsReset reset;
  auto KindOf = [&]() {
    nsCOMPtr<nsIURI> uri;
    EXPECT_EQ(NS_OK, NS_NewURI(getter_AddRefs(uri), "https://example.net/"_ns));
    nsCOMPtr<nsIChannel> channel;
    EXPECT_EQ(NS_OK, NS_NewChannel(getter_AddRefs(channel), uri, nsContentUtils::GetSystemPrincipal(),
                                   nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL, nsIContentPolicy::TYPE_OTHER));
    Maybe<LoadInfoArgs> args;
    EXPECT_EQ(NS_OK, ipc::LoadInfoToLoadInfoArgs(channel->LoadInfo(), &args));
    RefPtr<PcUdpProxy> proxy = PcUdpProxy::Lookup(*args);
    EXPECT_TRUE(SpinEventLoopUntil("StealthSessionUdpProxy"_ns,
                                   [&] { return proxy->GetKind() != PcUdpProxy::Kind::Resolving; }));
    return proxy->GetKind();
  };
  ASSERT_EQ(Preferences::SetInt("network.proxy.type", 0), NS_OK);
  EXPECT_EQ(PcUdpProxy::Kind::Direct, KindOf()) << "no proxy";

  ASSERT_EQ(Preferences::SetInt("network.proxy.type", 1), NS_OK);
  ASSERT_EQ(Preferences::SetCString("network.proxy.socks", "192.0.2.7"), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.socks_port", 1080), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.socks_version", 5), NS_OK);
  EXPECT_EQ(PcUdpProxy::Kind::Socks5, KindOf()) << "SOCKS5";

  ASSERT_EQ(Preferences::SetInt("network.proxy.socks_version", 4), NS_OK);
  EXPECT_EQ(PcUdpProxy::Kind::Blocked, KindOf()) << "SOCKS4 carries no UDP";

  ASSERT_EQ(Preferences::SetCString("network.proxy.socks", ""), NS_OK);
  ASSERT_EQ(Preferences::SetCString("network.proxy.http", "192.0.2.8"), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.http_port", 8080), NS_OK);
  EXPECT_EQ(PcUdpProxy::Kind::Blocked, KindOf()) << "an HTTP proxy carries no UDP";
}
}  // namespace mozilla::net
