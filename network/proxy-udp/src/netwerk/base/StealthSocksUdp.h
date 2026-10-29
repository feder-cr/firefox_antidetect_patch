/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef netwerk_base_StealthSocksUdp_h
#define netwerk_base_StealthSocksUdp_h

#include "prio.h"

namespace mozilla::net {

// Called by nsUDPSocket::InitWithAddress right after the UDP fd is created.
// Pushes the SOCKS UDP layer onto aFd when the SOCKS5 proxy prefs ask for it
// (see the definition). Never fails the caller: on error the socket stays a
// plain direct UDP socket and the failure is only logged (module UDPSocket).
// aFd must be non-null.
void StealthAttachSocksUdpLayer(PRFileDesc* aFd);

}  // namespace mozilla::net

#endif  // netwerk_base_StealthSocksUdp_h
