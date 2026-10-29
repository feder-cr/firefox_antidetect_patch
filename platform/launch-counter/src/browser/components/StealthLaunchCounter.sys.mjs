/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Anonymous launch counter. Fires once per browser process, from the hook in
// BrowserGlue._beforeUIStartup, which runs at final-ui-startup: before any tab
// is created and before any web content can possibly observe the network.
// The target is a fixed release asset whose download_count IS the global
// launch counter (exposed through a README badge). One GET, no payload, no
// credentials, never cached; errors are swallowed.
// Gate via pref `invisible_firefox.usage_ping.enabled` (default true).
// To permanently disable, remove the STEALTH-HOOK call in BrowserGlue.sys.mjs
// and rebuild from source.
// Full disclosure: "Anonymous launch counter" in the README.
//
// THE ADDRESS IS A PREF AND NOT A LITERAL, WHICH IS THE WHOLE POINT OF THIS
// MODULE. This counter has died twice, and both deaths have one cause: the
// address was a repository NAME compiled into every shipped binary, with no
// way to redirect it afterwards.
//   * 2026-07-22 - another repo took the freed name, which killed the rename
//     redirect this URL had been riding. Ten days of launches, never counted.
//   * 2026-08-18 - the repo hosting the asset was deleted outright, so not
//     even recreating the asset was available: the name itself was gone.
// Each time, every binary in the field went on asking for an address that
// nobody could move, and the only repair was restoring that exact name. With
// the address declared instead of compiled, moving it is a pref rather than a
// rebuild, and invisible_core can point a whole fleet elsewhere without
// shipping a byte of engine.
//
// The compiled default of the pref (modules/libpref/init/StealthPrefs.js) is what a
// binary launched WITHOUT the package uses: a manual run is a real launch and
// must count. It points at the engine's own repository. The literal below is
// only a last-resort fallback for the case where that default is absent; with
// the shipped default prefs it is never reached. That literal points at
// invisible_playwright, which is the flagship repo: it cannot be deleted
// without the product going with it, which is exactly the property the two
// previous hosts turned out to lack.

export const StealthLaunchCounter = {
  fire() {
    try {
      if (
        Services.prefs.getBoolPref("invisible_firefox.usage_ping.enabled", true)
      ) {
        const usagePingUrl = Services.prefs.getStringPref(
          "invisible_firefox.usage_ping.url",
          "https://github.com/feder-cr/invisible_playwright/releases/download/usage-counter/launch.txt"
        );
        fetch(usagePingUrl, {
          method: "GET",
          credentials: "omit",
          cache: "no-store",
        }).catch(() => {});
      }
    } catch (e) {}
  },
};
