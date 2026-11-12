/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * SOCKS5 UDP IO Layer (Stealthfox 2026-04-28)
 *
 * Wraps an NSPR UDP socket so that all sendto() / recvfrom() calls go
 * through a SOCKS5 proxy via the UDP ASSOCIATE command (RFC 1928 §6).
 *
 * Wire layout:
 *   1. TCP control connection to proxy:
 *        Greeting [VER=5, NMETHODS, METHODS] -> [VER=5, METHOD]
 *        (if METHOD=0x02 user/pass) -> auth subnegotiation
 *        UDP ASSOCIATE [VER=5, CMD=3, RSV=0, ATYP, ADDR, PORT]
 *          -> [VER=5, REP, RSV=0, ATYP, BND.ADDR, BND.PORT]
 *      The TCP connection MUST stay open for the lifetime of the UDP
 *      relay; the proxy tears down the relay if TCP closes.
 *   2. UDP datagrams to BND.ADDR:BND.PORT, each prefixed with:
 *        [RSV(2)=0, FRAG(1)=0, ATYP(1), DST.ADDR, DST.PORT, DATA]
 *      Replies arrive at the same UDP port we send from, with the same
 *      header layout (DST = the source server).
 *
 * Exposed API: push the layer onto an NSPR UDP fd via
 * nsSOCKSUDPIOLayerAddToSocket(...).
 */

#ifndef nsSOCKSUDPIOLayer_h__
#define nsSOCKSUDPIOLayer_h__

#include "nsString.h"
#include "nscore.h"
#include "prio.h"

class nsIProxyInfo;

namespace mozilla {
namespace net {

class nsProxyInfo;
class PcUdpProxy;

/*
 * Push the SOCKS5 UDP layer onto `aFd` and return at once. The proxy is the one
 * of the PeerConnection the socket belongs to (PcUdpProxy). What happens then,
 * on the socket thread, without waiting: no proxy: the socket is a plain one;
 * a proxy that carries no UDP: what is sent goes nowhere; SOCKS5: the
 * association (the TCP control connection to the proxy, greeting, auth,
 * UDP_ASSOCIATE; the name of the proxy is resolved by the resolver, the
 * connection is a non-blocking socket the socket transport service polls). The layer rewrites
 * all subsequent sendto() / recvfrom() calls. What is sent before the
 * association is made is held (a few datagrams) and goes to the relay once it
 * is; when the association fails every later send is dropped without an error, as behind a router that blocks UDP: nothing is sent direct.
 *
 * With a user name and a password the layer offers method 0x02 (user/pass) to
 * the proxy, as the nsIProxyInfo of the lookup says.
 *
 * Returns NS_OK once the layer is on the socket; an error only when it cannot
 * be pushed (bad arguments, no memory), and then the fd is left as it was.
 */
nsresult nsSOCKSUDPIOLayerAddToSocket(PRFileDesc* aFd, PcUdpProxy* aProxy);

/*
 * The associations of a proxy, made before a socket asks for one: called where Firefox learns a SOCKS5 proxy (when
 * Necko opens a SOCKS5 connection, nsSOCKSIOLayer.cpp), with what that proxy is. The pool of that proxy is brought to
 * two associations (being made on the socket thread, or made); a socket the layer is put on takes one, and the pool
 * makes another. A proxy that did not make the last one asked of it is not asked again by this call, only by a socket
 * that needs one. Any thread; returns at once.
 */
void nsSOCKSUDPIOLayerWarm(const nsACString& aHost, int32_t aPort, const nsACString& aUser, const nsACString& aPass);
void nsSOCKSUDPIOLayerWarm(nsIProxyInfo* aProxy);

/* How many associations the pool of a proxy keeps ready (the tests follow the code, not a copy of the number). */
uint32_t nsSOCKSUDPIOLayerPoolReady();

/* Tests only: do what the XPCOM shutdown hook does to the pool (true), or open it again (false). */
void nsSOCKSUDPIOLayerShutDownPoolForTests(bool aShutDown);

/*
 * Parent process, main thread: whether the socket process aProcessId has to be told about this proxy: true the first time for a proxy
 * and a process (and again for a new process id: a socket process that was restarted), false otherwise. The message is an advance, said
 * once: the pool of the socket process replaces what it hands out and what the proxy closes by itself. The parent tells the socket
 * process by PSocketProcess::StealthWarmSocksUdp, because the sockets of the PeerConnections are made there.
 */
bool nsSOCKSUDPIOLayerFirstWarmForProcess(uint64_t aProcessId, const nsACString& aHost, int32_t aPort, const nsACString& aUser,
                                          const nsACString& aPass);

/*
 * Parent process, main thread: whether the sockets of the PeerConnections are made in the socket process (so the parent has to tell it
 * about the proxies), which is the case by default: the socket process exists and WebRTC uses it.
 */
bool nsSOCKSUDPIOLayerSocketProcessMakesTheIceSockets();

/*
 * The layer for the SOCKS5 proxy of a connection (an HTTP/3 connection through a proxy): aProxyInfo is the nsIProxyInfo the
 * connection was made with, so the proxy of its context, credentials included. NS_OK and no layer for a proxy that is not
 * SOCKS5; the same layer, and the same associations, as for the ICE sockets.
 */
nsresult StealthPutSocksUdpLayer(PRFileDesc* aFd, nsIProxyInfo* aProxyInfo);

// Test only: the associations are started as if the socket transport service were gone (a browser that is shutting down).
void nsSOCKSUDPIOLayerNoSocketThreadForTests(bool aMissing);

// Test only: the next nsSOCKSUDPIOLayerAddToSocket (once) fails as if the layer could not be pushed: the socket of the PeerConnection must
// then not exist (a plain one would send direct).
void nsSOCKSUDPIOLayerFailNextPushForTests(bool aFail);

// True when the SOCKS5 UDP layer is on aFd.
bool nsSOCKSUDPIOLayerIsOn(PRFileDesc* aFd);

// True when the association of the layer on aFd failed: its sends are dropped without a word (a network that blocks UDP).
bool nsSOCKSUDPIOLayerHasFailed(PRFileDesc* aFd);

}  // namespace net
}  // namespace mozilla

#endif /* nsSOCKSUDPIOLayer_h__ */
