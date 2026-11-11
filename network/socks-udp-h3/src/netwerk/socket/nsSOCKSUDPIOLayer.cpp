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

#include "nspr.h"
#include "private/pprio.h"
#include "mozilla/Atomics.h"
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
#include "mozilla/net/SocketProcessChild.h"
#include "mozilla/net/SocketProcessParent.h"
#include "../protocol/http/nsHttpHandler.h"
#include "nsASocketHandler.h"
#include "nsICancelable.h"
#include "nsIDNSListener.h"
#include "nsIDNSRecord.h"
#include "nsIDNSService.h"
#include "nsISocketTransportService.h"
#include "nsProxyInfo.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsThreadUtils.h"
#include "prnetdb.h"
#include "nsXULAppAPI.h"

namespace mozilla {
namespace net {

// Logging — visible with MOZ_LOG=SOCKS-UDP:5.
static LazyLogModule gSocksUdpLog("SOCKS-UDP");
#define LOG(args) MOZ_LOG(gSocksUdpLog, LogLevel::Debug, args)
#define LOGE(args) MOZ_LOG(gSocksUdpLog, LogLevel::Error, args)

namespace {

PRDescIdentity gIdentity = PR_INVALID_IO_LAYER;
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

enum class State : uint8_t { Pending, Ready, Failed };

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
    for (const nsTArray<uint8_t>& frame : mHeld) {
      // UDP: one that cannot be sent is lost, as any datagram can be.
      if (mLayer->lower->methods->sendto(mLayer->lower, frame.Elements(),
                                         frame.Length(), 0, &mRelay,
                                         PR_INTERVAL_NO_WAIT) < 0) {
        LOGE(("SOCKS UDP: a held datagram was not sent (%d)", PR_GetError()));
      }
    }
    mHeld.Clear();
    mState = State::Ready;
  }

  // The association failed or is over: what is sent from now on fails.
  void Fail() {
    MutexAutoLock lock(mMutex);
    mState = State::Failed;
    mHeld.Clear();
  }

  // The socket is being closed: nothing is sent through it any more.
  void Detach() {
    MutexAutoLock lock(mMutex);
    mLayer = nullptr;
    mState = State::Failed;
    mHeld.Clear();
  }

  Mutex mMutex{"SocksUdpShared"};
  State mState = State::Pending;
  PRNetAddr mRelay;
  PRFileDesc* mLayer = nullptr;  // the layer's fd; null once the socket closes
  nsTArray<nsTArray<uint8_t>> mHeld;

 private:
  ~SocksUdpShared() = default;
};

// The control connection of one socket, as a socket handler of the socket
// transport service.
class SocksUdpAssociation final : public nsASocketHandler,
                                  public nsIDNSListener {
 public:
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSIDNSLISTENER

  SocksUdpAssociation(SocksUdpShared* aShared, const nsACString& aHost,
                      int32_t aPort, const nsACString& aUser,
                      const nsACString& aPass)
      : mShared(aShared),
        mHost(aHost),
        mPort(static_cast<uint16_t>(aPort)),
        mUser(aUser),
        mPass(aPass) {}

  // Any thread: begin, on the socket thread.
  void Start() {
    nsCOMPtr<nsIEventTarget> sts =
        do_GetService(NS_SOCKETTRANSPORTSERVICE_CONTRACTID);
    nsCOMPtr<nsIRunnable> begin = NewRunnableMethod(
        "SocksUdpAssociation::Begin", this, &SocksUdpAssociation::Begin);
    if (!sts || NS_FAILED(sts->Dispatch(begin.forget(), NS_DISPATCH_NORMAL))) {
      LOGE(("SOCKS UDP association: no socket thread"));
      mShared->Fail();
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
    mShared->Fail();
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
      mShared->Fail();
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
    mShared->Fail();
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
        mShared->MakeReady(relay);
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

  RefPtr<SocksUdpShared> mShared;
  nsCString mHost;
  uint16_t mPort;
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

// What the layer keeps on the socket.
struct SocksUdpLayerData {
  RefPtr<SocksUdpShared> mShared;
  RefPtr<SocksUdpAssociation> mAssociation;
};

SocksUdpShared* SharedOf(PRFileDesc* fd) {
  auto* data = reinterpret_cast<SocksUdpLayerData*>(fd->secret);
  return data ? data->mShared.get() : nullptr;
}

// True when a datagram comes from the relay (address and port).
bool FromTheRelay(const PRNetAddr& aRelay, const PRNetAddr& aSource) {
  if (aRelay.raw.family == PR_AF_INET && aSource.raw.family == PR_AF_INET) {
    return aRelay.inet.ip == aSource.inet.ip && aRelay.inet.port == aSource.inet.port;
  }
  if (aRelay.raw.family == PR_AF_INET6 && aSource.raw.family == PR_AF_INET6) {
    return !memcmp(&aRelay.ipv6.ip, &aSource.ipv6.ip, 16) && aRelay.ipv6.port == aSource.ipv6.port;
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
    data->mAssociation->Cancel();
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
// association is made the datagram is held (a few of them), after a failure it
// is refused: it is never sent direct.
PRInt32 PR_CALLBACK SocksUdpSendTo(PRFileDesc* fd, const void* buf,
                                   PRInt32 amount, PRIntn flags,
                                   const PRNetAddr* addr,
                                   PRIntervalTime timeout) {
  SocksUdpShared* shared = SharedOf(fd);
  if (!shared) {
    PR_SetError(PR_NOT_CONNECTED_ERROR, 0);
    return -1;
  }
  uint8_t frame[kHeaderMax + kPayloadMax];
  size_t hlen = BuildFrame(addr, buf, amount, frame);
  if (!hlen) {
    return -1;
  }
  PRNetAddr relay;
  {
    MutexAutoLock lock(shared->mMutex);
    if (shared->mState == State::Failed) {
      PR_SetError(PR_NOT_CONNECTED_ERROR, 0);
      return -1;
    }
    if (shared->mState == State::Pending) {
      // UDP: what cannot be held is dropped, and the sender is not told.
      if (shared->mHeld.Length() < kMaxHeld) {
        shared->mHeld.AppendElement()->AppendElements(frame, hlen + amount);
      }
      return amount;
    }
    relay = shared->mRelay;
  }
  if (addr->raw.family == PR_AF_INET6 && relay.raw.family == PR_AF_INET) {
    PR_SetError(PR_ADDRESS_NOT_SUPPORTED_ERROR, 0);  // no conversion: an IPv4 relay does not reach IPv6
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
  PRNetAddr relay;
  {
    MutexAutoLock lock(shared->mMutex);
    if (shared->mState != State::Ready) {
      // Nothing is expected from a relay that is not there; whatever the socket holds is thrown away, or the socket
      // stays readable and the socket thread comes back to it at once, for ever.
      PRNetAddr discarded;
      (void)(fd->lower->methods->recvfrom)(fd->lower, scratch, sizeof(scratch), flags, &discarded,
                                           PR_INTERVAL_NO_WAIT);
      PR_SetError(shared->mState == State::Failed ? PR_NOT_CONNECTED_ERROR : PR_WOULD_BLOCK_ERROR, 0);
      return -1;
    }
    relay = shared->mRelay;
  }

  PRNetAddr relaySrc;
  PRInt32 got = (fd->lower->methods->recvfrom)(
      fd->lower, scratch, sizeof(scratch), flags, &relaySrc, timeout);
  if (got < 0) return got;
  if (!FromTheRelay(relay, relaySrc)) {
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

nsresult nsSOCKSUDPIOLayerAddToSocket(PRFileDesc* aFd, const char* aProxyHost,
                                      int32_t aProxyPort,
                                      const char* aProxyUsername,
                                      const char* aProxyPassword) {
  if (!aProxyHost || !aProxyHost[0] || aProxyPort <= 0 || aProxyPort > 65535) {
    return NS_ERROR_INVALID_ARG;
  }
  EnsureMethods();

  PRFileDesc* layer = PR_CreateIOLayerStub(gIdentity, &gMethods);
  if (!layer) {
    return NS_ERROR_OUT_OF_MEMORY;
  }
  auto* data = new SocksUdpLayerData();
  data->mShared = new SocksUdpShared();
  data->mAssociation = new SocksUdpAssociation(
      data->mShared, nsDependentCString(aProxyHost), aProxyPort,
      nsDependentCString(aProxyUsername ? aProxyUsername : ""),
      nsDependentCString(aProxyPassword ? aProxyPassword : ""));
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
  // The association is made on the socket thread; this returns at once.
  data->mAssociation->Start();
  LOG(("SOCKS5 UDP layer pushed onto fd; the association is being made"));
  return NS_OK;
}

// The proxy the session leaves by, for UDP: the ONE place that says whether
// UDP goes through it. Everything that decides something about UDP behind the
// proxy asks here: the layer on every UDP socket (nsUDPSocket.cpp and
// NrSocket::create), the filter of direct ICE sends (nr_stealth_proxy_udp.h),
// the declared srflx (nr_stealth_bridge.cpp) and the HTTP/3 decisions below.
//
// Two inputs and no more. The verdict, network.proxy.socks_remote_udp, is
// declared by the core after it proved that the proxy relays UDP and that the
// UDP exit is the TCP exit. The proxy is the Juggler's (SessionRoute.js), which
// hands it to StealthSessionUdpProxyPublish when the verdict is true and the
// session proxy is SOCKS5, and clears it when the proxy goes away.
// network.proxy.* is not read: nothing in a Juggler session writes it.
//
// WHERE THE PROXY LIVES, and why it is not a pref. The credentials are held by
// two processes only: the parent, which publishes them, and the socket
// process, where ICE's UDP sockets are. They reach the socket process by one
// IPC message (PSocketProcess::StealthSessionUdpProxy), sent when they change
// and again whenever the process (re)launches (StealthSessionUdpProxyResend).
// No content process holds them and no pref does: libpref cannot keep a static
// string pref out of content processes, and
// docs/decisions/motore-06 forbids credentials in prefs. A content process
// only needs to know that UDP is routed, which is the one non-secret bool pref
// zoom.stealth.udp_proxy.armed, written next to the IPC message.
namespace {

struct SessionProxy {
  nsCString mHost;
  int32_t mPort = 0;
  nsCString mUser;
  nsCString mPass;
};

StaticMutex sSessionProxyMutex;
StaticAutoPtr<SessionProxy> sSessionProxy MOZ_GUARDED_BY(sSessionProxyMutex);

}  // namespace

void StealthSessionUdpProxySet(const nsACString& aHost, int32_t aPort,
                               const nsACString& aUser,
                               const nsACString& aPass) {
  StaticMutexAutoLock lock(sSessionProxyMutex);
  if (aHost.IsEmpty() || aPort <= 0 || aPort > 65535) {
    sSessionProxy = nullptr;
    return;
  }
  if (!sSessionProxy) {
    sSessionProxy = new SessionProxy();
    if (NS_IsMainThread()) {
      ClearOnShutdown(&sSessionProxy);
    }
  }
  sSessionProxy->mHost.Assign(aHost);
  sSessionProxy->mPort = aPort;
  sSessionProxy->mUser.Assign(aUser);
  sSessionProxy->mPass.Assign(aPass);
}

bool StealthSessionUdpProxy(nsACString& aHost, int32_t& aPort,
                            nsACString& aUser, nsACString& aPass) {
  if (!StaticPrefs::network_proxy_socks_remote_udp()) {
    return false;
  }
  StaticMutexAutoLock lock(sSessionProxyMutex);
  if (!sSessionProxy) {
    return false;
  }
  aHost.Assign(sSessionProxy->mHost);
  aPort = sSessionProxy->mPort;
  aUser.Assign(sSessionProxy->mUser);
  aPass.Assign(sSessionProxy->mPass);
  return true;
}

bool StealthSocksUdpRouted() {
  if (!StaticPrefs::network_proxy_socks_remote_udp()) {
    return false;
  }
  if (XRE_IsContentProcess()) {
    return StaticPrefs::zoom_stealth_udp_proxy_armed();
  }
  StaticMutexAutoLock lock(sSessionProxyMutex);
  return !!sSessionProxy;
}

static void SendSessionProxyToSocketProcess() {
  RefPtr<SocketProcessParent> socketProcess = SocketProcessParent::GetSingleton();
  if (!socketProcess) {
    return;
  }
  nsAutoCString host, user, pass;
  int32_t port = 0;
  {
    StaticMutexAutoLock lock(sSessionProxyMutex);
    if (sSessionProxy) {
      host = sSessionProxy->mHost;
      port = sSessionProxy->mPort;
      user = sSessionProxy->mUser;
      pass = sSessionProxy->mPass;
    }
  }
  (void)socketProcess->SendStealthSessionUdpProxy(host, port, user, pass);
}

void StealthSessionUdpProxyPublish(const nsACString& aHost, int32_t aPort,
                                   const nsACString& aUser,
                                   const nsACString& aPass) {
  MOZ_ASSERT(XRE_IsParentProcess() && NS_IsMainThread());
  StealthSessionUdpProxySet(aHost, aPort, aUser, aPass);
  SendSessionProxyToSocketProcess();
  // The content processes learn that UDP is routed, nothing more.
  Preferences::SetBool("zoom.stealth.udp_proxy.armed",
                       !aHost.IsEmpty() && aPort > 0 && aPort <= 65535,
                       PrefValueKind::Default);
}

void StealthSessionUdpProxyResend() {
  MOZ_ASSERT(XRE_IsParentProcess() && NS_IsMainThread());
  SendSessionProxyToSocketProcess();
}

mozilla::ipc::IPCResult SocketProcessChild::RecvStealthSessionUdpProxy(
    const nsACString& aHost, const int32_t& aPort, const nsACString& aUser,
    const nsACString& aPass) {
  StealthSessionUdpProxySet(aHost, aPort, aUser, aPass);
  return IPC_OK();
}

nsresult nsSOCKSUDPIOLayerAttachSession(PRFileDesc* aFd) {
  nsAutoCString host, user, pass;
  int32_t port = 0;
  if (!StealthSessionUdpProxy(host, port, user, pass)) {
    return NS_OK;
  }
  nsresult rv = nsSOCKSUDPIOLayerAddToSocket(aFd, host.get(), port, user.get(),
                                             pass.get());
  if (NS_FAILED(rv)) {
    LOGE(("SOCKS UDP layer refused (%" PRIx32 "): the socket is not usable",
          static_cast<uint32_t>(rv)));
  } else {
    LOG(("SOCKS UDP layer attached (proxy %s:%d)", host.get(), port));
  }
  return rv;
}

bool nsSOCKSUDPIOLayerIsOn(PRFileDesc* aFd) {
  return aFd && gIdentity != PR_INVALID_IO_LAYER &&
         PR_GetIdentitiesLayer(aFd, gIdentity) != nullptr;
}

// Stealthfox: also accept SOCKS5 when our SOCKS5-UDP-ASSOCIATE layer
// is enabled (network.proxy.socks_remote_udp). Without this, Firefox
// ignores Alt-Svc h3 advertisements whenever a SOCKS proxy is set,
// never creates a UDP socket, and our nsSOCKSUDPIOLayer hook never
// fires.
bool StealthHttp3ProxyAllowed(nsProxyInfo* aProxyInfo) {
  bool socksUdpEnabled = StealthSocksUdpEnabled();
  return aProxyInfo ? (aProxyInfo->IsDirect() || aProxyInfo->IsHttp3Proxy() ||
                       (socksUdpEnabled && aProxyInfo->IsSOCKS()))
                    : true;
}

bool StealthSocksUdpEnabled() {
  return StaticPrefs::network_proxy_socks_remote_udp();
}

// Stealthfox: when our SOCKS5 UDP-ASSOCIATE layer is enabled, treat
// SOCKS5 as transparent to h3 — the proxy tunnels QUIC datagrams via
// UDP_ASSOCIATE, so the connection should still be created as
// HttpConnectionUDP (not TCP nsHttpConnection with ALPN h3).
bool StealthSocksUdpH3ViaProxy(nsProxyInfo* aProxyInfo, bool aIsHttp3,
                               bool aIsHttp3ProxyConnection) {
  bool socksUdpEnabled = StealthSocksUdpEnabled();
  bool isSocks = aProxyInfo && aProxyInfo->IsSOCKS();
  if (socksUdpEnabled && isSocks) {
    return aIsHttp3;
  }
  return aIsHttp3ProxyConnection;
}

// Stealthfox: when our SOCKS5-UDP layer is enabled AND this
// connection is for HTTP/3 (UDP), we MUST resolve the host name
// client-side. Reason: h3 packets are UDP and bypass the SOCKS5
// CONNECT path; the proxy never sees a CONNECT containing the
// hostname. Our nsSOCKSUDPIOLayer needs a real numeric IP target
// to wrap into the SOCKS5 UDP datagram header (ATYP=IPv4 + ADDR).
// Without client-side DNS, h3+SOCKS goes through DnsAndConnectSocket
// but never fires OnLookupComplete, so SetupConn is never reached
// and HttpConnectionUDP::Init never runs.
bool StealthSocksUdpSkipDns(bool aIsHttp3) {
  bool socksUdpEnabled = StealthSocksUdpEnabled();
  return !(socksUdpEnabled && aIsHttp3);
}

}  // namespace net
}  // namespace mozilla
