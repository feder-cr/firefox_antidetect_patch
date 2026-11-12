/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsSOCKSUDPIOLayer.h"

#include <cstdint>
#include <cstring>
#include <inttypes.h>

#include <algorithm>

#ifdef XP_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

#include "nspr.h"
#include "private/pprio.h"
#include "mozilla/Atomics.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/Components.h"
#include "mozilla/Logging.h"
#include "mozilla/Mutex.h"
#include "mozilla/OriginAttributes.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/net/DNS.h"
#include "../protocol/http/nsHttpHandler.h"
#include "StealthPcUdpProxy.h"
#include "nsASocketHandler.h"
#include "nsICancelable.h"
#include "nsIProxyInfo.h"
#include "nsIDNSListener.h"
#include "nsIDNSRecord.h"
#include "nsIDNSService.h"
#include "nsISocketTransportService.h"
#include "nsIProxyInfo.h"
#include "nsProxyInfo.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"
#include "prnetdb.h"
#include "nsXULAppAPI.h"
#include "nsIOService.h"
#include "mozilla/net/SocketProcessParent.h"

namespace mozilla {
namespace net {

// Logging — visible with MOZ_LOG=SOCKS-UDP:5.
static LazyLogModule gSocksUdpLog("SOCKS-UDP");
#define LOG(args) MOZ_LOG(gSocksUdpLog, LogLevel::Debug, args)
#define LOGE(args) MOZ_LOG(gSocksUdpLog, LogLevel::Error, args)

namespace {
// The application has begun to shut down (what CaptivePortalService asks): from then on the pool is not made, kept or refilled.
bool ApplicationIsShuttingDown() { return AppShutdown::IsInOrBeyond(ShutdownPhase::AppShutdownConfirmed); }
}  // namespace

namespace {

PRDescIdentity gIdentity = PR_INVALID_IO_LAYER;

// Test only: the socket transport service is taken as missing (a browser that is shutting down), see Start().
Atomic<bool> sNoSocketThreadForTests{false};
PRIOMethods gMethods;

// ── The association ─────────────────────────────────────────────────────────
//
// The layer is put on the socket at once, and the association (the TCP control
// connection to the proxy, the greeting, the authentication, UDP ASSOCIATE) is
// made afterwards, on the socket thread, the way a Necko socket makes its own
// handshake (nsSOCKSIOLayer.cpp): the control connection is a non-blocking
// socket the socket transport service polls, each answer is read when it
// arrives, and the name of the proxy is resolved by the resolver, not by a
// call that waits. Nothing waits for the proxy, so the sockets of an ICE
// gather are made side by side and the first candidate is born at once. What
// is sent while the association is not made is held, a few packets, and goes
// to the relay when it is; it never goes direct.

constexpr size_t kMaxHeld = 16;       // datagrams held before the association
constexpr size_t kHeaderMax = 22;     // RSV(2) FRAG(1) ATYP(1) ADDR(16) PORT(2)
constexpr PRInt32 kPayloadMax = 2048;

// Pending: the proxy of the PeerConnection or the association is not known yet; Ready: the association is made;
// Direct: the PeerConnection has no proxy, the socket is a plain one; Failed: no association will come (a proxy that
// carries no UDP, or one that refused), and what is sent goes nowhere.
enum class State : uint8_t { Pending, Ready, Direct, Failed };

// A datagram held until it is known where it goes.
struct HeldDatagram {
  PRNetAddr mTo;
  nsTArray<uint8_t> mData;
};

size_t BuildFrame(const PRNetAddr* aTo, const void* aBuf, PRInt32 aAmount, uint8_t* aOut);

// The relay as this socket reaches it: the datagrams go to the relay, so the family that counts is the socket's (the
// destination of a datagram may be IPv6 or IPv4 whatever the relay, RFC 1928 section 7). An AF_INET6 socket reaches an IPv4
// relay by its IPv4-mapped address; an AF_INET socket does not reach an IPv6 relay (false).
bool RelayForSocket(const PRNetAddr& aRelay, PRUint16 aSocketFamily, PRNetAddr* aOut);

class SocksUdpAssociation;

// The proxy an association is made with, and the identity of a pool of them: what Firefox learns when it opens a SOCKS5
// connection (nsSOCKSIOLayer.cpp) and what the lookup of a PeerConnection answers are the same four strings.
struct ProxyKey {
  nsCString mHost;
  int32_t mPort = 0;
  nsCString mUser;
  nsCString mPass;
  bool operator==(const ProxyKey& aOther) const {
    return mPort == aOther.mPort && mHost == aOther.mHost && mUser == aOther.mUser && mPass == aOther.mPass;
  }
};

// The pool of associations (below the association): what the association tells it.
class SocksUdpPool {
 public:
  static already_AddRefed<SocksUdpAssociation> Take(const ProxyKey& aKey);
  static void Warm(const ProxyKey& aKey);
  static void Refused(const ProxyKey& aKey);
  static void Answered(const ProxyKey& aKey);
  // An association that had been made is over (the proxy closed its control connection): if it waited in the pool, it is replaced.
  static void Ended(const ProxyKey& aKey, SocksUdpAssociation* aEnded);
};


// What the layer on the socket and the association share: the state, the
// relay, and the datagrams held. Any thread; the layer is on whatever thread
// uses the socket, the association on the socket thread.
class SocksUdpShared final {
 public:
  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(SocksUdpShared)

  // The association is made: send what was held, then go on.
  void MakeReady(const PRNetAddr& aRelay) {
    MutexAutoLock lock(mMutex);
    if (!mLayer || mState != State::Pending) {
      mState = State::Failed;
      mHeld.Clear();
      return;
    }
    mRelay = aRelay;
    PRNetAddr relayForSocket;
    if (!RelayForSocket(aRelay, SocketFamilyLocked(), &relayForSocket)) {
      // An AF_INET socket cannot send to an IPv6 relay: this association is no road for it (what is sent goes nowhere).
      LOGE(("SOCKS UDP: an IPv6 relay for an IPv4 socket"));
      mState = State::Failed;
      mHeld.Clear();
      return;
    }
    for (const HeldDatagram& held : mHeld) {
      uint8_t frame[kHeaderMax + kPayloadMax];
      size_t hlen = BuildFrame(&held.mTo, held.mData.Elements(), held.mData.Length(), frame);
      // UDP: one that cannot be sent is lost, as any datagram can be.
      if (!hlen || mLayer->lower->methods->sendto(mLayer->lower, frame, hlen + held.mData.Length(), 0, &relayForSocket,
                                                  PR_INTERVAL_NO_WAIT) < 0) {
        LOGE(("SOCKS UDP: a held datagram was not sent (%d)", PR_GetError()));
      }
    }
    mHeld.Clear();
    mState = State::Ready;
  }

  // The PeerConnection has no proxy: the socket is a plain one, and what was held goes where it was sent.
  void MakeDirect() {
    MutexAutoLock lock(mMutex);
    if (!mLayer || mState != State::Pending) {
      return;
    }
    for (const HeldDatagram& held : mHeld) {
      (void)mLayer->lower->methods->sendto(mLayer->lower, held.mData.Elements(), held.mData.Length(), 0, &held.mTo,
                                           PR_INTERVAL_NO_WAIT);
    }
    mHeld.Clear();
    mState = State::Direct;
  }

  // The association failed or is over: what is sent from now on fails.
  void Fail() {
    MutexAutoLock lock(mMutex);
    mState = State::Failed;
    mHeld.Clear();
  }

  // The socket is being closed: nothing is sent through it any more, and the association it was given lets the proxy
  // go (defined after the association).
  void Detach();

  bool IsDetached() {
    MutexAutoLock lock(mMutex);
    return !mLayer;
  }

  // The association the pool gave this socket. False when the socket is already closed: the caller keeps it.
  bool SetAssociation(SocksUdpAssociation* aAssociation);

  // The family of the socket under the layer, asked once the socket is bound (an unbound socket has no name on Windows);
  // 0 until it can be told. The mutex is held by the caller.
  PRUint16 SocketFamilyLocked() {
    if (!mSocketFamily && mLayer && mLayer->lower) {
      PRNetAddr name;
      memset(&name, 0, sizeof(name));
      if (PR_GetSockName(mLayer->lower, &name) == PR_SUCCESS &&
          (name.raw.family == PR_AF_INET || name.raw.family == PR_AF_INET6)) {
        mSocketFamily = name.raw.family;
      }
    }
    return mSocketFamily;
  }

  Mutex mMutex{"SocksUdpShared"};
  State mState = State::Pending;
  PRNetAddr mRelay;
  PRUint16 mSocketFamily = 0;  // PR_AF_INET or PR_AF_INET6: the family of the socket the layer is on (0 = not known yet)
  PRFileDesc* mLayer = nullptr;  // the layer's fd; null once the socket closes
  nsTArray<HeldDatagram> mHeld;
  RefPtr<SocksUdpAssociation> mAssociation;  // the one this socket was given, to let go of it when the socket closes

 private:
  ~SocksUdpShared();
};

// The control connection of one socket, as a socket handler of the socket
// transport service.
class SocksUdpAssociation final : public nsASocketHandler,
                                  public nsIDNSListener {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIDNSLISTENER

  explicit SocksUdpAssociation(const ProxyKey& aKey)
      : mKey(aKey), mHost(aKey.mHost), mPort(static_cast<uint16_t>(aKey.mPort)), mUser(aKey.mUser), mPass(aKey.mPass) {}

  // Any thread: the socket this association was given to. If the association is already made the socket is ready at
  // once; if it is being made, when it is; if it failed, the socket's sends fail.
  void Bind(SocksUdpShared* aShared) {
    State state;
    PRNetAddr relay;
    {
      MutexAutoLock lock(mMutex);
      mShared = aShared;
      state = mState;
      relay = mRelay;
    }
    if (state == State::Ready) {
      aShared->MakeReady(relay);
    } else if (state == State::Failed) {
      aShared->Fail();
    }
  }

  // Any thread: it was handed out, or made, and the control connection is still there.
  bool IsFailed() {
    MutexAutoLock lock(mMutex);
    return mState == State::Failed;
  }

  // Any thread: the proxy answered the ASSOCIATE and the control connection is still there: a socket given this sends at once.
  bool IsReady() {
    MutexAutoLock lock(mMutex);
    return mState == State::Ready;
  }

  // Any thread: begin, on the socket thread.
  //
  // The pool calls this with its lock held: a start that cannot be made fails the association WITHOUT telling the pool
  // (Failed(false)): the proxy refused nothing, there is no socket thread, and the pool's lock is not recursive.
  void Start() {
    nsCOMPtr<nsIEventTarget> sts;
    if (!sNoSocketThreadForTests) {
      sts = do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID);
    }
    nsCOMPtr<nsIRunnable> begin = NewRunnableMethod(
        "SocksUdpAssociation::Begin", this, &SocksUdpAssociation::Begin);
    if (!sts || NS_FAILED(sts->Dispatch(begin.forget(), NS_DISPATCH_NORMAL))) {
      LOGE(("SOCKS UDP association: no socket thread"));
      Failed(false);
    }
  }

  // Any thread: the socket is closing; let the proxy go.
  void Cancel() {
    mCancelled = true;
    nsCOMPtr<nsIEventTarget> sts =
        do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID);
    nsCOMPtr<nsIRunnable> cancel = NewRunnableMethod(
        "SocksUdpAssociation::DoCancel", this, &SocksUdpAssociation::DoCancel);
    if (sts) {
      (void)sts->Dispatch(cancel.forget(), NS_DISPATCH_NORMAL);
    }
  }

  // nsASocketHandler (socket thread)
  void OnSocketReady(PRFileDesc* aFd, int16_t aOutFlags) override {
    if (NS_FAILED(mCondition) || mStep == Step::Idle) {
      return;
    }
    if (aOutFlags == -1) {
      if (mStep == Step::Connecting && mRecord) {
        mRetry = true;
        mCondition = NS_ERROR_ABORT;
        return;
      }
      Fail("timeout");
      return;
    }
    if (mStep == Step::Connecting) {
      PRPollDesc pd = {aFd, PR_POLL_WRITE, aOutFlags};
      if (PR_GetConnectStatus(&pd) != PR_SUCCESS) {
        if (PR_GetError() == PR_IN_PROGRESS_ERROR) {
          return;  // not yet
        }
        if (mRecord) {
          mRetry = true;  // OnSocketDetached goes on with the next address
          mCondition = NS_ERROR_ABORT;
          return;
        }
        Fail("connect");
        return;
      }
      QueueGreeting();
    } else if (mStep == Step::Associated) {
      WatchControl();
      return;
    } else if (aOutFlags & (PR_POLL_ERR | PR_POLL_NVAL)) {
      Fail("socket error");
      return;
    }
    Pump();
  }

  void OnSocketDetached(PRFileDesc* aFd) override {
    if (mFd) {
      PR_Close(mFd);
      mFd = nullptr;
    }
    mAttached = false;
    if (mRetry) {
      // The connection to this address failed and the name has another: try it (after the service is done with us).
      mRetry = false;
      mCondition = NS_OK;
      mStep = Step::Idle;
      nsCOMPtr<nsIRunnable> next = NewRunnableMethod(
          "SocksUdpAssociation::ConnectNext", this, &SocksUdpAssociation::ConnectNext);
      if (NS_SUCCEEDED(NS_DispatchToCurrentThread(next.forget()))) {
        return;
      }
    }
    // The control connection is closed, so the proxy ends the association (RFC 1928 section 6): whatever the socket
    // was doing, and whatever made the service let go of it (an error, the shutdown, the browser going offline).
    Failed();
  }

  void IsLocal(bool* aIsLocal) override { *aIsLocal = false; }
  uint64_t ByteCountSent() override { return 0; }
  uint64_t ByteCountReceived() override { return 0; }

 private:
  ~SocksUdpAssociation() = default;

  enum class Step : uint8_t {
    Idle,
    Resolving,
    Connecting,
    SendGreeting,
    ReadGreeting,
    SendAuth,
    ReadAuth,
    SendAssociate,
    ReadAssociateHead,
    ReadAssociateTail,
    Associated,
  };

  // Socket thread.
  void Begin() {
    if (mCancelled) {
      Failed();
      return;
    }
    PRNetAddr addr;
    if (PR_StringToNetAddr(mHost.get(), &addr) == PR_SUCCESS) {
      // An address, not a name: nothing to resolve.
      if (addr.raw.family == PR_AF_INET) {
        addr.inet.port = PR_htons(mPort);
      } else {
        addr.ipv6.port = PR_htons(mPort);
      }
      Connect(addr);
      return;
    }
    // A name: the resolver, the way Necko resolves the proxy of a TCP
    // connection. The flag is the one that says the name is the proxy's own,
    // which a session behind a proxy that resolves names may still look up.
    nsCOMPtr<nsIDNSService> dns = mozilla::components::DNS::Service();
    if (!dns) {
      Fail("no resolver");
      return;
    }
    mStep = Step::Resolving;
    OriginAttributes attrs;
    nsresult rv = dns->AsyncResolveNative(
        mHost, nsIDNSService::RESOLVE_TYPE_DEFAULT,
        nsIDNSService::RESOLVE_IGNORE_SOCKS_DNS, nullptr, this,
        GetCurrentSerialEventTarget(), attrs, getter_AddRefs(mLookup));
    if (NS_FAILED(rv)) {
      Fail("resolve");
    }
  }

  void ConnectNext();

  void DoCancel() {
    if (mLookup) {
      mLookup->Cancel(NS_ERROR_ABORT);
      mLookup = nullptr;
    }
    if (mAttached) {
      mCondition = NS_BINDING_ABORTED;  // the service detaches us
    } else if (mFd) {
      PR_Close(mFd);
      mFd = nullptr;
    }
    mStep = Step::Idle;
  }

  void Connect(const PRNetAddr& aAddr) {
    mFd = PR_OpenTCPSocket(aAddr.raw.family);
    if (!mFd) {
      Fail("socket");
      return;
    }
    PRSocketOptionData opt;
    opt.option = PR_SockOpt_Nonblocking;
    opt.value.non_blocking = PR_TRUE;
    if (PR_SetSocketOption(mFd, &opt) != PR_SUCCESS) {
      Fail("nonblocking");
      return;
    }
    mStep = Step::Connecting;
    if (PR_Connect(mFd, &aAddr, PR_INTERVAL_NO_WAIT) != PR_SUCCESS) {
      PRErrorCode err = PR_GetError();
      if (err != PR_IN_PROGRESS_ERROR && err != PR_WOULD_BLOCK_ERROR) {
        if (mRecord) {  // the next address of the name, if there is one
          PR_Close(mFd);
          mFd = nullptr;
          ConnectNext();
          return;
        }
        Fail("connect");
        return;
      }
    }
    mPollFlags = PR_POLL_WRITE | PR_POLL_EXCEPT;
    // The time Necko gives a connection to be made, the same for the connection of a page through a SOCKS proxy
    // (network.http.connection-timeout, ConnectionAttemptPool.cpp): a proxy that does not answer ends the association
    // instead of keeping the socket waiting for ever. None once the association is made.
    uint32_t seconds = gHttpHandler ? gHttpHandler->ConnectTimeout() / PR_MSEC_PER_SEC : 0;
    mPollTimeout = (seconds && seconds < UINT16_MAX) ? static_cast<uint16_t>(seconds) : UINT16_MAX;
    nsCOMPtr<nsISocketTransportService> sts =
        do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID);
    if (!sts || NS_FAILED(sts->AttachSocket(mFd, this))) {
      Fail("attach");
      return;
    }
    mAttached = true;
  }

  // Socket thread. The association is over, whatever it was doing.
  void Fail(const char* aWhy) {
    LOGE(("SOCKS UDP association failed: %s", aWhy));
    Failed();
    mStep = Step::Idle;
    if (mAttached) {
      mCondition = NS_ERROR_ABORT;  // the service detaches us, which closes
    } else if (mFd) {
      PR_Close(mFd);
      mFd = nullptr;
    }
  }

  void QueueGreeting() {
    bool wantAuth = !mUser.IsEmpty() && !mPass.IsEmpty();
    mOut.Clear();
    mOut.AppendElement(0x05);  // VER
    mOut.AppendElement(wantAuth ? 2 : 1);  // NMETHODS
    mOut.AppendElement(0x00);  // no authentication
    if (wantAuth) {
      mOut.AppendElement(0x02);  // user/password
    }
    mOutPos = 0;
    mStep = Step::SendGreeting;
  }

  bool QueueAuth() {
    if (mUser.Length() > 255 || mPass.Length() > 255) {
      return false;
    }
    mOut.Clear();
    mOut.AppendElement(0x01);  // VER of the subnegotiation
    mOut.AppendElement(static_cast<uint8_t>(mUser.Length()));
    mOut.AppendElements(mUser.get(), mUser.Length());
    mOut.AppendElement(static_cast<uint8_t>(mPass.Length()));
    mOut.AppendElements(mPass.get(), mPass.Length());
    mOutPos = 0;
    mStep = Step::SendAuth;
    return true;
  }

  void QueueAssociate() {
    // The client's address is not known to the socket: 0.0.0.0:0, which
    // RFC 1928 section 6 allows.
    static const uint8_t kAssociate[10] = {0x05, 0x03, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    mOut.Clear();
    mOut.AppendElements(kAssociate, sizeof(kAssociate));
    mOutPos = 0;
    mStep = Step::SendAssociate;
  }

  // What to read next, once what was queued is sent.
  void AfterSent() {
    mIn.Clear();
    switch (mStep) {
      case Step::SendGreeting:
        mStep = Step::ReadGreeting;
        mNeed = 2;
        break;
      case Step::SendAuth:
        mStep = Step::ReadAuth;
        mNeed = 2;
        break;
      default:
        mStep = Step::ReadAssociateHead;
        mNeed = 4;
        break;
    }
  }

  // The bytes wanted are in: take the next step. False when it failed.
  bool Parse() {
    switch (mStep) {
      case Step::ReadGreeting: {
        if (mIn[0] != 0x05) {
          Fail("greeting: not SOCKS5");
          return false;
        }
        mNeed = 0;
        if (mIn[1] == 0x00) {
          QueueAssociate();
        } else if (mIn[1] == 0x02 && !mUser.IsEmpty() && !mPass.IsEmpty()) {
          if (!QueueAuth()) {
            Fail("authentication: too long");
            return false;
          }
        } else {
          Fail("greeting: no acceptable method");
          return false;
        }
        return true;
      }
      case Step::ReadAuth:
        mNeed = 0;
        if (mIn[1] != 0x00) {
          Fail("authentication refused");
          return false;
        }
        QueueAssociate();
        return true;
      case Step::ReadAssociateHead:
        if (mIn[0] != 0x05 || mIn[1] != 0x00) {
          LOGE(("UDP ASSOCIATE rejected ver=%02x rep=%02x", mIn[0], mIn[1]));
          Fail("UDP ASSOCIATE rejected");
          return false;
        }
        if (mIn[3] == 0x01) {
          mNeed = 4 + 4 + 2;
        } else if (mIn[3] == 0x04) {
          mNeed = 4 + 16 + 2;
        } else {
          Fail("UDP ASSOCIATE: relay address not supported");
          return false;
        }
        mStep = Step::ReadAssociateTail;
        return true;
      default: {  // ReadAssociateTail
        PRNetAddr relay;
        memset(&relay, 0, sizeof(relay));
        uint16_t bndPort = 0;
        if (mIn[3] == 0x01) {
          relay.inet.family = PR_AF_INET;
          memcpy(&relay.inet.ip, &mIn[4], 4);
          memcpy(&bndPort, &mIn[8], 2);
          relay.inet.port = bndPort;
        } else {
          relay.ipv6.family = PR_AF_INET6;
          memcpy(&relay.ipv6.ip, &mIn[4], 16);
          memcpy(&bndPort, &mIn[20], 2);
          relay.ipv6.port = bndPort;
        }
        if (!bndPort) {
          Fail("UDP ASSOCIATE: relay port 0");
          return false;
        }
        if (PR_IsNetAddrType(&relay, PR_IpAddrAny)) {
          // The proxy says "the address you reached me by".
          PRNetAddr peer;
          if (PR_GetPeerName(mFd, &peer) != PR_SUCCESS) {
            Fail("UDP ASSOCIATE: no address for the relay");
            return false;
          }
          relay = peer;
          if (relay.raw.family == PR_AF_INET) {
            relay.inet.port = bndPort;
          } else {
            relay.ipv6.port = bndPort;
          }
        }
        mNeed = 0;
        mStep = Step::Associated;
        mPollTimeout = UINT16_MAX;  // the control connection is idle for as long as the relay lives
        Ready(relay);
        LOG(("UDP ASSOCIATE OK"));
        return true;
      }
    }
  }

  // Send what is queued, read what is wanted, as far as the socket lets us;
  // then ask the service to call back for the rest.
  void Pump() {
    for (;;) {
      if (mOutPos < mOut.Length()) {
        int32_t n = PR_Send(mFd, mOut.Elements() + mOutPos,
                            mOut.Length() - mOutPos, 0, PR_INTERVAL_NO_WAIT);
        if (n < 0) {
          if (PR_GetError() == PR_WOULD_BLOCK_ERROR) {
            mPollFlags = PR_POLL_WRITE;
            return;
          }
          Fail("send");
          return;
        }
        mOutPos += n;
        if (mOutPos < mOut.Length()) {
          mPollFlags = PR_POLL_WRITE;
          return;
        }
        mOut.Clear();
        mOutPos = 0;
        AfterSent();
        continue;
      }
      if (mStep == Step::Associated) {
        mPollFlags = PR_POLL_READ;  // the control connection is the relay's life
        return;
      }
      if (mNeed == 0) {
        return;
      }
      while (mIn.Length() < mNeed) {
        uint8_t buf[32];
        size_t want = std::min(mNeed - mIn.Length(), sizeof(buf));
        int32_t n = PR_Recv(mFd, buf, want, 0, PR_INTERVAL_NO_WAIT);
        if (n < 0) {
          if (PR_GetError() == PR_WOULD_BLOCK_ERROR) {
            mPollFlags = PR_POLL_READ;
            return;
          }
          Fail("receive");
          return;
        }
        if (n == 0) {
          Fail("the proxy closed the connection");
          return;
        }
        mIn.AppendElements(buf, n);
      }
      if (!Parse()) {
        return;
      }
    }
  }

  // Associated: the proxy ends the relay by closing the control connection.
  void WatchControl() {
    uint8_t b;
    int32_t n = PR_Recv(mFd, &b, 1, 0, PR_INTERVAL_NO_WAIT);
    if (n == 0 || (n < 0 && PR_GetError() != PR_WOULD_BLOCK_ERROR)) {
      Fail("the proxy ended the association");
    }
  }

  // The association is made (any thread of the socket service): the socket it was given to, if there is one, is ready.
  void Ready(const PRNetAddr& aRelay);
  // The association is over, made or not (see the pool for what a refusal means).
  // aProxyRefused: the proxy did not make it (told to the pool); false when it was never asked.
  void Failed(bool aProxyRefused = true);

  const ProxyKey mKey;
  Mutex mMutex{"SocksUdpAssociation"};
  State mState = State::Pending;  // Pending | Ready | Failed
  PRNetAddr mRelay;
  RefPtr<SocksUdpShared> mShared;  // null while it waits in the pool
  nsCString mHost;
  uint16_t mPort = 0;
  nsCString mUser;
  nsCString mPass;
  PRFileDesc* mFd = nullptr;
  nsCOMPtr<nsICancelable> mLookup;
  nsCOMPtr<nsIDNSAddrRecord> mRecord;  // the addresses of the proxy's name
  bool mRetry = false;
  Step mStep = Step::Idle;
  bool mAttached = false;
  Atomic<bool> mCancelled{false};
  nsTArray<uint8_t> mOut;
  size_t mOutPos = 0;
  nsTArray<uint8_t> mIn;
  size_t mNeed = 0;
};

NS_IMPL_ISUPPORTS(SocksUdpAssociation, nsIDNSListener)

NS_IMETHODIMP
SocksUdpAssociation::OnLookupComplete(nsICancelable*, nsIDNSRecord* aRecord,
                                      nsresult aStatus) {
  mLookup = nullptr;
  if (mCancelled || mStep != Step::Resolving) {
    return NS_OK;
  }
  nsCOMPtr<nsIDNSAddrRecord> record = do_QueryInterface(aRecord);
  NetAddr netAddr;
  if (NS_FAILED(aStatus) || !record ||
      NS_FAILED(record->GetNextAddr(mPort, &netAddr))) {
    Fail("the proxy's name did not resolve");
    return NS_OK;
  }
  PRNetAddr addr;
  NetAddrToPRNetAddr(&netAddr, &addr);
  mRecord = record;
  Connect(addr);
  return NS_OK;
}

// Socket thread: the connection to an address of the proxy's name failed; the next one, as nsSocketTransport does.
void SocksUdpAssociation::ConnectNext() {
  NetAddr netAddr;
  if (mCancelled || !mRecord || NS_FAILED(mRecord->GetNextAddr(mPort, &netAddr))) {
    Fail("no address of the proxy answered");
    return;
  }
  PRNetAddr addr;
  NetAddrToPRNetAddr(&netAddr, &addr);
  Connect(addr);
}

// ── The association's two ends, and the pool ────────────────────────────────

void SocksUdpAssociation::Ready(const PRNetAddr& aRelay) {
  RefPtr<SocksUdpShared> shared;
  {
    MutexAutoLock lock(mMutex);
    if (mState != State::Pending) {
      return;
    }
    mState = State::Ready;
    mRelay = aRelay;
    shared = mShared;
  }
  SocksUdpPool::Answered(mKey);
  if (shared) {
    shared->MakeReady(aRelay);
  }
}

void SocksUdpAssociation::Failed(bool aProxyRefused) {
  RefPtr<SocksUdpShared> shared;
  State before;
  {
    MutexAutoLock lock(mMutex);
    before = mState;
    mState = State::Failed;
    shared = mShared;
  }
  if (shared) {
    shared->Fail();
  }
  // A proxy that did not make the association (refused, did not answer, wrong password) is told to the pool; one that
  // ended an association it had made, or a socket that let go of it, is not a refusal.
  if (aProxyRefused && before == State::Pending && !mCancelled) {
    SocksUdpPool::Refused(mKey);
  }
  // One that had been made and is over: if it was waiting in the pool for a socket, the pool replaces it NOW, unseen, and does not
  // wait for the next socket to find it dead. Only a made one, and not one the pool itself cancelled: the pool's lock is not held here
  // (a start that fails under the lock passes aProxyRefused=false and was never made).
  if (before == State::Ready && !mCancelled) {
    SocksUdpPool::Ended(mKey, this);
  }
}

SocksUdpShared::~SocksUdpShared() = default;

void SocksUdpShared::Detach() {
  RefPtr<SocksUdpAssociation> association;
  {
    MutexAutoLock lock(mMutex);
    mLayer = nullptr;
    mState = State::Failed;
    mHeld.Clear();
    association = std::move(mAssociation);
  }
  if (association) {
    association->Cancel();
  }
}

bool SocksUdpShared::SetAssociation(SocksUdpAssociation* aAssociation) {
  MutexAutoLock lock(mMutex);
  if (!mLayer) {
    return false;
  }
  mAssociation = aAssociation;
  return true;
}

// The associations a context's sockets will need, made when Necko opens the first SOCKS5 connection of the context (the
// moment Firefox learns the proxy), so a socket takes one that is made, or being made, instead of paying the round trips
// of the handshake itself. Take is the only place that asks a proxy for an association of its own accord; a taken one
// is replaced, for the proxies that were warmed. Any thread.
// How many associations a proxy keeps ready. Measured (captures/udp-pool-size/ of the study repo invisible_WebRTC; the engine candidate of
// 12/11, one declared IPv4 interface, 2 runs of each case, equal): the UDP sockets ONE PeerConnection holds at once while it gathers are
//   data channel only .......................... 1
//   one audio m-section, or one video m-section ... 2
//   audio + data channel ....................... 3
//   audio + video .............................. 4
//   audio + video + data channel ............... 5
// with and without ICE servers. TWO are kept, not five, on purpose: the warm-up starts at the first TCP connection of EVERY context
// that has a proxy, whether or not the page ever uses WebRTC, and each ready association is an authenticated connection held open at
// the proxy, for the context and for the process (a context per fetch means that many more connections to the proxy's provider for
// every fetch). Two cover what a page that probes WebRTC does: a data channel (1 socket) and a first audio m-section. A whole
// audio + video + data PeerConnection has its third socket and the ones after it wait for the rest of the handshake - a known limit
// (docs/KNOWN-ISSUES.md). The pool refills on every take, so the sockets after the first two find the pool refilling, never empty
// for good, and no socket ever fails for want of a ready one: it makes its own association.
constexpr size_t kPoolReady = 2;
constexpr size_t kPoolKeys = 16;  // proxies kept; the one used longest ago is let go of
// A proxy that ends every association it makes (ready, then closed) is not replaced for ever: after this many replacements with no
// socket having taken one the pool stops, and the next socket finds it short (and makes its own, and the pool starts again).
constexpr uint32_t kPoolReplacementsWithoutUse = 8;

struct PoolEntry {
  ProxyKey mKey;
  nsTArray<RefPtr<SocksUdpAssociation>> mIdle;
  bool mWarmed = false;   // a SOCKS5 connection of this proxy was opened (or the parent said so): keep kPoolReady ready
  bool mRefused = false;  // the last association asked of this proxy was not made: warming does not ask again
  uint32_t mReplaced = 0;  // associations replaced since a socket last took one (kPoolReplacementsWithoutUse)
};

StaticMutex sPoolMutex;
StaticAutoPtr<nsTArray<PoolEntry>> sPool MOZ_GUARDED_BY(sPoolMutex);
// Set by the shutdown hook, under the lock: from then on nobody makes the pool again, finds an entry, or replaces anything.
bool sPoolShutDown MOZ_GUARDED_BY(sPoolMutex) = false;

// The pool is let go of at XPCOM shutdown UNDER ITS LOCK: the socket service thread closes the control connections of the associations that
// wait in it at that very moment, and the one that ends tells the pool (SocksUdpPool::Ended), which reads and changes it. ClearOnShutdown
// would have reset the pointer on the main thread with no lock. The array is moved out under the lock and destroyed outside it.
void ShutDownPool() {
  nsTArray<PoolEntry> doomed;
  {
    StaticMutexAutoLock lock(sPoolMutex);
    sPoolShutDown = true;
    if (sPool) {
      doomed = std::move(*sPool);
      sPool = nullptr;
    }
  }
  for (PoolEntry& entry : doomed) {
    for (const RefPtr<SocksUdpAssociation>& idle : entry.mIdle) {
      idle->Cancel();
    }
  }
}

// Main thread, once, when the pool is first made.
void RegisterPoolShutdown() {
  if (NS_IsMainThread()) {
    if (!ApplicationIsShuttingDown()) {
      RunOnShutdown([] { ShutDownPool(); }, ShutdownPhase::XPCOMShutdown);
    }
    return;
  }
  NS_DispatchToMainThread(NS_NewRunnableFunction("SocksUdpPool::RegisterShutdown", [] {
    if (!ApplicationIsShuttingDown()) {
      RunOnShutdown([] { ShutDownPool(); }, ShutdownPhase::XPCOMShutdown);
    }
  }));
}

// The entry of a proxy, the most recently used last; the ones pushed out are returned to be let go of outside the lock. Null once the pool
// is shut down or the application is: nothing is kept then, a socket makes its own association.
PoolEntry* PoolEntryFor(const ProxyKey& aKey, nsTArray<RefPtr<SocksUdpAssociation>>& aEvicted) MOZ_REQUIRES(sPoolMutex) {
  if (sPoolShutDown || ApplicationIsShuttingDown()) {
    return nullptr;
  }
  if (!sPool) {
    sPool = new nsTArray<PoolEntry>();
    RegisterPoolShutdown();
  }
  for (size_t i = 0; i < sPool->Length(); ++i) {
    if ((*sPool)[i].mKey == aKey) {
      if (i + 1 != sPool->Length()) {
        PoolEntry moved = std::move((*sPool)[i]);
        sPool->RemoveElementAt(i);
        sPool->AppendElement(std::move(moved));
      }
      return &sPool->LastElement();
    }
  }
  PoolEntry& entry = *sPool->AppendElement();
  entry.mKey = aKey;
  while (sPool->Length() > kPoolKeys) {
    aEvicted.AppendElements(std::move((*sPool)[0].mIdle));
    sPool->RemoveElementAt(0);
  }
  return &sPool->LastElement();
}

PoolEntry* PoolFind(const ProxyKey& aKey) MOZ_REQUIRES(sPoolMutex) {
  if (sPool) {
    for (PoolEntry& entry : *sPool) {
      if (entry.mKey == aKey) {
        return &entry;
      }
    }
  }
  return nullptr;
}

// Keep kPoolReady ready: the ones the proxy ended are dropped first.
void PoolFill(PoolEntry& aEntry) MOZ_REQUIRES(sPoolMutex) {
  aEntry.mIdle.RemoveElementsBy([](const RefPtr<SocksUdpAssociation>& a) { return a->IsFailed(); });
  while (aEntry.mIdle.Length() < kPoolReady) {
    RefPtr<SocksUdpAssociation> made = new SocksUdpAssociation(aEntry.mKey);
    made->Start();
    aEntry.mIdle.AppendElement(std::move(made));
  }
}


already_AddRefed<SocksUdpAssociation> SocksUdpPool::Take(const ProxyKey& aKey) {
  RefPtr<SocksUdpAssociation> taken;
  nsTArray<RefPtr<SocksUdpAssociation>> evicted;
  {
    StaticMutexAutoLock lock(sPoolMutex);
    PoolEntry* found = PoolEntryFor(aKey, evicted);
    if (!found) {
      // Shutting down: no pool; the socket makes its own association and nothing is kept.
      taken = new SocksUdpAssociation(aKey);
      taken->Start();
      return taken.forget();
    }
    PoolEntry& entry = *found;
    entry.mIdle.RemoveElementsBy([](const RefPtr<SocksUdpAssociation>& a) { return a->IsFailed(); });  // the proxy may have ended it
    // A ready one first (it sends at once), else the one that has been waiting longest.
    size_t chosen = entry.mIdle.Length();
    for (size_t i = 0; i < entry.mIdle.Length(); ++i) {
      if (entry.mIdle[i]->IsReady()) {
        chosen = i;
        break;
      }
    }
    if (chosen == entry.mIdle.Length() && !entry.mIdle.IsEmpty()) {
      chosen = 0;
    }
    if (chosen < entry.mIdle.Length()) {
      taken = std::move(entry.mIdle[chosen]);
      entry.mIdle.RemoveElementAt(chosen);
    } else {
      taken = new SocksUdpAssociation(aKey);
      taken->Start();
    }
    // A proxy that was warmed keeps its associations ready: the one taken is replaced. A socket took one, so the replacements start over.
    entry.mReplaced = 0;
    if (entry.mWarmed && !entry.mRefused) {
      PoolFill(entry);
    }
  }
  for (const RefPtr<SocksUdpAssociation>& gone : evicted) {
    gone->Cancel();
  }
  return taken.forget();
}

void SocksUdpPool::Warm(const ProxyKey& aKey) {
  nsTArray<RefPtr<SocksUdpAssociation>> evicted;
  {
    StaticMutexAutoLock lock(sPoolMutex);
    PoolEntry* entry = PoolEntryFor(aKey, evicted);
    if (!entry) {
      return;
    }
    entry->mWarmed = true;
    if (!entry->mRefused) {
      PoolFill(*entry);
    }
  }
  for (const RefPtr<SocksUdpAssociation>& gone : evicted) {
    gone->Cancel();
  }
}

void SocksUdpPool::Ended(const ProxyKey& aKey, SocksUdpAssociation* aEnded) {
  StaticMutexAutoLock lock(sPoolMutex);
  // Shutting down the associations close on their own and nothing is replaced: the pool may be gone or going.
  if (sPoolShutDown || ApplicationIsShuttingDown()) {
    return;
  }
  PoolEntry* entry = PoolFind(aKey);
  if (!entry) {
    return;
  }
  // Only one that was WAITING here is replaced; one a socket holds is the socket's.
  if (!entry->mIdle.RemoveElement(aEnded)) {
    return;
  }
  if (entry->mWarmed && !entry->mRefused && entry->mReplaced < kPoolReplacementsWithoutUse) {
    ++entry->mReplaced;
    PoolFill(*entry);
  }
}

// What the proxy did with an association asked of it; a proxy the pool does not hold (it was pushed out) is not made one.
void SocksUdpPool::Refused(const ProxyKey& aKey) {
  StaticMutexAutoLock lock(sPoolMutex);
  if (PoolEntry* entry = PoolFind(aKey)) {
    entry->mRefused = true;
  }
}

void SocksUdpPool::Answered(const ProxyKey& aKey) {
  StaticMutexAutoLock lock(sPoolMutex);
  if (PoolEntry* entry = PoolFind(aKey)) {
    entry->mRefused = false;
  }
}

// Any thread: the PeerConnection's proxy is known (or will be): give the socket its association, or its plain socket, or
// its refusal.
void BindSocket(SocksUdpShared* aShared, PcUdpProxy* aProxy) {
  PcUdpProxy::Socks5Info info;
  switch (aProxy->GetKind(&info)) {
    case PcUdpProxy::Kind::Resolving: {
      // The lookup of the PeerConnection's proxy is not done: come back when it is. The sockets are made meanwhile.
      RefPtr<SocksUdpShared> shared = aShared;
      RefPtr<PcUdpProxy> proxy = aProxy;
      aProxy->WhenKnown([shared, proxy] { BindSocket(shared, proxy); });
      return;
    }
    case PcUdpProxy::Kind::Direct:
      aShared->MakeDirect();
      return;
    case PcUdpProxy::Kind::Blocked:
      aShared->Fail();
      return;
    case PcUdpProxy::Kind::Socks5:
      break;
  }
  ProxyKey key;
  key.mHost = info.mHost;
  key.mPort = info.mPort;
  key.mUser = info.mUser;
  key.mPass = info.mPass;
  if (aShared->IsDetached()) {
    return;  // the socket was closed meanwhile: it does not use one up
  }
  RefPtr<SocksUdpAssociation> association = SocksUdpPool::Take(key);
  if (!aShared->SetAssociation(association)) {
    association->Cancel();
    return;
  }
  association->Bind(aShared);
}

// What the layer keeps on the socket.
struct SocksUdpLayerData {
  RefPtr<SocksUdpShared> mShared;
};

SocksUdpShared* SharedOf(PRFileDesc* fd) {
  if (!fd) {
    return nullptr;  // no layer on the socket (PR_GetIdentitiesLayer found none)
  }
  auto* data = reinterpret_cast<SocksUdpLayerData*>(fd->secret);
  return data ? data->mShared.get() : nullptr;
}

// True for 127.0.0.0/8 and ::1 (also as an IPv4-mapped address): a destination that does not leave the machine. Firefox sends
// to localhost direct whatever the proxy; the layer does the same, so two PeerConnections of one page reach each other.
bool IsLoopbackAddr(const PRNetAddr& aAddr) {
  if (aAddr.raw.family == PR_AF_INET) {
    return (PR_ntohl(aAddr.inet.ip) >> 24) == 127;
  }
  if (aAddr.raw.family == PR_AF_INET6) {
    return PR_IsNetAddrType(&aAddr, PR_IpAddrLoopback) ||
           (PR_IsNetAddrType(&aAddr, PR_IpAddrV4Mapped) && (aAddr.ipv6.ip.pr_s6_addr[12] == 127));
  }
  return false;
}

// True when a datagram comes from the relay (address and port).
bool FromTheRelay(const PRNetAddr& aRelay, const PRNetAddr& aSource) {
  if (aRelay.raw.family == PR_AF_INET && aSource.raw.family == PR_AF_INET) {
    return aRelay.inet.ip == aSource.inet.ip && aRelay.inet.port == aSource.inet.port;
  }
  if (aRelay.raw.family == PR_AF_INET && aSource.raw.family == PR_AF_INET6) {
    // An AF_INET6 socket sees an IPv4 relay by its IPv4-mapped address.
    return PR_IsNetAddrType(&aSource, PR_IpAddrV4Mapped) && !memcmp(aSource.ipv6.ip.pr_s6_addr + 12, &aRelay.inet.ip, 4) &&
           aSource.ipv6.port == aRelay.inet.port;
  }
  if (aRelay.raw.family == PR_AF_INET6 && aSource.raw.family == PR_AF_INET6) {
    return !memcmp(&aRelay.ipv6.ip, &aSource.ipv6.ip, 16) && aRelay.ipv6.port == aSource.ipv6.port;
  }
  return false;
}

bool RelayForSocket(const PRNetAddr& aRelay, PRUint16 aSocketFamily, PRNetAddr* aOut) {
  *aOut = aRelay;
  if (!aSocketFamily || aSocketFamily == aRelay.raw.family) {
    return true;
  }
  if (aSocketFamily == PR_AF_INET6 && aRelay.raw.family == PR_AF_INET) {
    memset(aOut, 0, sizeof(*aOut));
    aOut->ipv6.family = PR_AF_INET6;
    aOut->ipv6.port = aRelay.inet.port;
    PR_ConvertIPv4AddrToIPv6(aRelay.inet.ip, &aOut->ipv6.ip);
    return true;
  }
  return false;
}

// The datagram as the relay wants it: [RSV(2) FRAG(1) ATYP ADDR PORT DATA].
// Returns the length of the header (0 with the NSPR error set when it cannot
// be built); the datagram is aOut[0 .. header + aAmount).
size_t BuildFrame(const PRNetAddr* aTo, const void* aBuf, PRInt32 aAmount,
                  uint8_t* aOut) {
  if (aAmount < 0 || aAmount > kPayloadMax) {
    PR_SetError(PR_BUFFER_OVERFLOW_ERROR, 0);
    return 0;
  }
  size_t hlen = 0;
  aOut[hlen++] = 0x00;  // RSV
  aOut[hlen++] = 0x00;  // RSV
  aOut[hlen++] = 0x00;  // FRAG (no fragmentation)
  if (aTo->raw.family == PR_AF_INET) {
    aOut[hlen++] = 0x01;  // ATYP IPv4
    memcpy(aOut + hlen, &aTo->inet.ip, 4);
    hlen += 4;
    memcpy(aOut + hlen, &aTo->inet.port, 2);
    hlen += 2;
  } else if (aTo->raw.family == PR_AF_INET6) {
    aOut[hlen++] = 0x04;  // ATYP IPv6
    memcpy(aOut + hlen, &aTo->ipv6.ip, 16);
    hlen += 16;
    memcpy(aOut + hlen, &aTo->ipv6.port, 2);
    hlen += 2;
  } else {
    PR_SetError(PR_ADDRESS_NOT_SUPPORTED_ERROR, 0);
    return 0;
  }
  memcpy(aOut + hlen, aBuf, aAmount);
  return hlen;
}

// ── NSPR layer methods ──────────────────────────────────────────────────────

PRStatus PR_CALLBACK SocksUdpClose(PRFileDesc* fd) {
  // Lets the proxy go, takes our layer off the stack, frees it, and closes
  // what is below. The layer used to free itself and return, so the UDP socket
  // under it was never closed: one operating-system socket left behind per
  // socket that had the layer (netwerk/test/gtest/TestStealthSocksCredentials.cpp).
  auto* data = reinterpret_cast<SocksUdpLayerData*>(fd->secret);
  if (data) {
    data->mShared->Detach();
  }
  PRFileDesc* layer = PR_PopIOLayer(fd, gIdentity);
  if (layer) {
    layer->secret = nullptr;
    layer->dtor(layer);
  }
  delete data;
  return fd->methods->close(fd);
}

// sendto: wrap data in SOCKS5 UDP header, redirect to relay. Before the
// association is made the datagram is held (a few of them); after a failure it
// is dropped and the send succeeds, as behind a router that blocks UDP: nothing
// leaves, nothing is said, the retransmissions of ICE run out in their own time.
// It is never sent direct.
PRInt32 PR_CALLBACK SocksUdpSendTo(PRFileDesc* fd, const void* buf,
                                   PRInt32 amount, PRIntn flags,
                                   const PRNetAddr* addr,
                                   PRIntervalTime timeout) {
  SocksUdpShared* shared = SharedOf(fd);
  if (!shared) {
    PR_SetError(PR_NOT_CONNECTED_ERROR, 0);
    return -1;
  }
  // To the machine itself: direct, in every state (it does not leave the machine, and the proxy could not reach it).
  if (IsLoopbackAddr(*addr)) {
    return (fd->lower->methods->sendto)(fd->lower, buf, amount, flags, addr, timeout);
  }
  uint8_t frame[kHeaderMax + kPayloadMax];
  size_t hlen = 0;
  PRNetAddr relay;
  PRUint16 socketFamily = 0;
  {
    MutexAutoLock lock(shared->mMutex);
    if (shared->mState == State::Failed) {
      return amount;
    }
    if (shared->mState == State::Pending) {
      // UDP: what cannot be held is dropped, and the sender is not told.
      if (shared->mHeld.Length() < kMaxHeld) {
        HeldDatagram* held = shared->mHeld.AppendElement();
        held->mTo = *addr;
        held->mData.AppendElements(static_cast<const uint8_t*>(buf), amount);
      }
      return amount;
    }
    if (shared->mState == State::Direct) {
      return (fd->lower->methods->sendto)(fd->lower, buf, amount, flags, addr, timeout);
    }
    relay = shared->mRelay;
    socketFamily = shared->SocketFamilyLocked();
  }
  hlen = BuildFrame(addr, buf, amount, frame);
  if (!hlen) {
    return -1;
  }
  // The destination may be IPv6 behind an IPv4 relay (RFC 1928 section 7): the frame says so (ATYP 4) and the datagram
  // goes to the relay in the family of the socket.
  if (!RelayForSocket(relay, socketFamily, &relay)) {
    PR_SetError(PR_ADDRESS_NOT_SUPPORTED_ERROR, 0);
    return -1;
  }
  PRInt32 sent = (fd->lower->methods->sendto)(
      fd->lower, frame, hlen + amount, flags, &relay, timeout);
  if (sent < 0) return sent;
  if ((size_t)sent < hlen) {
    PR_SetError(PR_IO_ERROR, 0);
    return -1;
  }
  return sent - (PRInt32)hlen;
}

PRInt32 PR_CALLBACK SocksUdpSend(PRFileDesc* fd, const void* buf,
                                 PRInt32 amount, PRIntn flags,
                                 PRIntervalTime timeout) {
  // PR_Send on a UDP socket — only valid after PR_Connect. We don't
  // support that path here; QUIC uses sendto.
  PR_SetError(PR_NOT_IMPLEMENTED_ERROR, 0);
  return -1;
}

// recvfrom: receive from relay, strip SOCKS5 UDP header, expose to caller.
PRInt32 PR_CALLBACK SocksUdpRecvFrom(PRFileDesc* fd, void* buf, PRInt32 amount,
                                     PRIntn flags, PRNetAddr* addr,
                                     PRIntervalTime timeout) {
  SocksUdpShared* shared = SharedOf(fd);
  if (!shared) {
    PR_SetError(PR_NOT_CONNECTED_ERROR, 0);
    return -1;
  }
  static thread_local uint8_t scratch[2048 + 22];
  State state;
  PRNetAddr relay;
  {
    MutexAutoLock lock(shared->mMutex);
    state = shared->mState;
    relay = shared->mRelay;
  }
  if (state == State::Direct) {
    return (fd->lower->methods->recvfrom)(fd->lower, buf, amount, flags, addr, timeout);
  }

  PRNetAddr src;
  PRInt32 got = (fd->lower->methods->recvfrom)(fd->lower, scratch, sizeof(scratch), flags, &src, timeout);
  if (got < 0) return got;
  if (IsLoopbackAddr(src) && (state != State::Ready || !FromTheRelay(relay, src))) {
    // A datagram of the machine itself, sent direct as it was sent: handed over as it is.
    if (amount < got) {
      PR_SetError(PR_BUFFER_OVERFLOW_ERROR, 0);
      return -1;
    }
    memcpy(buf, scratch, got);
    if (addr) *addr = src;
    return got;
  }
  if (state != State::Ready) {
    // Nothing is expected from a relay that is not there; what the socket held is thrown away (a readable fd would keep
    // the socket thread coming back at once, for ever).
    PR_SetError(PR_WOULD_BLOCK_ERROR, 0);  // nothing comes: as on a network that blocks UDP
    return -1;
  }
  if (!FromTheRelay(relay, src)) {
    // A datagram of someone else (RFC 1928 section 7 wants the relay's): not ours to hand to the application.
    PR_SetError(PR_WOULD_BLOCK_ERROR, 0);
    return -1;
  }
  if (got < 10) {
    PR_SetError(PR_IO_ERROR, 0);
    return -1;
  }

  // Parse SOCKS5 UDP header.
  // RSV(2) + FRAG(1) + ATYP(1) + ADDR(?) + PORT(2)
  if (scratch[0] != 0 || scratch[1] != 0) {
    // RSV must be zero per RFC.
    PR_SetError(PR_IO_ERROR, 0);
    return -1;
  }
  if (scratch[2] != 0) {
    // We don't reassemble fragments — drop the packet and indicate
    // would-block so the caller polls again.
    PR_SetError(PR_WOULD_BLOCK_ERROR, 0);
    return -1;
  }
  size_t hlen = 4;
  uint8_t atyp = scratch[3];
  if (atyp == 0x01) {  // IPv4
    if (got < 10) { PR_SetError(PR_IO_ERROR, 0); return -1; }
    if (addr) {
      memset(addr, 0, sizeof(*addr));
      addr->inet.family = PR_AF_INET;
      memcpy(&addr->inet.ip, scratch + 4, 4);
      memcpy(&addr->inet.port, scratch + 8, 2);
    }
    hlen += 4 + 2;
  } else if (atyp == 0x04) {  // IPv6
    if (got < 22) { PR_SetError(PR_IO_ERROR, 0); return -1; }
    if (addr) {
      memset(addr, 0, sizeof(*addr));
      addr->ipv6.family = PR_AF_INET6;
      memcpy(&addr->ipv6.ip, scratch + 4, 16);
      memcpy(&addr->ipv6.port, scratch + 20, 2);
    }
    hlen += 16 + 2;
  } else if (atyp == 0x03) {  // domain (rare)
    uint8_t dlen = scratch[4];
    if ((size_t)got < (size_t)5 + dlen + 2) {
      PR_SetError(PR_IO_ERROR, 0); return -1;
    }
    hlen += 1 + dlen + 2;
    if (addr) memset(addr, 0, sizeof(*addr));
  } else {
    PR_SetError(PR_IO_ERROR, 0);
    return -1;
  }

  size_t payload = (size_t)got - hlen;
  if ((size_t)amount < payload) {
    PR_SetError(PR_BUFFER_OVERFLOW_ERROR, 0);
    return -1;
  }
  memcpy(buf, scratch + hlen, payload);
  return (PRInt32)payload;
}

PRInt32 PR_CALLBACK SocksUdpRecv(PRFileDesc* fd, void* buf, PRInt32 amount,
                                 PRIntn flags, PRIntervalTime timeout) {
  return SocksUdpRecvFrom(fd, buf, amount, flags, nullptr, timeout);
}

PRInt32 PR_CALLBACK SocksUdpNoRead(PRFileDesc*, void*, PRInt32) {
  PR_SetError(PR_NOT_IMPLEMENTED_ERROR, 0);
  return -1;
}
PRInt32 PR_CALLBACK SocksUdpNoWrite(PRFileDesc*, const void*, PRInt32) {
  PR_SetError(PR_NOT_IMPLEMENTED_ERROR, 0);
  return -1;
}
PRInt32 PR_CALLBACK SocksUdpNoWritev(PRFileDesc*, const PRIOVec*, PRInt32, PRIntervalTime) {
  PR_SetError(PR_NOT_IMPLEMENTED_ERROR, 0);
  return -1;
}
PRStatus PR_CALLBACK SocksUdpNoConnect(PRFileDesc*, const PRNetAddr*, PRIntervalTime) {
  PR_SetError(PR_NOT_IMPLEMENTED_ERROR, 0);
  return PR_FAILURE;
}
PRInt32 PR_CALLBACK SocksUdpNoSendfile(PRFileDesc*, PRSendFileData*, PRTransmitFileFlags, PRIntervalTime) {
  PR_SetError(PR_NOT_IMPLEMENTED_ERROR, 0);
  return -1;
}

void EnsureMethods() {
  // Any thread: the first caller makes them, the others wait for it.
  static const bool sMade = [] {
    gIdentity = PR_GetUniqueIdentity("SOCKS UDP layer");
    gMethods = *PR_GetDefaultIOMethods();
    gMethods.close = SocksUdpClose;
    gMethods.send = SocksUdpSend;
    gMethods.recv = SocksUdpRecv;
    gMethods.sendto = SocksUdpSendTo;
    gMethods.recvfrom = SocksUdpRecvFrom;
    // The calls that do not frame a datagram would reach the socket under the layer as it is: a send direct, around the
    // proxy, or a read of the relay's frames with their headers.
    gMethods.read = SocksUdpNoRead;
    gMethods.write = SocksUdpNoWrite;
    gMethods.writev = SocksUdpNoWritev;
    gMethods.connect = SocksUdpNoConnect;
    gMethods.sendfile = SocksUdpNoSendfile;
    return true;
  }();
  (void)sMade;
}

}  // namespace

namespace {

// The socket of a PeerConnection can be an AF_INET6 socket and the relay of the proxy an IPv4 address (the layer sends to its IPv4-mapped
// form). Windows makes an AF_INET6 socket v6-only by default and then refuses that send (WSAEADDRNOTAVAIL, 10049; reproduced in plain
// Winsock: V6ONLY=1 fails, V6ONLY=0 arrives), neither NSPR nor NrSocket switch it off, and Linux has it off by default: so it is switched
// off here, before the socket is bound. It succeeds only on an AF_INET6 socket; anything else is left as it is.
void MakeDualStack(PRFileDesc* aFd) {
  PRFileDesc* native = PR_GetIdentitiesLayer(aFd, PR_NSPR_IO_LAYER);
  if (!native) {
    return;
  }
  PROsfd osfd = PR_FileDesc2NativeHandle(native);
  if (osfd == -1) {
    return;
  }
  int off = 0;
#ifdef XP_WIN
  (void)setsockopt(static_cast<SOCKET>(osfd), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
#else
  (void)setsockopt(static_cast<int>(osfd), IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
#endif
}

}  // namespace

nsresult nsSOCKSUDPIOLayerAddToSocket(PRFileDesc* aFd, PcUdpProxy* aProxy) {
  if (!aProxy) {
    return NS_ERROR_INVALID_ARG;
  }
  EnsureMethods();
  MakeDualStack(aFd);

  PRFileDesc* layer = PR_CreateIOLayerStub(gIdentity, &gMethods);
  if (!layer) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  auto* data = new SocksUdpLayerData();
  data->mShared = new SocksUdpShared();
  layer->secret = reinterpret_cast<PRFilePrivate*>(data);

  if (PR_PushIOLayer(aFd, PR_GetLayersIdentity(aFd), layer) != PR_SUCCESS) {
    LOGE(("PR_PushIOLayer failed"));
    layer->secret = nullptr;
    delete data;
    layer->dtor(layer);
    return NS_ERROR_FAILURE;
  }
  {
    // aFd is the layer now (the stack keeps its head).
    PRFileDesc* layerFd = PR_GetIdentitiesLayer(aFd, gIdentity);
    MutexAutoLock lock(data->mShared->mMutex);
    data->mShared->mLayer = layerFd ? layerFd : aFd;
  }
  // The socket takes its association from the pool (made by then, or being made on the socket thread); this returns at
  // once, whatever the proxy takes to answer.
  BindSocket(data->mShared, aProxy);
  LOG(("SOCKS5 UDP layer pushed onto fd; the association is being bound"));
  return NS_OK;
}

uint32_t nsSOCKSUDPIOLayerPoolReady() { return static_cast<uint32_t>(kPoolReady); }

void nsSOCKSUDPIOLayerShutDownPoolForTests(bool aShutDown) {
  if (aShutDown) {
    ShutDownPool();
    return;
  }
  StaticMutexAutoLock lock(sPoolMutex);
  sPoolShutDown = false;
}

void nsSOCKSUDPIOLayerWarm(const nsACString& aHost, int32_t aPort, const nsACString& aUser, const nsACString& aPass) {
  if (aHost.IsEmpty() || aPort <= 0 || aPort > 65535) {
    return;
  }
  ProxyKey key;
  key.mHost = aHost;
  key.mPort = aPort;
  key.mUser = aUser;
  key.mPass = aPass;
  SocksUdpPool::Warm(key);
}

namespace {

// The proxies the parent told the socket process about, with the id of that process. Main thread only.
struct WarmSent {
  uint64_t mProcessId = 0;
  nsTArray<ProxyKey> mKeys;
};
StaticAutoPtr<WarmSent> sWarmSent;

// The proxies whose message to the socket process is queued on the main thread and not yet decided: a page that opens a hundred
// SOCKS connections queues one message, not a hundred. Taken off when the main thread has decided (told, or not needed). Any thread.
StaticMutex sWarmQueuedMutex;
StaticAutoPtr<nsTArray<ProxyKey>> sWarmQueued MOZ_GUARDED_BY(sWarmQueuedMutex);
bool sWarmQueuedShutDown MOZ_GUARDED_BY(sWarmQueuedMutex) = false;

void RegisterWarmQueuedShutdown() {
  auto hook = [] {
    RunOnShutdown(
        [] {
          nsTArray<ProxyKey> doomed;
          StaticMutexAutoLock lock(sWarmQueuedMutex);
          sWarmQueuedShutDown = true;
          if (sWarmQueued) {
            doomed = std::move(*sWarmQueued);
            sWarmQueued = nullptr;
          }
        },
        ShutdownPhase::XPCOMShutdown);
  };
  if (NS_IsMainThread()) {
    if (!ApplicationIsShuttingDown()) hook();
    return;
  }
  NS_DispatchToMainThread(NS_NewRunnableFunction("WarmQueued::RegisterShutdown", [hook] {
    if (!ApplicationIsShuttingDown()) hook();
  }));
}

bool WarmQueueEnter(const ProxyKey& aKey) {
  StaticMutexAutoLock lock(sWarmQueuedMutex);
  if (sWarmQueuedShutDown || ApplicationIsShuttingDown()) {
    return false;
  }
  if (!sWarmQueued) {
    sWarmQueued = new nsTArray<ProxyKey>();
    RegisterWarmQueuedShutdown();
  }
  if (sWarmQueued->Contains(aKey)) {
    return false;
  }
  if (sWarmQueued->Length() >= kPoolKeys) {
    sWarmQueued->RemoveElementAt(0);
  }
  sWarmQueued->AppendElement(aKey);
  return true;
}

void WarmQueueLeave(const ProxyKey& aKey) {
  StaticMutexAutoLock lock(sWarmQueuedMutex);
  if (sWarmQueued) {
    sWarmQueued->RemoveElement(aKey);
  }
}

}  // namespace

bool nsSOCKSUDPIOLayerFirstWarmForProcess(uint64_t aProcessId, const nsACString& aHost, int32_t aPort, const nsACString& aUser,
                                          const nsACString& aPass) {
  MOZ_ASSERT(NS_IsMainThread());
  if (!sWarmSent) {
    sWarmSent = new WarmSent();
    ClearOnShutdown(&sWarmSent);
  }
  if (sWarmSent->mProcessId != aProcessId) {
    sWarmSent->mProcessId = aProcessId;
    sWarmSent->mKeys.Clear();
  }
  ProxyKey key;
  key.mHost = aHost;
  key.mPort = aPort;
  key.mUser = aUser;
  key.mPass = aPass;
  if (sWarmSent->mKeys.Contains(key)) {
    return false;
  }
  if (sWarmSent->mKeys.Length() >= kPoolKeys) {
    sWarmSent->mKeys.RemoveElementAt(0);
  }
  sWarmSent->mKeys.AppendElement(std::move(key));
  return true;
}

bool nsSOCKSUDPIOLayerSocketProcessMakesTheIceSockets() {
  MOZ_ASSERT(NS_IsMainThread());
  // The conditions under which the sockets of a PeerConnection are made in the socket process (MediaTransportHandler::Create) and
  // the socket process is launched at all (nsIOService::LaunchSocketProcess). NOT nsIOService::UseSocketProcess(): that one asks
  // whether HTTP runs in the socket process, which it does not by default, and the PeerConnections' sockets are there all the same.
  return XRE_IsParentProcess() && StaticPrefs::network_process_enabled() &&
         Preferences::GetBool("media.peerconnection.mtransport_process", false) && !PR_GetEnv("MOZ_DISABLE_SOCKET_PROCESS");
}

namespace {

// Parent process: the sockets of the PeerConnections are made in the SOCKET process, which opens no SOCKS5 connection of Necko's, so
// nothing there would ever warm the pool before its first socket: the parent, where Necko does open them, says so ONCE per proxy and
// per socket process. It is an advance and nothing more: the pool of that process takes care of itself from the first socket on (a
// taken association is replaced, one the proxy closes is replaced), so there is no timer and no message repeated. If the socket
// process is not up yet the message waits for it (CallOrWaitForSocketProcess), and nothing waits on this thread.
void WarmTheSocketProcess(const nsACString& aHost, int32_t aPort, const nsACString& aUser, const nsACString& aPass) {
  if (!XRE_IsParentProcess()) {
    return;
  }
  ProxyKey key;
  key.mHost = aHost;
  key.mPort = aPort;
  key.mUser = aUser;
  key.mPass = aPass;
  if (!WarmQueueEnter(key)) {
    return;
  }
  NS_DispatchToMainThread(NS_NewRunnableFunction(
      "WarmTheSocketProcess", [key, host = nsCString(aHost), aPort, user = nsCString(aUser), pass = nsCString(aPass)] {
        // The key leaves the queue NOW, whatever follows: it only keeps a page's hundred SOCKS connections from queueing a hundred messages
        // at once, and a socket process that never calls back (it failed to start) must not leave the proxy unannounced for good. A second
        // message for the same process is refused by nsSOCKSUDPIOLayerFirstWarmForProcess.
        WarmQueueLeave(key);
        if (!nsSOCKSUDPIOLayerSocketProcessMakesTheIceSockets()) {
          return;
        }
        RefPtr<nsIOService> io = nsIOService::GetInstance();
        if (!io) {
          return;
        }
        io->CallOrWaitForSocketProcess([host, aPort, user, pass] {
          RefPtr<SocketProcessParent> actor = SocketProcessParent::GetSingleton();
          if (actor && nsSOCKSUDPIOLayerFirstWarmForProcess(static_cast<uint64_t>(actor->OtherPid()), host, aPort, user, pass)) {
            (void)actor->SendStealthWarmSocksUdp(host, aPort, user, pass);
          }
        });
      }));
}

}  // namespace

void nsSOCKSUDPIOLayerWarm(nsIProxyInfo* aProxy) {
  if (!aProxy) {
    return;
  }
  // Only a proxy the session measured coherent carries UDP (nsIProxyInfo::CARRIES_COHERENT_UDP): any other is not asked for an
  // association at all.
  uint32_t flags = 0;
  aProxy->GetFlags(&flags);
  if (!(flags & nsIProxyInfo::CARRIES_COHERENT_UDP)) {
    return;
  }
  nsAutoCString host, user, pass;
  int32_t port = 0;
  if (NS_SUCCEEDED(aProxy->GetHost(host)) && NS_SUCCEEDED(aProxy->GetPort(&port))) {
    (void)aProxy->GetUsername(user);
    (void)aProxy->GetPassword(pass);
    nsSOCKSUDPIOLayerWarm(host, port, user, pass);   // this process: the HTTP/3 sockets of the parent
    WarmTheSocketProcess(host, port, user, pass);    // the socket process: the sockets of the PeerConnections
  }
}

nsresult StealthPutSocksUdpLayer(PRFileDesc* aFd, nsIProxyInfo* aProxyInfo) {
  nsAutoCString type;
  if (!aProxyInfo || NS_FAILED(aProxyInfo->GetType(type)) ||
      !(type.EqualsLiteral("socks") || type.EqualsLiteral("socks5"))) {
    return NS_OK;
  }
  PcUdpProxy::Socks5Info info;
  aProxyInfo->GetHost(info.mHost);
  aProxyInfo->GetPort(&info.mPort);
  aProxyInfo->GetUsername(info.mUser);
  aProxyInfo->GetPassword(info.mPass);
  // The same rule as the PeerConnection's lookup (PcUdpProxy::DoLookup): only a SOCKS5 proxy the session measured coherent
  // carries UDP; any other is a router that drops it (HTTP/3 then falls back to h2 by itself), never a direct send.
  uint32_t flags = 0;
  aProxyInfo->GetFlags(&flags);
  const bool carries = (flags & nsIProxyInfo::CARRIES_COHERENT_UDP) != 0;
  RefPtr<PcUdpProxy> proxy =
      PcUdpProxy::Known(carries ? PcUdpProxy::Kind::Socks5 : PcUdpProxy::Kind::Blocked, info);
  return nsSOCKSUDPIOLayerAddToSocket(aFd, proxy);
}

void nsSOCKSUDPIOLayerNoSocketThreadForTests(bool aMissing) { sNoSocketThreadForTests = aMissing; }

bool nsSOCKSUDPIOLayerHasFailed(PRFileDesc* aFd) {
  SocksUdpShared* shared = aFd ? SharedOf(PR_GetIdentitiesLayer(aFd, gIdentity)) : nullptr;
  if (!shared) {
    return false;
  }
  MutexAutoLock lock(shared->mMutex);
  return shared->mState == State::Failed;
}

bool nsSOCKSUDPIOLayerIsOn(PRFileDesc* aFd) {
  return aFd && gIdentity != PR_INVALID_IO_LAYER &&
         PR_GetIdentitiesLayer(aFd, gIdentity) != nullptr;
}

}  // namespace net
}  // namespace mozilla
