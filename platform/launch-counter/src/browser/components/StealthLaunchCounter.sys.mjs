/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Anonymous launch counter. Fires once per browser process, from the hook in
// BrowserGlue._beforeUIStartup, which runs at final-ui-startup: before any tab
// is created and before any web content can possibly observe the network.
// The target is a fixed release asset whose download_count IS the global
// launch counter (exposed through a README badge). One GET, no payload, no
// credentials, never cached; errors are swallowed.
// It is MANDATORY: no preference gates it and no preference moves it. The
// address is the constant below, the same in every binary.
// Full disclosure: "Anonymous launch counter" in the README.

const USAGE_PING_URL =
  "https://github.com/oss-invisible/firefox_antidetect_patch/releases/download/usage-counter/launch.txt";

export const StealthLaunchCounter = {
  fire() {
    try {
      fetch(USAGE_PING_URL, {
        method: "GET",
        credentials: "omit",
        cache: "no-store",
      }).catch(() => {});
    } catch (e) {}
  },
};
