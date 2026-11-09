/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Our default prefs: only prefs that Mozilla does not declare, each declared
// once. greprefs.js includes this file right after all.js. A Mozilla pref whose
// value we change is changed where Mozilla declares it (all.js, firefox.js),
// with a STEALTH-HOOK marker on the line; it is never declared again here.
// modules/libpref/test/test_stealth_prefs_declared_once.py fails otherwise.
// Static prefs are in StealthPrefList.yaml.

// The CSS system fonts (ui.font.*) are declared by invisible_core, and ONLY there:
// the engine ships no copy, not even as a floor, and on Linux the declaration gate
// refuses a launch that lacks them (StealthSystemFonts.cpp).

// The SOCKS proxy credentials, declared empty so that the prefs exist with a
// string type; read by netwerk/base/StealthSocksCredentials.cpp and
// StealthSocksUdp.cpp.
pref("network.proxy.socks_username",        "");
pref("network.proxy.socks_password",        "");

// STEALTHFOX: anonymous launch counter pref. Default true. The ping fires from
// browser/components/StealthLaunchCounter.sys.mjs, called by BrowserGlue at
// startup. To disable per-session: set false via about:config or extra_prefs.
// To disable permanently: remove that call and rebuild.
// Full disclosure: README of feder-cr/invisible_playwright.
pref("invisible_firefox.usage_ping.enabled", true);
// The counter's address is a PREF, not a constant: it died twice as a
// repository name compiled into every binary with no way to move it. Now
// invisible_core repoints it without a rebuild, and this default is the
// engine's own repository, so the thing that pings and the thing that is
// counted are the same release series.
pref("invisible_firefox.usage_ping.url", "https://github.com/feder-cr/firefox_antidetect_patch/releases/download/usage-counter/launch.txt");
