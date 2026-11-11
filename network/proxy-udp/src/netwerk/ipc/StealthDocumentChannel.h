/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthDocumentChannel_h
#define mozilla_net_StealthDocumentChannel_h

#include "mozilla/Maybe.h"
#include "mozilla/net/NeckoChannelParams.h"
#include "nsError.h"

class nsIChannel;
class nsILoadInfo;
class nsIURI;

namespace mozilla::net {

/**
 * The channel a proxy lookup is made for when it is made on behalf of a document (the ICE sockets of a PeerConnection):
 * built from the document's load info, as WebrtcTCPSocket builds the channel of its HTTP proxy, so that the proxy
 * filters (Juggler's, per browser context) see the origin attributes of the document and answer with the proxy its
 * context uses, bypass list included. The load info is given back as well (the caller of the HTTP proxy tunnel sets it on
 * the channel it goes on with). Parent process, main thread. One function for WebrtcTCPSocket and for the lookup of the UDP.
 */
nsresult StealthNewChannelOfLoadInfo(nsIURI* aURI, uint32_t aProxyResolveFlags,
                                     const LoadInfoArgs& aLoadInfoArgs,
                                     nsIChannel** aResult,
                                     nsILoadInfo** aLoadInfo = nullptr);

}  // namespace mozilla::net

#endif  // mozilla_net_StealthDocumentChannel_h
