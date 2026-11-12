/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Cc = Components.classes;
const Ci = Components.interfaces;
const { say } = ChromeUtils.importESModule('resource://gre/modules/juggler/Say.js');
const { setTimeout } = ChromeUtils.importESModule('resource://gre/modules/Timer.sys.mjs');

// A channel that waits this long with no decision is the sign of a client that does not know the contract (a stock Playwright
// started on this binary, say): the hold never lets it go on a timer, so the one thing to do is to say why it waits.
export const HELD_TOO_LONG_MS = 5000;
export const HELD_TOO_LONG_LINE =
    'juggler: a request has been held for more than 5 s: no request leaves until Browser.setBrowserProxy or Browser.setNoBrowserProxy is received\n';

// Starts the one look at the hold: after HELD_TOO_LONG_MS, if aStillHeld() says a channel still waits, one line on stderr (Say.js: not
// gated by a pref, not readable from a page). It decides nothing and releases nothing; no URL, no host, no credential in it.
export function sayWhenHeldTooLong(aStillHeld) {
  setTimeout(() => {
    try {
      if (aStillHeld())
        say(HELD_TOO_LONG_LINE);
    } catch (e) {
      // A diagnostic must not become the failure it describes.
    }
  }, HELD_TOO_LONG_MS);
}

/**
 * The proxy filter of a driven browser, from the first point where Juggler
 * sees the pipe flag.
 *
 * The NetworkObserver, and the pending filter it used to register itself, is
 * built at final-ui-startup. A request made before that had no filter at all
 * and went DIRECT in a session the client drives. So Juggler registers this
 * filter at command-line-startup, and the NetworkObserver adopts it later:
 * one object, registered once for the whole life of the browser.
 *
 * Before the NetworkObserver exists there is no registry, so the session's
 * road is undecided and every channel the proxy service asks about is stored
 * as (channel, defaultProxyInfo, callback) and not answered. The same array is
 * handed to the NetworkObserver, which answers it, in order, when
 * Browser.setBrowserProxy or Browser.setNoBrowserProxy decides.
 *
 * Only channels that ask the proxy service reach the filter: http(s) and the
 * ws(s) handshake, the DoH stand-in, TCPSocket and WebRTC TCP. file, resource,
 * about, chrome, jar, moz-extension, data and blob never ask.
 */
export const EarlyProxyHold = {
  _filter: null,
  _pending: [],
  _delegate: null,
  _waitingSaid: false,

  // The first time a channel is held, one look 5 s later: a driver that never sends Browser.setBrowserProxy or
  // Browser.setNoBrowserProxy does not hang without a word. A session that decides in time (every core/wrapper session)
  // writes nothing. No URL, no credential, no line per channel, and no release on a timer: the channels stay held.
  noteHeld() {
    if (this._waitingSaid)
      return;
    this._waitingSaid = true;
    sayWhenHeldTooLong(() => this._pending.length > 0);
  },

  // The (channel, callback) pairs waiting for the decision.
  get pending() {
    return this._pending;
  },

  // Registers the filter once; a second call returns the same object.
  register() {
    if (this._filter)
      return this;
    const filter = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIProtocolProxyChannelFilter]),
      applyFilter: (channel, defaultProxyInfo, proxyFilter) => {
        if (this._delegate)
          this._delegate(channel, defaultProxyInfo, proxyFilter);
        else {
          this._pending.push({ channel, defaultProxyInfo, proxyFilter });
          this.noteHeld();
        }
      },
    };
    const protocolProxyService = Cc['@mozilla.org/network/protocol-proxy-service;1'].getService();
    protocolProxyService.registerChannelFilter(filter, 0 /* position */);
    this._filter = filter;
    return this;
  },

  // The NetworkObserver takes over: from now on it decides for each channel
  // (hold or answer); what was stored stays in `pending` for it.
  adopt(delegate) {
    this._delegate = delegate;
  },
};
