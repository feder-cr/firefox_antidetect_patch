/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

"use strict";

// Uses dummy.html and request_accept_language.sjs, both already listed in
// support-files of [DEFAULT] in browser.toml.
const PAGE_URL = "https://example.com/browser/netwerk/test/browser/dummy.html";
const SCRIPT_URL =
  "https://example.com/browser/netwerk/test/browser/request_accept_language.sjs";

// STEALTHFOX: the override can carry a language list (juggler.locale.override
// seeds it with the same list as intl.accept_languages). It must go out the
// way Firefox sends that list from intl.accept_languages, q-values included,
// not as the raw list.
add_task(async function test_language_override_list_is_prepared() {
  const list = "it-IT, it, en-US, en";
  const tab = BrowserTestUtils.addTab(gBrowser, PAGE_URL);
  const browser = gBrowser.getBrowserForTab(tab);
  await BrowserTestUtils.browserLoaded(browser);

  info("The header Firefox sends for the list as intl.accept_languages");
  await SpecialPowers.pushPrefEnv({ set: [["intl.accept_languages", list]] });
  const fromPref = await getAcceptLanguageHeader(browser);
  await SpecialPowers.popPrefEnv();
  is(
    fromPref,
    "it-IT,it;q=0.9,en-US;q=0.8,en;q=0.7",
    "intl.accept_languages is prepared with q-values"
  );

  info("The same list as a language override");
  browser.browsingContext.languageOverride = list;
  is(
    await getAcceptLanguageHeader(browser),
    fromPref,
    '"Accept-Language" from an override list is what the pref would send'
  );
  const languages = await SpecialPowers.spawn(browser, [], () =>
    content.navigator.languages.join("|")
  );
  is(languages, "it-IT|it|en-US|en", "navigator.languages is the list");

  browser.browsingContext.languageOverride = "";
  BrowserTestUtils.removeTab(tab);
});

async function getAcceptLanguageHeader(browser) {
  return SpecialPowers.spawn(browser, [SCRIPT_URL], async url => {
    const response = await content.fetch(url);
    return response.json();
  });
}
