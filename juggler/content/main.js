/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Services is available as a global

// Load SimpleChannel and Runtime in content process's global.
// NOTE: since these have to exist in both Worker and main threads, and we do
// not know a way to load ES Modules in worker threads, we have to use the loadSubScript
// utility instead.
Services.scriptloader.loadSubScript('chrome://juggler/content/SimpleChannel.js');
Services.scriptloader.loadSubScript('chrome://juggler/content/content/Runtime.js');

const {Helper} = ChromeUtils.importESModule('chrome://juggler/content/Helper.js');
const {FrameTree} = ChromeUtils.importESModule('chrome://juggler/content/content/FrameTree.js');
const {PageAgent} = ChromeUtils.importESModule('chrome://juggler/content/content/PageAgent.js');

const helper = new Helper();

export function initialize(browsingContext, docShell) {
  const data = { channel: undefined, pageAgent: undefined, frameTree: undefined, failedToOverrideTimezone: false };

  const applySetting = {
    geolocation: (geolocation) => {
      if (geolocation) {
        browsingContext.setGeolocationServiceOverride({
          coords: {
            latitude: geolocation.latitude,
            longitude: geolocation.longitude,
            accuracy: geolocation.accuracy,
            altitude: NaN,
            altitudeAccuracy: NaN,
            heading: NaN,
            speed: NaN,
          },
          timestamp: Date.now() + 24 * 60 * 60 * 1000,  // Make sure it does not expire for a day.
        });
      } else {
        browsingContext.setGeolocationServiceOverride();
      }
    },
    bypassCSP: (bypassCSP) => {
      docShell.bypassCSPEnabled = bypassCSP;
    },

    timezoneId: (timezoneId) => {
      data.failedToOverrideTimezone = !docShell.overrideTimezone(timezoneId);
    },

    locale: (locale) => {
      // languageOverride drives navigator.language(s) + the Accept-Language
      // header; overrideLocale applies the same locale to the realm's default
      // Intl locale (Intl.DateTimeFormat/NumberFormat/toLocaleString), keeping
      // navigator and Intl consistent for non-US locales.
      docShell.languageOverride = locale;
      docShell.overrideLocale(locale);
    },

    javaScriptDisabled: (javaScriptDisabled) => {
      data.frameTree.setJavaScriptDisabled(javaScriptDisabled);
    },
  };

  const contextCrossProcessCookie = Services.cpmm.sharedData.get('juggler:context-cookie-' + browsingContext.originAttributes.userContextId) || { initScripts: [], bindings: [], settings: {} };
  const pageCrossProcessCookie = Services.cpmm.sharedData.get('juggler:page-cookie-' + browsingContext.browserId) || { initScripts: [], bindings: [], interceptFileChooserDialog: false };

  // STEALTHFOX: qui c'erano docShell.overrideHasFocus e forceActiveState,
  // messi su OGNI documento di primo livello. Sono codice morto: verificato in
  // tutto l'albero che l'unico lettore e' la coppia getter/setter in
  // nsDocShell.cpp:2891-2917, e che ne' Document::HasFocus() ne'
  // ComputeVisibilityState() li guardano. Il commento in Document.cpp:17188 e'
  // nostro e lo dice: "GetForceActiveState removed in 150".
  //
  // Misurato prima di toglierli, per non fidarsi del solo sorgente: su tre
  // schede il fuoco resta UNO, come in un browser vero, sia con sia senza.
  // Stealth: keep BFCache enabled by default so pageshow event.persisted
  // behaves like a real user. Set zoom.stealth.juggler.disallow_bfcache=true
  // to restore Juggler's original behavior (needed for tests that expect
  // fresh page state on back/forward navigation).
  let disallowBFCache = false;
  try { disallowBFCache = Services.prefs.getBoolPref("zoom.stealth.juggler.disallow_bfcache", false); } catch(e) {}
  if (disallowBFCache) docShell.disallowBFCache = true;
  data.frameTree = new FrameTree(browsingContext);
  for (const [name, value] of Object.entries(contextCrossProcessCookie.settings)) {
    if (value !== undefined)
      applySetting[name](value);
  }
  for (const { worldName, name, script } of [...contextCrossProcessCookie.bindings, ...pageCrossProcessCookie.bindings])
    data.frameTree.addBinding(worldName, name, script);
  data.frameTree.setInitScripts([...contextCrossProcessCookie.initScripts, ...pageCrossProcessCookie.initScripts]);
  data.channel = new SimpleChannel('', 'process-' + Services.appinfo.processID);
  data.pageAgent = new PageAgent(data.channel, data.frameTree);
  docShell.fileInputInterceptionEnabled = !!pageCrossProcessCookie.interceptFileChooserDialog;

  data.channel.register('', {
    setInitScripts(scripts) {
      data.frameTree.setInitScripts(scripts);
    },

    addBinding({worldName, name, script}) {
      data.frameTree.addBinding(worldName, name, script);
    },

    applyContextSetting({name, value}) {
      applySetting[name](value);
    },

    setInterceptFileChooserDialog(enabled) {
      docShell.fileInputInterceptionEnabled = !!enabled;
    },

    ensurePermissions() {
      // noop, just a rountrip.
    },

    hasFailedToOverrideTimezone() {
      return data.failedToOverrideTimezone;
    },

    async awaitViewportDimensions({width, height}) {
      await new Promise(resolve => {
        const listeners = [];
        const check = () => {
          helper.removeListeners(listeners);
          if (docShell.domWindow.innerWidth === width && docShell.domWindow.innerHeight === height) {
            resolve();
            return;
          }
          // Note: "domWindow" listeners are often removed upon navigation, as specced.
          // To survive viewport changes across navigations, re-install listeners upon commit.
          listeners.push(helper.addEventListener(docShell.domWindow, 'resize', check));
          listeners.push(helper.addEventListener(data.frameTree, 'navigationcommitted', check));
        };
        check();
      });
    },

  });

  return data;
}
