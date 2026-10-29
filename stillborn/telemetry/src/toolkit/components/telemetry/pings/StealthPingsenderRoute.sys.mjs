/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import { PINGSENDER_ROUTE_ENV } from "resource://gre/modules/TelemetrySend.sys.mjs";

const { newProxyInfoFor, proxyAuthCallbacks, proxyForHost } =
  ChromeUtils.importESModule("resource://gre/modules/juggler/SessionRoute.js");

// The proxy record taken by takeSessionRoute(), read by pingAuthFor(). One
// task process takes the road exactly once (the environment variable is
// emptied on the first call, so a second call throws), so this is set at most
// once. Null means the default road.
let sessionProxy = null;

/**
 * Take the road of the session that started this task, and return its proxy
 * record (or null for the default road).
 *
 * The browser hands it over in `PINGSENDER_ROUTE_ENV` (TelemetrySend.sys.mjs):
 * its prefs.js, read here so this empty profile carries the session's network
 * prefs, and the proxy record that governed the session's own requests, which
 * a channel filter applies here with the very functions the browser's filter
 * uses (SessionRoute.js). B266.
 *
 * ⛔ IT REFUSES WHEN THE ROAD IS MISSING, it does not fall back. Without it the
 * ping would leave by this profile's defaults, which is the host's road: the
 * exact leak this task replaced `pingsender[.exe]` to close.
 */
export function takeSessionRoute() {
  const raw = Services.env.get(PINGSENDER_ROUTE_ENV);
  if (!raw) {
    throw new Error(
      `pingsender: ${PINGSENDER_ROUTE_ENV} is not set, so the session's road is unknown; not sending`
    );
  }
  // Read once, then out of the environment: the processes this task starts
  // (a socket process, a content process) have no use for a proxy password.
  Services.env.set(PINGSENDER_ROUTE_ENV, "");
  const { proxy, prefs } = JSON.parse(raw);

  const prefsFile = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
  prefsFile.initWithPath(prefs);
  Services.prefs.readUserPrefsFromFile(prefsFile);

  Cc["@mozilla.org/network/protocol-proxy-service;1"]
    .getService(Ci.nsIProtocolProxyService)
    .registerChannelFilter(
      {
        QueryInterface: ChromeUtils.generateQI([
          Ci.nsIProtocolProxyChannelFilter,
        ]),
        applyFilter(channel, defaultProxyInfo, callback) {
          const record = proxyForHost(proxy, channel.URI.host);
          callback.onProxyFilterResult(
            record ? newProxyInfoFor(record) : defaultProxyInfo
          );
        },
      },
      0
    );
  sessionProxy = proxy;
  return proxy;
}

/**
 * The notification callbacks that answer an HTTP proxy's 407 for a ping sent
 * to `hostname`, or null when the ping takes the default road (no proxy, or
 * the host matches the record's bypass list). The 4th argument of
 * sendStandalonePing (TelemetrySend.sys.mjs).
 */
export function pingAuthFor(hostname) {
  const record = proxyForHost(sessionProxy, hostname);
  return record ? proxyAuthCallbacks(record) : null;
}
