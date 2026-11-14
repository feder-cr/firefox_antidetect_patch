/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * The proxy of the contexts that ended, for the life of the browser.
 *
 * A context that closes while its page is still working leaves requests in
 * flight: lookups, DoH requests, retries. Each of them asks the proxy filter
 * AFTER the context left the registry, and the registry knows no such context:
 * with no browser-wide proxy the answer was "no proxy", which is DIRECT, over
 * the host's own address, for a request that exists only because the context
 * had a proxy. The proxy of the context does not end with the context: its
 * requests keep leaving by it, exactly as when it was alive.
 *
 * TargetRegistry's `BrowserContext.destroy` keeps the record here before it
 * forgets the context, and `TargetRegistry.proxyRecordFor` asks here after the
 * live contexts. The ids are never reused (ContextualIdentityService counts
 * up), and a record is a handful of strings, so nothing is ever dropped.
 * Only a context that had a proxy of its own is kept: one without takes the
 * browser's, which the registry still has. The default context (0) is never
 * gone.
 * (juggler/tests/a_context_that_ended_keeps_the_proxy_of_its_requests.test.mjs)
 */
const gone = new Map();

export const GoneContexts = {
  keep(userContextId, record) {
    if (userContextId && record)
      gone.set(userContextId, record);
  },

  // The record the context had, or null for a context that never had one.
  recordFor(userContextId) {
    return gone.get(userContextId) || null;
  },
};
