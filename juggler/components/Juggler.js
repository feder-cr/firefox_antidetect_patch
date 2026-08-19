/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Services is available as a global in XPCOM component context

// Load SimpleChannel in browser-process global.
Services.scriptloader.loadSubScript('chrome://juggler/content/SimpleChannel.js');
const {Dispatcher} = ChromeUtils.importESModule("chrome://juggler/content/protocol/Dispatcher.js");
const {BrowserHandler} = ChromeUtils.importESModule("chrome://juggler/content/protocol/BrowserHandler.js");
const {NetworkObserver} = ChromeUtils.importESModule("chrome://juggler/content/NetworkObserver.js");
const {TargetRegistry} = ChromeUtils.importESModule("chrome://juggler/content/TargetRegistry.js");
const {Helper} = ChromeUtils.importESModule('chrome://juggler/content/Helper.js');
const {ActorManagerParent} = ChromeUtils.importESModule('resource://gre/modules/ActorManagerParent.sys.mjs');
const helper = new Helper();

const Cc = Components.classes;
const Ci = Components.interfaces;

// Register JSWindowActors that will be instantiated for each frame.
ActorManagerParent.addJSWindowActors({
  JugglerFrame: {
    parent: {
      esModuleURI: 'chrome://juggler/content/JugglerFrameParent.sys.mjs',
    },
    child: {
      esModuleURI: 'chrome://juggler/content/JugglerFrameChild.sys.mjs',
      events: {
        // Normally, we instantiate an actor when a new window is created.
        DOMWindowCreated: {},
        // However, for same-origin iframes, the navigation from about:blank
        // to the URL will share the same window, so we need to also create
        // an actor for a new document via DOMDocElementInserted.
        DOMDocElementInserted: {},
        // Also, listening to DOMContentLoaded.
        DOMContentLoaded: {},
        DOMWillOpenModalDialog: {},
        DOMModalDialogClosed: {},
      },
    },
    allFrames: true,
  },
});

let browserStartupFinishedCallback;
let browserStartupFinishedPromise = new Promise(x => browserStartupFinishedCallback = x);

export class Juggler {
  get classDescription() { return "Sample command-line handler"; }
  get classID() { return Components.ID('{f7a74a33-e2ab-422d-b022-4fb213dd2639}'); }
  get contractID() { return "@mozilla.org/remote/juggler;1" }
  get QueryInterface() {
    return ChromeUtils.generateQI([ Ci.nsICommandLineHandler, Ci.nsIObserver ]);
  }
  get helpInfo() {
    return "  --juggler            Enable Juggler automation\n";
  }

  handle(cmdLine) {
    // flag has to be consumed in nsICommandLineHandler:handle
    // to avoid issues on macos. See Marionette.jsm::handle() for more details.
    // TODO: remove after Bug 1724251 is fixed.
    cmdLine.handleFlag("juggler-pipe", false);
  }

  // This flow is taken from Remote agent and Marionette.
  // See https://github.com/mozilla-firefox/firefox/blob/35e22180b0b61413dd8eccf6c00b1c6fac073eee/remote/components/RemoteAgent.sys.mjs#L417
  async observe(subject, topic) {
    switch (topic) {
      case "profile-after-change":
        Services.obs.addObserver(this, "command-line-startup");
        Services.obs.addObserver(this, "browser-idle-startup-tasks-finished");
        break;
      case "command-line-startup":
        Services.obs.removeObserver(this, topic);
        const cmdLine = subject;
        const jugglerPipeFlag = cmdLine.handleFlag('juggler-pipe', false);
        if (!jugglerPipeFlag)
          return;

        this._silent = cmdLine.findFlag('silent', false) >= 0;
        if (this._silent) {
          Services.startup.enterLastWindowClosingSurvivalArea();
          browserStartupFinishedCallback();
        }
        Services.obs.addObserver(this, "final-ui-startup");
        break;
      case "browser-idle-startup-tasks-finished":
        browserStartupFinishedCallback();
        break;
      // Used to wait until the initial application window has been opened.
      case "final-ui-startup":
        Services.obs.removeObserver(this, topic);

        const targetRegistry = new TargetRegistry();
        new NetworkObserver(targetRegistry);

        const loadStyleSheet = () => {
          if (Cc["@mozilla.org/gfx/info;1"].getService(Ci.nsIGfxInfo).isHeadless) {
            const styleSheetService = Cc["@mozilla.org/content/style-sheet-service;1"].getService(Components.interfaces.nsIStyleSheetService);
            const ioService = Cc["@mozilla.org/network/io-service;1"].getService(Components.interfaces.nsIIOService);
            const uri = ioService.newURI('chrome://juggler/content/content/hidden-scrollbars.css', null, null);
            styleSheetService.loadAndRegisterSheet(uri, styleSheetService.AGENT_SHEET);
          }
        };

        // Force create hidden window here, otherwise its creation later closes the web socket!
        // Since https://phabricator.services.mozilla.com/D219834, hiddenDOMWindow is only available on MacOS.
        if (Services.appShell.hasHiddenWindow) {
          Services.appShell.hiddenDOMWindow;
        }

        let pipeStopped = false;
        let browserHandler;
        const pipe = Cc['@mozilla.org/juggler/remotedebuggingpipe;1'].getService(Ci.nsIRemoteDebuggingPipe);
        const connection = {
          QueryInterface: ChromeUtils.generateQI([Ci.nsIRemoteDebuggingPipeClient]),
          receiveMessage(message) {
            if (this.onmessage)
              this.onmessage({ data: message });
          },
          disconnected() {
            if (browserHandler)
              browserHandler['Browser.close']();
          },
          send(message) {
            if (pipeStopped) {
              // We are missing the response to Browser.close,
              // but everything works fine. Once we actually need it,
              // we have to stop the pipe after the response is sent.
              return;
            }
            pipe.sendMessage(message);
          },
        };
        pipe.init(connection);
        const dispatcher = new Dispatcher(connection);
        browserHandler = new BrowserHandler(dispatcher.rootSession(), dispatcher, targetRegistry, browserStartupFinishedPromise, () => {
          if (this._silent)
            Services.startup.exitLastWindowClosingSurvivalArea();
          connection.onclose();
          pipe.stop();
          pipeStopped = true;
        });
        dispatcher.rootSession().setHandler(browserHandler);
        loadStyleSheet();
        // STEALTHFOX: Playwright dichiara il browser pronto SOLO quando legge
        // questa riga sullo stdout del processo (waitForReadyState cerca la
        // stringa esatta 'Juggler listening to the pipe' nei log del browser).
        //
        // dump() nei moduli di sistema passa da nsJSUtils::DumpEnabled(), che
        // senza DEBUG e senza MOZ_ENABLE_JS_DUMP - nessuno dei due definito in
        // una nostra build di release - ricade su
        // browser.dom.window.dump.enabled, il cui default nello StaticPrefList
        // e' @IS_NOT_MOZILLA_OFFICIAL@. Con MOZILLA_OFFICIAL=1 vale FALSE, la
        // riga non esce, e ogni lancio scade con il pipe perfettamente
        // funzionante: nessun errore da nessuna parte.
        //
        // Misurato il 2026-08-19 sullo stesso binario, tre bracci: controllo
        // PILOTA; sola pref spenta una riga prima -> TimeoutError con
        // PW_PIPE_READ/WRITE validi, pipe.init ok e 4 notifiche su 4; pref
        // spenta piu' questo rimedio -> PILOTA di nuovo.
        //
        // Si accende SOLO intorno a questa riga e si rimette esattamente com'era
        // (clearUserPref se non c'era un valore utente), perche' una pref lasciata
        // accesa per tutta la sessione sarebbe una divergenza dal retail. Nessuna
        // pagina esiste ancora a final-ui-startup, e window.dump non restituisce
        // niente di diverso nei due stati, quindi la finestra non e' osservabile.
        const _dumpUtente = Services.prefs.prefHasUserValue("browser.dom.window.dump.enabled");
        const _dumpVal = Services.prefs.getBoolPref("browser.dom.window.dump.enabled", false);
        Services.prefs.setBoolPref("browser.dom.window.dump.enabled", true);
        try {
          dump(`\nJuggler listening to the pipe\n`);
        } finally {
          if (_dumpUtente) Services.prefs.setBoolPref("browser.dom.window.dump.enabled", _dumpVal);
          else Services.prefs.clearUserPref("browser.dom.window.dump.enabled");
        }
        break;
    }
  }

}

const jugglerInstance = new Juggler();

// This is used by the XPCOM codepath which expects a constructor
export var JugglerFactory = function() {
  return jugglerInstance;
};

