/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The three API keys are declared through zoom.stealth.apikey.* prefs.

const PREF_MOZILLA = "zoom.stealth.apikey.mozilla";
const PREF_GEOLOC = "zoom.stealth.apikey.google_location_service";
const PREF_SB = "zoom.stealth.apikey.google_safebrowsing";

const MISSING_SB =
  "STEALTHFOX: nessuna chiave dichiarata in zoom.stealth.apikey.google_safebrowsing";

function formatKey(name) {
  return Services.urlFormatter.formatURL("http://x/?k=%" + name + "%");
}

function clearPrefs() {
  for (let pref of [PREF_MOZILLA, PREF_GEOLOC, PREF_SB]) {
    Services.prefs.clearUserPref(pref);
  }
}

add_task(async function test_no_declaration() {
  clearPrefs();
  let messages = [];
  let listener = {
    observe(aMsg) {
      messages.push(aMsg.message);
    },
  };
  Services.console.registerListener(listener);
  try {
    Assert.equal(formatKey("GOOGLE_SAFEBROWSING_API_KEY"), "http://x/?k=");
    Assert.equal(formatKey("MOZILLA_API_KEY"), "http://x/?k=");
    Assert.equal(formatKey("GOOGLE_LOCATION_SERVICE_API_KEY"), "http://x/?k=");
  } finally {
    Services.console.unregisterListener(listener);
  }
  Assert.ok(
    messages.some(m => m.includes(MISSING_SB)),
    "A missing declaration is reported in the console"
  );

  messages = [];
  Services.console.registerListener(listener);
  try {
    Assert.equal(Services.urlFormatter.trimSensitiveURLs("abc"), "abc");
  } finally {
    Services.console.unregisterListener(listener);
  }
  Assert.ok(
    !messages.some(m => m.includes("STEALTHFOX")),
    "trimSensitiveURLs does not log a missing declaration"
  );
});

add_task(async function test_declared_keys() {
  Services.prefs.setStringPref(PREF_MOZILLA, "AAA111");
  Services.prefs.setStringPref(PREF_GEOLOC, "BBB222");
  Services.prefs.setStringPref(PREF_SB, "CCC333");

  Assert.equal(formatKey("MOZILLA_API_KEY"), "http://x/?k=AAA111");
  Assert.equal(
    formatKey("GOOGLE_LOCATION_SERVICE_API_KEY"),
    "http://x/?k=BBB222"
  );
  Assert.equal(formatKey("GOOGLE_SAFEBROWSING_API_KEY"), "http://x/?k=CCC333");

  Assert.equal(
    Services.urlFormatter.trimSensitiveURLs("u/BBB222/CCC333/BBB222"),
    "u/[trimmed-google-api-key]/[trimmed-google-api-key]/[trimmed-google-api-key]"
  );

  // The getters are lazy: a changed pref is seen by the next call.
  Services.prefs.setStringPref(PREF_SB, "DDD444");
  Assert.equal(formatKey("GOOGLE_SAFEBROWSING_API_KEY"), "http://x/?k=DDD444");

  clearPrefs();
});
