/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Our default prefs: only prefs that Mozilla does not declare, each declared
// once. greprefs.js includes this file right after all.js. A Mozilla pref whose
// value we change is changed where Mozilla declares it (all.js, firefox.js),
// with a STEALTH-HOOK marker on the line; it is never declared again here.
// modules/libpref/test/test_stealth_prefs_declared_once.py fails otherwise.
// Static prefs are in StealthPrefList.yaml.

// STEALTH: pin the CSS system fonts to the Windows shell font, on EVERY host.
//
// The system-font keywords a page can read - caption, icon, menu, message-box,
// small-caption, status-bar - are filled from the PLATFORM's UI settings, which
// is a different subsystem from the font list: GTK settings on Linux, the shell
// font on Windows, and neither is touched by the bundled-font work. So they
// named the operating system outright. Measured 2026-08-07 on the shipped
// firefox-18, same build and seed:
//
//     Windows : family "Segoe UI", size 12px
//     Linux   : family "Sans",     size 13.3333px
//
// A public fingerprinting test page reads exactly this and reported `system_fonts: Segoe UI:Windows`
// against `Sans:Linux` - a direct platform disclosure, and one that survived
// sealing the font-family list, because "Sans" here never came from the font
// list at all.
//
// nsXPLookAndFeel::GetFontValue tries these prefs BEFORE calling
// NativeGetFont, so setting them means the host is never consulted. Values are
// the measured Windows ones: 9pt Segoe UI = 12px at 96dpi, weight 400.
//
// The sizes are QUOTED on purpose: GetFontValue reads them with
// Preferences::GetFloat, and a float pref in Gecko is declared as a string
// (cf. media.volume_scale "1.0"). Declared as a bare int the read fails and
// the fallback StyleFONT_MEDIUM_PX (16px) is used instead - measured, that is
// exactly what the first attempt produced.
//
// The four -moz-* entries are pinned too. They are not readable as CSS
// keywords, but they drive form-control rendering, and a real Windows Firefox
// draws buttons and fields with the same shell font - leaving them host-derived
// would put the difference back into button metrics, which a page CAN measure.
pref("ui.font.caption", "Segoe UI");
pref("ui.font.caption.size", "12");
pref("ui.font.icon", "Segoe UI");
pref("ui.font.icon.size", "12");
pref("ui.font.menu", "Segoe UI");
pref("ui.font.menu.size", "12");
pref("ui.font.message-box", "Segoe UI");
pref("ui.font.message-box.size", "12");
pref("ui.font.small-caption", "Segoe UI");
pref("ui.font.small-caption.size", "12");
pref("ui.font.status-bar", "Segoe UI");
pref("ui.font.status-bar.size", "12");
pref("ui.font.-moz-pull-down-menu", "Segoe UI");
pref("ui.font.-moz-pull-down-menu.size", "12");
pref("ui.font.-moz-button", "Segoe UI");
pref("ui.font.-moz-button.size", "12");
pref("ui.font.-moz-list", "Segoe UI");
pref("ui.font.-moz-list.size", "12");
pref("ui.font.-moz-field", "Segoe UI");
pref("ui.font.-moz-field.size", "12");

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
