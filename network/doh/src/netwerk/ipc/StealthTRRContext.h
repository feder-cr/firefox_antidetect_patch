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
class nsISocketTransport;

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
 * A request made on behalf of the TLS of a page leaves by the proxy, and its
 * name is resolved by the resolver, of the browser context of that page. The
 * OCSP request of a certificate keeps of the page's attributes only the first
 * party domain and the private browsing id (Mozilla's rule, which stays); this
 * adds the userContextId, the one the proxy filter answers by. The default
 * context stays the default one.
 */
void StealthKeepRequestContext(const OriginAttributes& aPage,
                               OriginAttributes& aRequest);

/**
 * The attributes of the socket transport that carries a TLS: the TLS layered
 * over a tunnel (the transport of an HTTPS proxy or of an HTTP/2 tunnel) is
 * made for the page whose connection it is, so its socket control must know
 * that page's context, not the default one. The default attributes when there
 * is no transport.
 */
OriginAttributes StealthTransportContext(nsISocketTransport* aTransport);

/**
 * Fail closed. When the proxy of a DoH request could not be learned, upstream
 * goes on DIRECT ("we just failover to DIRECT when proxy resolution fails"),
 * which would send the name from the host's own address while the session is
 * behind a proxy. The request fails instead, with the error this returns: a
 * cancellation keeps its own code, any other failure is "the proxy cannot be
 * reached".
 */
nsresult StealthTRRProxyFailure(nsresult aProxyStatus);

/**
 * The same rule for the nsHttpChannel of a DoH request. DNS runs in the parent
 * when there is no socket process, and after a proxy 401/407 the request is
 * sent from the main thread, where DNSUtils::CreateChannelHelper makes a real
 * nsHttpChannel whose own OnProxyAvailable fails over to DIRECT. True when the
 * channel is a TRR service channel (TRR.cpp marks it) whose proxy lookup failed
 * and that was not cancelled: it must end with StealthTRRProxyFailure. Every
 * other channel keeps upstream's behaviour.
 */
bool StealthFailClosedOnProxyFailure(nsresult aProxyStatus,
                                     bool aIsTRRServiceChannel,
                                     bool aCanceled);

/**
 * What the two channel classes do with that rule, once, in the header: a
 * template over the channel class, whose AsyncAbort (HttpAsyncAborter) and
 * CloseCacheEntry are public. True: the channel is ended and the caller returns
 * at once; false: nothing was done and upstream's code goes on.
 *
 * nsHttpChannel (a DoH request made on the main thread): the rule above, then
 * the cache entry is closed, as upstream does when BeginConnect fails.
 */
template <typename ChannelT>
bool StealthTRRFailClosed(ChannelT* aChannel, nsresult aProxyStatus,
                          bool aIsTRRServiceChannel, bool aCanceled) {
  if (!StealthFailClosedOnProxyFailure(aProxyStatus, aIsTRRServiceChannel,
                                       aCanceled)) {
    return false;
  }
  aChannel->CloseCacheEntry(false);
  (void)aChannel->AsyncAbort(StealthTRRProxyFailure(aProxyStatus));
  return true;
}

/**
 * TRRServiceChannel: the same rule for a channel that is one by construction,
 * and has no cache entry. A failed lookup of the channel's proxy ends it unless
 * it was cancelled (aCanceled; the start of the request has no cancellation to
 * consult and passes false).
 */
template <typename ServiceChannelT>
bool StealthTRRServiceFailClosed(ServiceChannelT* aChannel,
                                 nsresult aProxyStatus, bool aCanceled) {
  if (!StealthFailClosedOnProxyFailure(aProxyStatus, true, aCanceled)) {
    return false;
  }
  (void)aChannel->AsyncAbort(StealthTRRProxyFailure(aProxyStatus));
  return true;
}

/**
 * A channel of a context that has a proxy never goes out DIRECT because the
 * lookup of its proxy failed. aIsTRRServiceChannel is what the hook of
 * nsHttpChannel passed alone before: a DoH request is always mandatory. Any
 * other channel is when the user context of its load info is in the declared
 * list of contexts with a proxy (network.trr.stealth.contexts: "*" or decimal
 * ids), the list the resolver already follows for the names. A channel of a
 * context without a proxy keeps upstream's behaviour (a failed lookup goes on
 * DIRECT). Defined in netwerk/dns/StealthSessionResolver.cpp, next to the parser
 * of that list.
 *
 * The first form is pure (the list is a parameter), the second reads the pref
 * and the context of aLoadInfo (null: the default context). Any thread.
 */
bool StealthProxyIsMandatory(bool aIsTRRServiceChannel, uint32_t aUserContextId,
                             const nsACString& aContextsWithProxy);
bool StealthProxyIsMandatory(bool aIsTRRServiceChannel, nsILoadInfo* aLoadInfo);

}  // namespace mozilla::net

#endif  // mozilla_net_StealthTRRContext_h
