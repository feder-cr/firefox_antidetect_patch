/* Any copyright is dedicated to the Public Domain.
 * https://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

const { HttpServer } = ChromeUtils.importESModule(
  "resource://testing-common/httpd.sys.mjs"
);
const { StealthLaunchCounter } = ChromeUtils.importESModule(
  "resource:///modules/StealthLaunchCounter.sys.mjs"
);

let server;
let seen;

add_setup(() => {
  server = new HttpServer();
  seen = [];
  server.registerPathHandler("/launch.txt", (req, res) => {
    seen.push({
      method: req.method,
      cookie: req.hasHeader("Cookie"),
      cacheControl: req.hasHeader("Cache-Control")
        ? req.getHeader("Cache-Control")
        : "",
    });
    res.setStatusLine(req.httpVersion, 200, "OK");
    res.write("x");
  });
  server.start(-1);
  registerCleanupFunction(() => new Promise(r => server.stop(r)));
  Services.prefs.setStringPref(
    "invisible_firefox.usage_ping.url",
    `http://localhost:${server.identity.primaryPort}/launch.txt`
  );
});

add_task(async function test_enabled_sends_one_plain_get() {
  Services.prefs.setBoolPref("invisible_firefox.usage_ping.enabled", true);
  StealthLaunchCounter.fire();
  await TestUtils.waitForCondition(() => seen.length == 1);
  Assert.equal(seen[0].method, "GET");
  Assert.ok(!seen[0].cookie, "credentials omit: no Cookie header");
  Assert.equal(
    seen[0].cacheControl,
    "no-cache",
    "cache no-store: revalidating request"
  );
});

add_task(async function test_disabled_sends_nothing() {
  seen.length = 0;
  Services.prefs.setBoolPref("invisible_firefox.usage_ping.enabled", false);
  StealthLaunchCounter.fire();
  await new Promise(r => do_timeout(500, r));
  Assert.equal(seen.length, 0);
});

add_task(async function test_unreachable_url_is_swallowed() {
  Services.prefs.setBoolPref("invisible_firefox.usage_ping.enabled", true);
  Services.prefs.setStringPref(
    "invisible_firefox.usage_ping.url",
    "http://localhost:1/none"
  );
  // Must not throw nor leave an unhandled rejection.
  StealthLaunchCounter.fire();
  await new Promise(r => do_timeout(500, r));
  Assert.ok(true);
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
