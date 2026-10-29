/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The road of the pingsender background task (B266): what TelemetrySend.sys.mjs
 * and BackgroundTask_pingsender.sys.mjs share. TelemetrySend only calls the
 * functions below from one-line hooks.
 */

import { AppConstants } from "resource://gre/modules/AppConstants.sys.mjs";

/**
 * The environment variable that carries the session's road to the pingsender
 * background task, as JSON: `{proxy, prefs}`. Written by `runPingSender` for
 * the one child it starts, read by BackgroundTask_pingsender.sys.mjs (B266).
 */
export const PINGSENDER_ROUTE_ENV = "STEALTHFOX_PINGSENDER_ROUTE";

/**
 * @param {object} request the ServiceRequest of sendStandalonePing, after
 *   `open()` and before it is sent.
 * @param {nsIInterfaceRequestor} [callbacks] handed to the channel before the
 *   request goes out. The pingsender background task passes the answer to an
 *   HTTP proxy's 407 here: a background request gets no prompt of its own, and
 *   XMLHttpRequest asks the channel's earlier callbacks first.
 */
export function attachCallbacks(request, callbacks) {
  if (callbacks) {
    request.channel.notificationCallbacks = callbacks;
  }
}

/**
 * Which program the pings go to, and its arguments.
 *
 * ⛔ ALWAYS `firefox[.exe] --backgroundtask pingsender URL path ...`, NEVER
 * `pingsender[.exe]` (B266). The separate executable took its road from
 * the HOST - WinINet's system proxy settings on Windows, libcurl's
 * environment on Linux - so a session behind a proxy sent its shutdown
 * pings from the machine's own address: measured on Windows and Linux,
 * with a SOCKS5 proxy, an HTTP one and none. WinINet cannot express a
 * SOCKS5 proxy or SOCKS credentials at all. The background task is this
 * engine, with necko, and leaves by the road `sessionRoute()` hands it
 * below.
 *
 * `params` is a fresh array on every call: the caller appends the ping urls
 * and paths to it.
 *
 * @returns {{exeName: string, params: string[]}}
 */
export function invocation() {
  const exeName =
    AppConstants.MOZ_APP_NAME +
    (AppConstants.platform === "win" ? ".exe" : "");
  return { exeName, params: ["--backgroundtask", "pingsender"] };
}

/**
 * Start the background task with the session's road in its environment.
 *
 * The road goes through the environment: not the command line, which any
 * process on the machine can list, and not a file that could outlive the
 * session with a proxy password in it. Only this child inherits it,
 * because `runAsync` starts the process on this thread before returning.
 *
 * @param {nsIProcess} process an initialised process (init, startHidden, noShell done)
 * @param {string[]} params the complete argument list
 * @param {nsIObserver} observer
 */
export function runAsync(process, params, observer) {
  Services.env.set(PINGSENDER_ROUTE_ENV, JSON.stringify(sessionRoute()));
  try {
    process.runAsync(params, params.length, observer);
  } finally {
    Services.env.set(PINGSENDER_ROUTE_ENV, "");
  }
}

/**
 * The road this session's requests leave by, as the pingsender background
 * task needs it (B266):
 *
 * - `proxy`: the proxy record that governs a request with no page - the
 *   browser's own telemetry is one - bypass list included, or null. Juggler
 *   holds it (`Browser.setBrowserProxy`, `BrowserContext.setProxy`); when this
 *   browser is not driven by Juggler there is none, and the record is null.
 * - `prefs`: this profile's prefs.js, so the task reads the network prefs the
 *   session was declared with (`network.proxy.*`, `zoom.stealth.dns.*`) rather
 *   than the defaults of its own empty profile. Saved at profile-before-change,
 *   before the shutdown pings are flushed.
 */
function sessionRoute() {
  let proxy = null;
  if (Cu.isESModuleLoaded("resource://gre/modules/juggler/TargetRegistry.js")) {
    const { TargetRegistry } = ChromeUtils.importESModule(
      "resource://gre/modules/juggler/TargetRegistry.js"
    );
    const registry = TargetRegistry.instance();
    // userContextId 0: the context of a request with the system principal.
    proxy = registry ? registry.proxyRecordFor(0) : null;
  }
  const prefs = Services.dirsvc.get("ProfD", Ci.nsIFile);
  prefs.append("prefs.js");
  return { proxy, prefs: prefs.path };
}
