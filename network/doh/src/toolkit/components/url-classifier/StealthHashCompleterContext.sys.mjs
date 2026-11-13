/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * A Safe Browsing completion (gethash) is made for a page load, so it is
 * requested in the browser context of that page: its channel carries the
 * page's userContextId, which is what the proxy filter of the session answers
 * by, and the name of the provider is resolved by that context's resolver.
 * Upstream creates the channel in the default context and batches the
 * completions of every caller of one tick into one request per gethash URL.
 *
 * nsUrlClassifierDBService::Classify knows the page's principal and hands the
 * context to the completion callback (nsIUrlClassifierHashCompleterCallback
 * .userContextId). Here:
 *  - a completion of a context other than the default one goes to the
 *    completer of that context, a HashCompleter of its own (own batch, own
 *    back-off, own pending and in-flight requests), so a request never mixes
 *    contexts and the back-off the server asked for is kept per context, as for
 *    separate machines;
 *  - the channel of such a completer carries the context in its load info.
 * The default context, and a callback that declares no context (the tests
 * of upstream), take upstream's code unchanged. The cache of completed
 * prefixes lives in the database and stays shared by every context.
 */

// userContextId -> the HashCompleter of that context. One entry per context
// that ever had a prefix to complete, which is rare.
const gContextCompleters = new Map();

const DEFAULT_USER_CONTEXT_ID = 0;

// The context a completion callback was made in. A callback with no such
// attribute, or the default value, means the default context.
function userContextIdOf(aCallback) {
  const id = aCallback ? aCallback.userContextId : undefined;
  return Number.isInteger(id) && id > DEFAULT_USER_CONTEXT_ID
    ? id
    : DEFAULT_USER_CONTEXT_ID;
}

/**
 * Called first by HashCompleter.complete. True: the completion was handed to
 * the completer of its context and the caller returns; false: nothing was
 * done and upstream's code goes on (default context, or aCompleter is already
 * the completer of a context).
 */
export function stealthCompleteInContext(
  aCompleter,
  aPartialHash,
  aGethashUrl,
  aTableName,
  aCallback
) {
  if (aCompleter.stealthUserContextId) {
    return false;
  }
  const userContextId = userContextIdOf(aCallback);
  if (userContextId == DEFAULT_USER_CONTEXT_ID) {
    return false;
  }
  let completer = gContextCompleters.get(userContextId);
  if (!completer) {
    completer = new aCompleter.constructor();
    completer.stealthUserContextId = userContextId;
    gContextCompleters.set(userContextId, completer);
  }
  completer.complete(aPartialHash, aGethashUrl, aTableName, aCallback);
  return true;
}

/**
 * Called by HashCompleterRequestBase.openChannel once upstream has set the
 * attributes of the channel (the first party domain of Safe Browsing): the
 * request of the completer of a context carries that context too. A channel
 * that cannot be given the context throws, and the request ends in failure
 * (beginBuildChannel notifies it): it never leaves by the wrong road.
 */
export function stealthApplyContext(aChannel, aCompleter) {
  const userContextId = aCompleter.stealthUserContextId;
  if (!userContextId) {
    return;
  }
  aChannel.loadInfo.originAttributes = {
    firstPartyDomain: aChannel.loadInfo.originAttributes.firstPartyDomain,
    userContextId,
  };
}
