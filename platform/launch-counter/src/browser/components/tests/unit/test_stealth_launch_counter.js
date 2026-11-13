/* Any copyright is dedicated to the Public Domain.
 * https://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// The launch counter is mandatory: no preference gates it and none moves it. The address is a constant of the module; the test observes the request
// at that address and cancels it before it leaves the machine (no server, no network).
const { TestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/TestUtils.sys.mjs"
);
const { StealthLaunchCounter } = ChromeUtils.importESModule(
  "resource:///modules/StealthLaunchCounter.sys.mjs"
);

const COUNTER_URL =
  "https://github.com/oss-invisible/firefox_antidetect_patch/releases/download/usage-counter/launch.txt";
const OTHER_URL = "http://localhost:1/none";

let seen = [];
const observer = {
  observe(subject) {
    let channel = subject.QueryInterface(Ci.nsIHttpChannel);
    let spec = channel.URI.spec;
    if (spec == COUNTER_URL || spec == OTHER_URL) {
      seen.push({
        url: spec,
        method: channel.requestMethod,
        cookiePolicy: channel.loadInfo.cookiePolicy,
        cacheMode: channel.QueryInterface(Ci.nsIHttpChannelInternal)
          .fetchCacheMode,
      });
      channel.cancel(Cr.NS_ERROR_ABORT);
    }
  },
};

add_setup(() => {
  Services.obs.addObserver(observer, "http-on-opening-request");
  registerCleanupFunction(() =>
    Services.obs.removeObserver(observer, "http-on-opening-request")
  );
});

add_task(async function test_sends_one_plain_get_to_the_fixed_address() {
  seen.length = 0;
  StealthLaunchCounter.fire();
  await TestUtils.waitForCondition(() => seen.length == 1);
  Assert.equal(seen[0].url, COUNTER_URL);
  Assert.equal(seen[0].method, "GET");
  Assert.equal(
    seen[0].cookiePolicy,
    Ci.nsILoadInfo.SEC_COOKIES_OMIT,
    "credentials omit: no cookies"
  );
  Assert.equal(
    seen[0].cacheMode,
    Ci.nsIHttpChannelInternal.FETCH_CACHE_MODE_NO_STORE,
    "cache no-store: never served from or stored in the cache"
  );
});

add_task(async function test_old_prefs_neither_stop_it_nor_move_it() {
  // The two preferences the counter used to read. Set the way an old client would set them: it must send the same request to the same address.
  seen.length = 0;
  Services.prefs.setBoolPref("invisible_firefox.usage_ping.enabled", false);
  Services.prefs.setStringPref("invisible_firefox.usage_ping.url", OTHER_URL);
  StealthLaunchCounter.fire();
  await TestUtils.waitForCondition(() => seen.length >= 1);
  await new Promise(r => do_timeout(300, r));
  Assert.equal(seen.length, 1, "one request");
  Assert.equal(seen[0].url, COUNTER_URL, "to the fixed address, not the pref");
  Assert.ok(
    !seen.some(r => r.url == OTHER_URL),
    "nothing goes to the address the old pref names"
  );
  Services.prefs.clearUserPref("invisible_firefox.usage_ping.enabled");
  Services.prefs.clearUserPref("invisible_firefox.usage_ping.url");
});

add_task(async function test_a_failed_request_is_swallowed() {
  // The observer cancels the request, so the fetch rejects: fire() must not throw nor leave an unhandled rejection.
  seen.length = 0;
  StealthLaunchCounter.fire();
  await TestUtils.waitForCondition(() => seen.length == 1);
  await new Promise(r => do_timeout(300, r));
  Assert.ok(true);
});

add_task(async function test_the_module_reads_no_preference() {
  let src = await (
    await fetch("resource:///modules/StealthLaunchCounter.sys.mjs")
  ).text();
  Assert.ok(!src.includes("usage_ping"), "no usage_ping preference is read");
  Assert.ok(!src.includes("Services.prefs"), "no preference is read at all");
});

add_task(async function test_hook_is_wired() {
  // Guards the one thing xpcshell cannot reach: the final-ui-startup
  // observer reaches the call.
  let src = await (
    await fetch("resource:///modules/BrowserGlue.sys.mjs")
  ).text();
  Assert.ok(
    src.includes("// STEALTH-HOOK(platform/launch-counter)") &&
      src.includes("lazy.StealthLaunchCounter.fire()")
  );
});
