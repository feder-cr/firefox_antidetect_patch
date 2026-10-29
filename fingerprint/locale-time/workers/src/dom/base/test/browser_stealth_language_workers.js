/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

"use strict";

// Browser tests are separate scopes, so the constants and helpers below are
// copies of the ones in browser_language_override.js that this test needs.
const PAGE_URL = "https://example.com/chrome/dom/base/test/file_empty.html";
const languages = ["en-US", "es-ES", "fr-FR"];

add_task(async function test_language_override_reaches_workers() {
  const tab = BrowserTestUtils.addTab(gBrowser, PAGE_URL);
  const browser = gBrowser.getBrowserForTab(tab);

  await BrowserTestUtils.browserLoaded(browser);

  const defaultLanguage = await getIntlLanguage(browser);
  const languageOverride = getLanguageToOverride(defaultLanguage);

  info("Set language override before the workers are created");
  browser.browsingContext.languageOverride = languageOverride;
  await assertLanguageOverridden(browser, languageOverride);

  for (const kind of ["dedicated", "nested", "shared"]) {
    const result = await getWorkerLanguages(browser, kind);
    is(
      result.intl,
      languageOverride,
      `Intl locale in a ${kind} worker follows the language override`
    );
    is(
      result.language,
      languageOverride,
      `navigator.language in a ${kind} worker follows the language override`
    );
    Assert.deepEqual(
      result.languages,
      [languageOverride],
      `navigator.languages in a ${kind} worker follows the language override`
    );
  }

  BrowserTestUtils.removeTab(tab);
});

async function getWorkerLanguages(browser, kind) {
  return SpecialPowers.spawn(browser, [kind], async workerKind => {
    const probe = `
      function probe() {
        return {
          intl: Intl.DateTimeFormat().resolvedOptions().locale,
          language: navigator.language,
          languages: Array.from(navigator.languages),
        };
      }
    `;
    const blobURL = source =>
      content.URL.createObjectURL(
        new content.Blob([source], { type: "text/javascript" })
      );
    const receive = target =>
      new Promise(resolve => {
        target.addEventListener(
          "message",
          event => resolve(JSON.parse(content.JSON.stringify(event.data))),
          { once: true }
        );
      });

    if (workerKind === "shared") {
      const worker = new content.SharedWorker(
        blobURL(
          probe + "onconnect = e => e.ports[0].postMessage(probe());"
        )
      );
      const received = receive(worker.port);
      worker.port.start();
      return received;
    }

    let source = probe + "postMessage(probe());";
    if (workerKind === "nested") {
      const inner = blobURL(source);
      source = `const w = new Worker("${inner}");
                w.onmessage = e => postMessage(e.data);`;
    }
    const worker = new content.Worker(blobURL(source));
    return receive(worker);
  });
}

function getLanguageToOverride(defaultLanguage) {
  return languages.find(lang => lang !== defaultLanguage);
}

async function getIntlLanguage(browser) {
  return SpecialPowers.spawn(browser, [], () => {
    return content.eval(`Intl.DateTimeFormat().resolvedOptions().locale`);
  });
}

async function getNavigatorLanguage(browser) {
  return SpecialPowers.spawn(browser, [], () => {
    return content.eval(`window.navigator.language`);
  });
}

async function getNavigatorLanguages(browser) {
  return SpecialPowers.spawn(browser, [], () => {
    return content.eval(`window.navigator.languages`);
  });
}

async function assertLanguageOverridden(browser, languageOverride) {
  is(
    await getIntlLanguage(browser),
    languageOverride,
    "new Intl.DateTimeFormat().resolvedOptions().locale is overridden"
  );

  is(
    await getNavigatorLanguage(browser),
    languageOverride,
    "navigator.language is overridden"
  );

  const navigatorLanguages = await getNavigatorLanguages(browser);
  is(
    navigatorLanguages.includes(languageOverride),
    true,
    "navigator.languages is overridden"
  );
}
