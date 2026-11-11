/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthTRRContext_h
#define mozilla_net_StealthTRRContext_h

#include "mozilla/Maybe.h"
#include "mozilla/OriginAttributes.h"
#include "nsError.h"
#include "nsStringFwd.h"

class nsIChannel;
class nsILoadInfo;

namespace mozilla::net {

/**
 * Behind a proxy the DNS-over-HTTPS request leaves by the proxy of the browser
 * context whose lookup caused it. The proxy filter (Juggler's) answers per
 * userContextId of the channel's load info, so the load info of the DoH channel
 * and of the stand-in channel of its proxy lookup carry that id.
 *
 * The context of a lookup, taken from its origin suffix: Nothing for the
 * default context (the browser's proxy answers it, as upstream), else the
 * attributes with the userContextId alone.
 */
Maybe<OriginAttributes> StealthTRRContextOfSuffix(
    const nsACString& aOriginSuffix);

/**
 * The context a DoH channel carries, from its load info (any thread: the load
 * info of a TRR channel is a TRRLoadInfo when the channel is off the main
 * thread). Nothing for the default context.
 */
Maybe<OriginAttributes> StealthTRRContextOfLoadInfo(nsILoadInfo* aLoadInfo);

/**
 * Gives a load info the context. Nothing: left as it is. A context that cannot
 * be given is an error, never a silent default: the request would leave by the
 * wrong proxy.
 */
nsresult StealthApplyTRRContext(nsILoadInfo* aLoadInfo,
                                const Maybe<OriginAttributes>& aContext);

/**
 * The same on the load info of a channel (main thread or the TRR thread).
 */
nsresult StealthApplyTRRContextToChannel(
    nsIChannel* aChannel, const Maybe<OriginAttributes>& aContext);

/**
 * Fail closed. When the proxy of a DoH request could not be learned, upstream
 * goes on DIRECT ("we just failover to DIRECT when proxy resolution fails"),
 * which would send the name from the host's own address while the session is
 * behind a proxy. The request fails instead, with the error this returns: a
 * cancellation keeps its own code, any other failure is "the proxy cannot be
 * reached".
 */
nsresult StealthTRRProxyFailure(nsresult aProxyStatus);

}  // namespace mozilla::net

#endif  // mozilla_net_StealthTRRContext_h
