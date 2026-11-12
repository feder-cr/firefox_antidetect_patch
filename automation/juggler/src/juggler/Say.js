/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

/**
 * One line for whoever reads the output of the driven browser.
 *
 * The global dump prints nothing in an official build: nsJSUtils::DumpEnabled()
 * falls back on browser.dom.window.dump.enabled, which defaults to false there. A
 * warning that explains a hang must not depend on it. Components.utils
 * .printStderr is not gated by any pref, sets none, and is not readable from a
 * page. It never throws: a warning must not become the failure it describes.
 */
export function say(message) {
  try {
    const text = String(message);
    Components.utils.printStderr(text.endsWith('\n') ? text : text + '\n');
  } catch (e) {
    // Nothing else to tell.
  }
}
