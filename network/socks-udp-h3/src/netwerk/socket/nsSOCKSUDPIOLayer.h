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

#include "prio.h"
#include "nscore.h"

namespace mozilla {
namespace net {

/*
 * Push the SOCKS5 UDP layer onto `aFd`. The layer immediately opens a TCP
 * control connection to `aProxyHost:aProxyPort`, performs greeting+auth+
 * UDP_ASSOCIATE, and rewrites all subsequent sendto() / recvfrom() calls.
 *
 * `aProxyUsername` / `aProxyPassword` may be empty for no-auth proxies;
 * if non-empty, the layer offers method 0x02 (user/pass) to the proxy.
 *
 * Returns NS_OK on success. On failure, the caller's UDP fd is left
 * untouched (still usable for direct UDP).
 */
nsresult nsSOCKSUDPIOLayerAddToSocket(PRFileDesc* aFd,
                                      const char* aProxyHost,
                                      int32_t aProxyPort,
                                      const char* aProxyUsername,
                                      const char* aProxyPassword);

}  // namespace net
}  // namespace mozilla

#endif /* nsSOCKSUDPIOLayer_h__ */
