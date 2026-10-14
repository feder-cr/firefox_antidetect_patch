/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

import {
  PINGSENDER_ROUTE_ENV,
  sendStandalonePing,
} from "resource://gre/modules/TelemetrySend.sys.mjs";

const { newProxyInfoFor, proxyAuthCallbacks, proxyForHost } =
  ChromeUtils.importESModule("resource://gre/modules/juggler/SessionRoute.js");

export async function runBackgroundTask(commandLine) {
  const proxy = takeSessionRoute();

  let sends = [];
  for (let i = 0; i < commandLine.length; i += 2) {
    sends.push(
      sendPing(commandLine.getArgument(i), commandLine.getArgument(i + 1), proxy)
    );
  }

  return Promise.all(sends);
}

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
function takeSessionRoute() {
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
  return proxy;
}

// The standalone pingsender utility had an allowlist of endpoints, which was
// added to prevent it from being used as a generic exfiltration utility by
// unrelated malware running on the same system. It's unclear whether a gecko-
// based pingsender would be similarly desirable for that use-case, but it's
// easy enough to implement an allowlist here as well.
const ALLOWED_ENDPOINTS = ["localhost", "incoming.telemetry.mozilla.org"];

async function sendPing(endpoint, path, proxy) {
  console.log(`pingsender: sending ${path} to ${endpoint}`);

  let hostname = new URL(endpoint).hostname;
  if (!ALLOWED_ENDPOINTS.includes(hostname)) {
    throw new Error(`pingsender: Endpoint ${endpoint} is not allowed`);
  }

  let json = await IOUtils.readUTF8(path);
  const record = proxyForHost(proxy, hostname);
  await sendStandalonePing(
    endpoint,
    json,
    {
      "User-Agent": "pingsender/2.0",
      "X-PingSender-Version": "2.0",
    },
    record ? proxyAuthCallbacks(record) : null
  );

  return IOUtils.remove(path);
}
