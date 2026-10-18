/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Services is available as a global

// Load SimpleChannel and Runtime in content process's global.
// NOTE: since these have to exist in both Worker and main threads, and we do
// not know a way to load ES Modules in worker threads, we have to use the loadSubScript
// utility instead.
Services.scriptloader.loadSubScript('resource://gre/modules/juggler/SimpleChannel.js');
Services.scriptloader.loadSubScript('resource://gre/modules/juggler/content/Runtime.js');

const {Helper} = ChromeUtils.importESModule('resource://gre/modules/juggler/Helper.js');
const {FrameTree} = ChromeUtils.importESModule('resource://gre/modules/juggler/content/FrameTree.js');
const {PageAgent} = ChromeUtils.importESModule('resource://gre/modules/juggler/content/PageAgent.js');

const helper = new Helper();

export function initialize(browsingContext, docShell) {
  const data = { channel: undefined, pageAgent: undefined, frameTree: undefined };

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

    // No `locale` and no `timezoneId` here: the context's language list and
    // zone are the BrowsingContext's LanguageOverride and TimezoneOverride
    // fields, set from the parent (see PageTarget.updateLanguageOverride and
    // updateTimezoneOverride in TargetRegistry.js).

    javaScriptDisabled: (javaScriptDisabled) => {
      data.frameTree.setJavaScriptDisabled(javaScriptDisabled);
    },
  };

  const contextCrossProcessCookie = Services.cpmm.sharedData.get(helper.contextCookieKey(browsingContext.originAttributes.userContextId)) || { initScripts: [], bindings: [], settings: {} };
  const pageCrossProcessCookie = Services.cpmm.sharedData.get(helper.pageCookieKey(browsingContext.browserId)) || { initScripts: [], bindings: [], interceptFileChooserDialog: false };

  // STEALTHFOX: qui c'erano docShell.overrideHasFocus e forceActiveState,
  // messi su OGNI documento di primo livello. Sono codice morto: verificato in
  // tutto l'albero che l'unico lettore e' la coppia getter/setter in
  // nsDocShell.cpp:2891-2917, e che ne' Document::HasFocus() ne'
  // ComputeVisibilityState() li guardano. Il commento in Document.cpp:17188 e'
  // nostro e lo dice: "GetForceActiveState removed in 150".
  //
  // Misurato prima di toglierli, per non fidarsi del solo sorgente: su tre
  // schede il fuoco resta UNO, come in un browser vero, sia con sia senza.
  // STEALTHFOX: BFCache stays enabled, as for a real user, so
  // pageshow's event.persisted behaves like one. Upstream Juggler disables it
  // per docshell; the knob that was here to restore that
  // (zoom.stealth.juggler.disallow_bfcache) set an attribute nothing read, so
  // it never did anything, and it is gone with the attribute.
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
  // A document restored from the back-forward cache announces itself when
  // its channel is attached again, before anything it said meanwhile goes
  // out: the client learns the document before it hears it. The detachment
  // and the request to be bound again are JugglerFrameChild's.
  data.channel.onReattached(() => data.frameTree.announceRestoredDocument());
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

    // In device pixels: see PageTarget._awaitContentSize. One device pixel of
    // slack, because the parent's size is a CSS size times a scale and the
    // viewer's is the same size after layout snapped it to whole pixels.
    async awaitViewportDimensions({deviceWidth, deviceHeight}) {
      await new Promise(resolve => {
        const listeners = [];
        const check = () => {
          helper.removeListeners(listeners);
          const w = {}, h = {};
          docShell.domWindow.windowUtils.getDocumentViewerSize(w, h);
          if (Math.abs(w.value - deviceWidth) <= 1 && Math.abs(h.value - deviceHeight) <= 1) {
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
