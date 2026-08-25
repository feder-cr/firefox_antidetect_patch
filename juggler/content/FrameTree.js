/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";
const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

const {Helper} = ChromeUtils.importESModule('chrome://juggler/content/Helper.js');

const helper = new Helper();

// ⛔ MONDO SCRIPT - leva `STEALTHFOX_SCRIPT_WORLD=1`.
//
// E' una variabile d'AMBIENTE e non una pref perche' i mondi si costruiscono
// prima che le prefs arrivino: su questo percorso Playwright non scrive niente
// nel profilo, manda le prefs sul filo e `Browser.enable` le applica a browser
// gia' avviato. Con una pref la leva risultava inerte, e la rilettura da dentro
// il browser e' cio' che l'ha fatto vedere.
let _mondoScript = null;

function _leggiMondoScript() {
  try {
    return Services.env.get('STEALTHFOX_SCRIPT_WORLD') === '1';
  } catch (e) {
    return false;
  }
}

export class FrameTree {
  static mondoScript() {
    if (_mondoScript === null)
      _mondoScript = _leggiMondoScript();
    return _mondoScript;
  }

  constructor(rootBrowsingContext) {
    helper.decorateAsEventEmitter(this);

    this._rootBrowsingContext = rootBrowsingContext;

    this._browsingContextGroup = rootBrowsingContext.group;
    if (!this._browsingContextGroup.__jugglerFrameTrees)
      this._browsingContextGroup.__jugglerFrameTrees = new Set();
    this._browsingContextGroup.__jugglerFrameTrees.add(this);
    this._isolatedWorlds = new Map();

    this._webSocketEventService = Cc[
      "@mozilla.org/websocketevent/service;1"
    ].getService(Ci.nsIWebSocketEventService);

    this._runtime = new Runtime(false /* isWorker */);
    this._workers = new Map();
    this._frameIdToFrame = new Map();
    this._pageReady = false;
    this._javaScriptDisabled = false;
    for (const browsingContext of helper.collectAllBrowsingContexts(rootBrowsingContext))
      this._createFrame(browsingContext);
    this._mainFrame = this.frameForBrowsingContext(rootBrowsingContext);

    const webProgress = rootBrowsingContext.docShell.QueryInterface(Ci.nsIInterfaceRequestor)
                                .getInterface(Ci.nsIWebProgress);
    this.QueryInterface = ChromeUtils.generateQI([
      Ci.nsIWebProgressListener,
      Ci.nsIWebProgressListener2,
      Ci.nsISupportsWeakReference,
    ]);

    this._wdm = Cc["@mozilla.org/dom/workers/workerdebuggermanager;1"].createInstance(Ci.nsIWorkerDebuggerManager);
    this._wdmListener = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIWorkerDebuggerManagerListener]),
      onRegister: this._onWorkerCreated.bind(this),
      onUnregister: this._onWorkerDestroyed.bind(this),
    };
    this._wdm.addListener(this._wdmListener);
    for (const workerDebugger of this._wdm.getWorkerDebuggerEnumerator())
      this._onWorkerCreated(workerDebugger);

    const flags = Ci.nsIWebProgress.NOTIFY_STATE_DOCUMENT |
                  Ci.nsIWebProgress.NOTIFY_LOCATION;
    this._eventListeners = [
      helper.addObserver((docShell, topic, loadIdentifier) => {
        const frame = this.frameForDocShell(docShell);
        if (!frame)
          return;
        frame._pendingNavigationId = helper.toProtocolNavigationId(loadIdentifier);
        this.emit(FrameTree.Events.NavigationStarted, frame);
      }, 'juggler-navigation-started-renderer'),
      helper.addObserver(this._onDOMWindowCreated.bind(this), 'content-document-global-created'),
      helper.addObserver(this._onDOMWindowCreated.bind(this), 'juggler-dom-window-reused'),
      helper.addObserver((browsingContext, topic, why) => {
        this._onBrowsingContextAttached(browsingContext);
      }, 'browsing-context-attached'),
      helper.addObserver((browsingContext, topic, why) => {
        this._onBrowsingContextDetached(browsingContext);
      }, 'browsing-context-discarded'),
      helper.addObserver((subject, topic, eventInfo) => {
        const [type, jugglerEventId] = eventInfo.split(' ');
        this.emit(FrameTree.Events.InputEvent, { type, jugglerEventId: +(jugglerEventId ?? '0') });
      }, 'juggler-mouse-event-hit-renderer'),
      helper.addProgressListener(webProgress, this, flags),
    ];

    this._dragEventListeners = [];
  }

  workers() {
    return [...this._workers.values()];
  }

  runtime() {
    return this._runtime;
  }

  setInitScripts(scripts) {
    for (const world of this._isolatedWorlds.values())
      world._scriptsToEvaluateOnNewDocument = [];

    for (let { worldName, script } of scripts) {
      worldName = worldName || '';
      const existing = this._isolatedWorlds.has(worldName);
      const world = this._ensureWorld(worldName);
      world._scriptsToEvaluateOnNewDocument.push(script);
      // FIXME: 'should inherit http credentials from browser context' fails without this
      if (worldName && !existing) {
        for (const frame of this.frames())
          frame._createIsolatedContext(worldName);
      }
    }
  }

  _ensureWorld(worldName) {
    worldName = worldName || '';
    let world = this._isolatedWorlds.get(worldName);
    if (!world) {
      world = new IsolatedWorld(worldName);
      this._isolatedWorlds.set(worldName, world);
    }
    return world;
  }

  _frameForWorker(workerDebugger) {
    if (workerDebugger.type !== Ci.nsIWorkerDebugger.TYPE_DEDICATED)
      return null;
    if (!workerDebugger.window)
      return null;
    return this.frameForDocShell(workerDebugger.window.docShell);
  }

  _onDOMWindowCreated(window) {
    const frame = this.frameForDocShell(window.docShell);
    if (!frame)
      return;
    frame._onGlobalObjectCleared();
  }

  setJavaScriptDisabled(javaScriptDisabled) {
    this._javaScriptDisabled = javaScriptDisabled;
    for (const frame of this.frames())
      frame._updateJavaScriptDisabled();
  }

  _onWorkerCreated(workerDebugger) {
    // Note: we do not interoperate with firefox devtools.
    if (workerDebugger.isInitialized)
      return;
    const frame = this._frameForWorker(workerDebugger);
    if (!frame)
      return;
    const worker = new Worker(frame, workerDebugger);
    this._workers.set(workerDebugger, worker);
    this.emit(FrameTree.Events.WorkerCreated, worker);
  }

  _onWorkerDestroyed(workerDebugger) {
    const worker = this._workers.get(workerDebugger);
    if (!worker)
      return;
    worker.dispose();
    this._workers.delete(workerDebugger);
    this.emit(FrameTree.Events.WorkerDestroyed, worker);
  }

  allFramesInBrowsingContextGroup(group) {
    const frames = [];
    for (const frameTree of (group.__jugglerFrameTrees || [])) {
      for (const frame of frameTree.frames()) {
        try {
          // Try accessing docShell and domWindow to filter out dead frames.
          // This might happen for print-preview frames, but maybe for something else as well.
          frame.docShell();
          frame.domWindow();
          frames.push(frame);
        } catch (e) {
          dump(`WARNING: unable to access docShell and domWindow of the frame[id=${frame.id()}]\n`);
        }
      }
    }
    return frames;
  }

  isPageReady() {
    return this._pageReady;
  }

  forcePageReady() {
    if (this._pageReady)
      return false;
    this._pageReady = true;
    this.emit(FrameTree.Events.PageReady);
    return true;
  }

  addBinding(worldName, name, script) {
    worldName = worldName || '';
    const world = this._ensureWorld(worldName);
    world._bindings.set(name, script);
    for (const frame of this.frames())
      frame._addBinding(worldName, name, script);
  }

  frameForBrowsingContext(browsingContext) {
    if (!browsingContext)
      return null;
    const frameId = helper.browsingContextToFrameId(browsingContext);
    return this._frameIdToFrame.get(frameId) ?? null;
  }

  frameForDocShell(docShell) {
    if (!docShell)
      return null;
    const frameId = helper.browsingContextToFrameId(docShell.browsingContext);
    return this._frameIdToFrame.get(frameId) ?? null;
  }

  frame(frameId) {
    return this._frameIdToFrame.get(frameId) || null;
  }

  frames() {
    let result = [];
    collect(this._mainFrame);
    return result;

    function collect(frame) {
      result.push(frame);
      for (const subframe of frame._children)
        collect(subframe);
    }
  }

  mainFrame() {
    return this._mainFrame;
  }

  dispose() {
    this._browsingContextGroup.__jugglerFrameTrees.delete(this);
    this._wdm.removeListener(this._wdmListener);
    this._runtime.dispose();
    try {
      helper.removeListeners(this._eventListeners);
    } catch (e) {
      if (e) dump(`[FrameTree] removeListeners(_eventListeners) failed (half-destroyed webProgress): ${e.message}\n`);
    }
    try {
      helper.removeListeners(this._dragEventListeners);
    } catch (e) {
      if (e) dump(`[FrameTree] removeListeners(_dragEventListeners) failed: ${e.message}\n`);
    }
  }

  onWindowEvent(event) {
    if (event.type !== 'DOMDocElementInserted' || !event.target.ownerGlobal)
      return;

    const docShell = event.target.ownerGlobal.docShell;
    const frame = this.frameForDocShell(docShell);
    if (!frame) {
      dump(`WARNING: ${event.type} for unknown frame ${helper.browsingContextToFrameId(docShell.browsingContext)}\n`);
      return;
    }
    if (frame._pendingNavigationId) {
      docShell.QueryInterface(Ci.nsIWebNavigation);
      this._frameNavigationCommitted(frame, docShell.currentURI.spec);
    }

    if (frame === this._mainFrame) {
      helper.removeListeners(this._dragEventListeners);
      const chromeEventHandler = docShell.chromeEventHandler;
      const options = {
        mozSystemGroup: true,
        capture: true,
      };
      const emitInputEvent = (event) => this.emit(FrameTree.Events.InputEvent, { type: event.type, jugglerEventId: 0 });
      // Drag events are dispatched from content process, so these we don't see in the
      // `juggler-mouse-event-hit-renderer` instrumentation.
      this._dragEventListeners = [
        helper.addEventListener(chromeEventHandler, 'dragstart', emitInputEvent, options),
        helper.addEventListener(chromeEventHandler, 'dragover', emitInputEvent, options),
      ];
    }
  }

  _frameNavigationCommitted(frame, url) {
    for (const subframe of frame._children)
      this._detachFrame(subframe);
    const navigationId = frame._pendingNavigationId;
    frame._pendingNavigationId = null;
    frame._lastCommittedNavigationId = navigationId;
    frame._url = url;
    this.emit(FrameTree.Events.NavigationCommitted, frame);
    if (frame === this._mainFrame)
      this.forcePageReady();
  }

  onStateChange(progress, request, flag, status) {
    if (!(request instanceof Ci.nsIChannel))
      return;
    const channel = request.QueryInterface(Ci.nsIChannel);
    const docShell = progress.DOMWindow.docShell;
    const frame = this.frameForDocShell(docShell);
    if (!frame)
      return;

    if (!channel.isDocument) {
      // Somehow, we can get worker requests here,
      // while we are only interested in frame documents.
      return;
    }

    const isStop = flag & Ci.nsIWebProgressListener.STATE_STOP;
    if (isStop && frame._pendingNavigationId && status) {
      // FF150 Fission workaround: NS_BINDING_ABORTED on the main frame is
      // SPURIOUS during cross-process BC swap. The old BC's DocLoader stops
      // (firing STATE_STOP with status=NS_BINDING_ABORTED) before the new BC
      // takes over and commits the same navigation. Suppressing this abort
      // lets _frameNavigationCommitted on the new BC fire correctly.
      // Real network failures use other status codes (e.g. NS_ERROR_*) and
      // still propagate normally.
      if (frame === this._mainFrame && status === Cr.NS_BINDING_ABORTED)
        return;
      // Navigation is aborted.
      const navigationId = frame._pendingNavigationId;
      frame._pendingNavigationId = null;
      // Always report download navigation as failure to match other browsers.
      const errorText = helper.getNetworkErrorStatusText(status);
      this.emit(FrameTree.Events.NavigationAborted, frame, navigationId, errorText);
      if (frame === this._mainFrame && status !== Cr.NS_BINDING_ABORTED)
        this.forcePageReady();
    }
  }

  onLocationChange(progress, request, location, flags) {
    const docShell = progress.DOMWindow.docShell;
    const frame = this.frameForDocShell(docShell);
    const sameDocumentNavigation = !!(flags & Ci.nsIWebProgressListener.LOCATION_CHANGE_SAME_DOCUMENT);
    if (frame && sameDocumentNavigation) {
      frame._url = location.spec;
      this.emit(FrameTree.Events.SameDocumentNavigation, frame);
    }
  }

  _onBrowsingContextAttached(browsingContext) {
    // If this browsing context doesn't belong to our frame tree - do nothing.
    if (browsingContext.top !== this._rootBrowsingContext)
      return;
    this._createFrame(browsingContext);
  }

  _onBrowsingContextDetached(browsingContext) {
    const frame = this.frameForBrowsingContext(browsingContext);
    if (frame)
      this._detachFrame(frame);
  }

  _createFrame(browsingContext) {
    const parentFrame = this.frameForBrowsingContext(browsingContext.parent);
    if (!parentFrame && this._mainFrame) {
      dump(`WARNING: found docShell with the same root, but no parent!\n`);
      return;
    }
    const frame = new Frame(this, this._runtime, browsingContext, parentFrame);
    this._frameIdToFrame.set(frame.id(), frame);
    if (browsingContext.docShell?.domWindow && browsingContext.docShell?.domWindow.location)
      frame._url = browsingContext.docShell.domWindow.location.href;
    this.emit(FrameTree.Events.FrameAttached, frame);
    // Create execution context **after** reporting frame.
    // This is our protocol contract.
    if (frame.domWindow())
      frame._onGlobalObjectCleared();
    return frame;
  }

  _detachFrame(frame) {
    // Detach all children first
    for (const subframe of frame._children)
      this._detachFrame(subframe);
    if (frame === this._mainFrame) {
      // Do not detach main frame (happens during cross-process navigation),
      // as it confuses the client.
      return;
    }
    this._frameIdToFrame.delete(frame.id());
    if (frame._parentFrame)
      frame._parentFrame._children.delete(frame);
    frame._parentFrame = null;
    frame.dispose();
    this.emit(FrameTree.Events.FrameDetached, frame);
  }
}

FrameTree.Events = {
  FrameAttached: 'frameattached',
  FrameDetached: 'framedetached',
  WorkerCreated: 'workercreated',
  WorkerDestroyed: 'workerdestroyed',
  WebSocketCreated: 'websocketcreated',
  WebSocketOpened: 'websocketopened',
  WebSocketClosed: 'websocketclosed',
  WebSocketFrameReceived: 'websocketframereceived',
  WebSocketFrameSent: 'websocketframesent',
  NavigationStarted: 'navigationstarted',
  NavigationCommitted: 'navigationcommitted',
  NavigationAborted: 'navigationaborted',
  SameDocumentNavigation: 'samedocumentnavigation',
  PageReady: 'pageready',
  InputEvent: 'inputevent',
};

class IsolatedWorld {
  constructor(name) {
    this._name = name;
    this._scriptsToEvaluateOnNewDocument = [];
    this._bindings = new Map();
  }
}

class Frame {
  constructor(frameTree, runtime, browsingContext, parentFrame) {
    this._frameTree = frameTree;
    this._runtime = runtime;
    this._browsingContext = browsingContext;
    this._children = new Set();
    this._frameId = helper.browsingContextToFrameId(browsingContext);
    this._parentFrame = null;
    this._url = '';
    if (parentFrame) {
      this._parentFrame = parentFrame;
      parentFrame._children.add(this);
    }

    this._lastCommittedNavigationId = null;
    this._pendingNavigationId = null;

    this._textInputProcessor = null;

    this._worldNameToContext = new Map();
    this._initialNavigationDone = false;

    this._webSocketListenerInnerWindowId = 0;
    // WebSocketListener calls frameReceived event before webSocketOpened.
    // To avoid this, serialize event reporting.
    this._webSocketInfos = new Map();

    const dispatchWebSocketFrameReceived = (webSocketSerialID, frame) => this._frameTree.emit(FrameTree.Events.WebSocketFrameReceived, {
      frameId: this._frameId,
      wsid: webSocketSerialID + '',
      opcode: frame.opCode,
      data: frame.opCode !== 1 ? btoa(frame.payload) : frame.payload,
    });
    this._webSocketListener = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIWebSocketEventListener, ]),

      webSocketCreated: (webSocketSerialID, uri, protocols) => {
        this._frameTree.emit(FrameTree.Events.WebSocketCreated, {
          frameId: this._frameId,
          wsid: webSocketSerialID + '',
          requestURL: uri,
        });
        this._webSocketInfos.set(webSocketSerialID, {
          opened: false,
          pendingIncomingFrames: [],
        });
      },

      webSocketOpened: (webSocketSerialID, effectiveURI, protocols, extensions, httpChannelId) => {
        this._frameTree.emit(FrameTree.Events.WebSocketOpened, {
          frameId: this._frameId,
          requestId: httpChannelId + '',
          wsid: webSocketSerialID + '',
          effectiveURL: effectiveURI,
        });
        const info = this._webSocketInfos.get(webSocketSerialID);
        info.opened = true;
        for (const frame of info.pendingIncomingFrames)
          dispatchWebSocketFrameReceived(webSocketSerialID, frame);
      },

      webSocketMessageAvailable: (webSocketSerialID, data, messageType) => {
        // We don't use this event.
      },

      webSocketClosed: (webSocketSerialID, wasClean, code, reason) => {
        this._webSocketInfos.delete(webSocketSerialID);
        let error = '';
        if (!wasClean) {
          const keys = Object.keys(Ci.nsIWebSocketChannel);
          for (const key of keys) {
            if (Ci.nsIWebSocketChannel[key] === code)
              error = key;
          }
        }
        this._frameTree.emit(FrameTree.Events.WebSocketClosed, {
          frameId: this._frameId,
          wsid: webSocketSerialID + '',
          error,
        });
      },

      frameReceived: (webSocketSerialID, frame) => {
        // Report only text and binary frames.
        if (frame.opCode !== 1 && frame.opCode !== 2)
          return;
        const info = this._webSocketInfos.get(webSocketSerialID);
        if (info.opened)
          dispatchWebSocketFrameReceived(webSocketSerialID, frame);
        else
          info.pendingIncomingFrames.push(frame);
      },

      frameSent: (webSocketSerialID, frame) => {
        // Report only text and binary frames.
        if (frame.opCode !== 1 && frame.opCode !== 2)
          return;
        this._frameTree.emit(FrameTree.Events.WebSocketFrameSent, {
          frameId: this._frameId,
          wsid: webSocketSerialID + '',
          opcode: frame.opCode,
          data: frame.opCode !== 1 ? btoa(frame.payload) : frame.payload,
        });
      },
    };
  }

  // STEALTHFOX: l'UNICO punto che sa come si costruisce un mondo. I due mondi
  // differiscono per una bandiera sola, e prima erano due blocchi separati -
  // uno qui e uno inline in _initializeWorlds - quindi la differenza fra loro
  // era invisibile a chi leggeva.
  //
  // `wantXrays` decide COSA il mondo vede degli oggetti del contenuto:
  //   true  = la vista NATIVA. Gli expando che la pagina appiccica a window e
  //           ai nodi sono nascosti, e i getter che la pagina rimpiazza non
  //           vengono invocati. E' cio' che serve al MACCHINARIO (i selettori,
  //           l'actionability): un sito che rimpiazza Element.prototype.matches
  //           non deve poterlo sviare.
  //   false = la vista del CONTENUTO, cioe' esattamente quello che vede il
  //           sito. E' cio' che serve al CODICE DELL'UTENTE, che chiede
  //           `page.evaluate(() => window.__APP_STATE__)` e si aspetta il
  //           valore del sito, non undefined.
  //
  // ⛔ E LA BANDIERA DA SOLA NON BASTA, misurato il 2026-08-23: una sandbox con
  // `wantXrays: false` ma con l'ExpandedPrincipal continua a vedere la vista
  // Xray, perche' il principal della sandbox non e' quello del sito. In quella
  // configurazione `page.evaluate(() => window.__STATO)` torna `undefined`,
  // `document.title` torna il titolo nativo invece di quello che il getter
  // della pagina produce, e le funzioni della pagina risultano "is not a
  // function". **A decidere e' il PRINCIPAL**, non la bandiera: con il
  // principal della pagina la vista del contenuto si apre da sola, e infatti il
  // mondo script non ha bisogno di nessun ponte.
  //
  // ⛔ E QUESTA FUNZIONE E' UNA SOLA APPOSTA. Il 2026-08-23 ne era stata
  // aggiunta una seconda per il mondo script, che differiva per due argomenti:
  // esattamente i due blocchi separati che il commento qui sopra racconta di
  // aver eliminato una volta. Rifatta com'era: un posto solo, e la differenza
  // fra i mondi si legge negli ARGOMENTI, dove e' visibile.
  //
  //   principal `[dw]` (esteso) + xray true  -> utility, serializzazione
  //   principal `dw`            + xray false -> mondo script
  // ⛔ IL MONDO SCRIPT, e perche' e' un DISEGNO e non una variante.
  //
  // Il conflitto che nessuna membrana poteva chiudere: l'invariante dei Proxy
  // impone di restituire il valore CRUDO su una proprieta' non falsificabile,
  // ma una funzione cruda chiamata con il Proxy come `this` viene rifiutata da
  // WebIDL. Non e' un difetto dell'implementazione: "la pagina non deve
  // vedermi" e "la pagina deve poter usare le mie cose" sono LA STESSA
  // manopola, il principal, girata nei due versi.
  //
  // Qui si smette di girarla in tutti e due i versi nello stesso mondo: il
  // mondo main prende il principal DELLA PAGINA, quindi niente Xray, niente
  // waiver, niente conversione - tutto e' trasparente - e il codice utente
  // resta comunque invisibile alla pagina, perche' il GLOBAL e' un altro.
  // Misurato: un global dichiarato per sbaglio da un init script oggi la pagina
  // lo vede, col mondo script no; e su undici banchi il comportamento e'
  // identico riga per riga.
  //
  // Il prezzo, dichiarato: senza ExpandedPrincipal non si vedono le shadow root
  // CHIUSE. Restano nel mondo utility, dove i locator gia' le vedono.

  _creaMondo(esteso, xray) {
    const dw = this.domWindow();
    return Cu.Sandbox(esteso ? [dw] : dw, {
      sandboxPrototype: dw,
      wantComponents: false,
      wantExportHelpers: false,
      wantXrays: xray,
    });
  }

  // Il MESTIERE del mondo si passa per nome ('main' / 'isolato'), non
  // codificato dentro `wantXrays` con `null` a significare "e' il main":
  // era un valore usato per dire un'altra cosa, e costringeva chi legge a
  // ricordare una convenzione invece di leggere un nome.
  //
  // Il quarto argomento e' il global in cui si SERIALIZZANO i valori di ritorno,
  // e non e' mai quello della pagina: il perche', con i numeri, sta nel
  // costruttore di ExecutionContext in Runtime.js. E' una sandbox sola per
  // frame, condivisa da tutti i mondi, creata pigramente perche' un mondo che
  // non serializza non deve pagarla.
  // Quale global ospita un mondo. Il parametro dice il MESTIERE, non la
  // configurazione: la configurazione e' la riga sotto ciascun mestiere, e si
  // legge tutta insieme.
  _globalPerMondo(tipo) {
    if (tipo === 'isolato')
      return this._creaMondo(true /* principal esteso */, true /* xray */);
    // 'main' e' il mondo del codice dell'UTENTE.
    return FrameTree.mondoScript()
        ? this._creaMondo(false /* principal della pagina */, false /* niente xray */)
        : this.domWindow();
  }

  _createWorldContext(name, tipo) {
    const global = this._globalPerMondo(tipo);
    const world = this._runtime.createExecutionContext(this.domWindow(), global, {
      frameId: this.id(),
      name,
    // ⛔ La sandbox di serializzazione RESTA anche col mondo script. Sembra
    // superflua - il mondo script non e' il realm della pagina - ma il valore
    // reso puo' essere un oggetto DELLA PAGINA (`() => window.qualcosa`), e
    // allora `JSON.stringify` ne percorre la catena di prototipi e chiama il
    // suo `toJSON`. La sandbox di serializzazione guarda con l'Xray, che quella
    // catena non la espone: e' §5.2x, e vale ancora.
    }, () => this._ensureSerializationSandbox());
    this._worldNameToContext.set(name, world);
    return world;
  }

  _createIsolatedContext(name) {
    return this._createWorldContext(name, 'isolato');
  }

  _ensureSerializationSandbox() {
    if (!this._serializationSandbox)
      this._serializationSandbox = this._creaMondo(true, true);
    return this._serializationSandbox;
  }

  unsafeObject(objectId) {
    for (const context of this._worldNameToContext.values()) {
      const result = context.unsafeObject(objectId);
      if (result)
        return result.object;
    }
    throw new Error('Cannot find object with id = ' + objectId);
  }

  dispose() {
    for (const context of this._worldNameToContext.values())
      this._runtime.destroyExecutionContext(context);
    this._worldNameToContext.clear();
    // la sandbox di serializzazione appartiene al documento, come i contesti
    this._serializationSandbox = null;
  }

  _addBinding(worldName, name, script) {
    let executionContext = this._worldNameToContext.get(worldName);
    if (worldName && !executionContext)
      executionContext = this._createIsolatedContext(worldName);
    if (executionContext)
      executionContext.addBinding(name, script);
  }

  _onGlobalObjectCleared() {
    const webSocketService = this._frameTree._webSocketEventService;
    if (this._webSocketListenerInnerWindowId && webSocketService.hasListenerFor(this._webSocketListenerInnerWindowId))
      webSocketService.removeListener(this._webSocketListenerInnerWindowId, this._webSocketListener);

    // Stealthfox issue #18: during rapid iframe create/detach bursts (e.g. a
    // telemetry SDK's session-start sequence), this handler can fire for a
    // frame whose BrowsingContext is mid-teardown: `domWindow()` then
    // returns undefined and unguarded `.windowGlobalChild.innerWindowId`
    // threw TypeError, corrupting FrameTree and triggering page disposal.
    const dw = this.domWindow();
    if (!dw || !dw.windowGlobalChild) {
      this._webSocketListenerInnerWindowId = 0;
      return;
    }
    this._webSocketListenerInnerWindowId = dw.windowGlobalChild.innerWindowId;
    webSocketService.addListener(this._webSocketListenerInnerWindowId, this._webSocketListener);

    for (const context of this._worldNameToContext.values())
      this._runtime.destroyExecutionContext(context);
    this._worldNameToContext.clear();
    // la sandbox di serializzazione appartiene al documento, come i contesti
    this._serializationSandbox = null;

    // ⛔ IL MONDO "main" E' IL GLOBAL DELLA PAGINA, E SPOSTARLO IN UNA SANDBOX
    // NON E' UNA BANDIERA: PROVATO IL 2026-08-23, MISURE IN 90-firefox-internals.md.
    //
    // Il secondo argomento e' il GLOBAL in cui il codice gira. Passando la
    // window, `page.evaluate()` gira nel compartimento del sito. Sostituirlo con
    // una sandbox isola davvero - misurato, cio' che l'automazione dichiara
    // diventa invisibile alla pagina - ma rompe il contratto di `evaluate`, e
    // rimetterlo in piedi costa piu' di una riga:
    //
    //   - `wantXrays: false` NON apre la vista del contenuto: il principal della
    //     sandbox non e' quello del sito, quindi Gecko tiene l'Xray lo stesso.
    //   - nemmeno passare la window WAIVED come `sandboxPrototype` la apre: il
    //     waiver non sopravvive come legame strutturale, viene ri-avvolto al
    //     confine del compartimento.
    //   - il waiver funziona solo come VALORE esplicito, `window.wrappedJSObject`,
    //     ed e' appiccicoso: due salti, gli expando sui nodi e i getter della
    //     pagina continuano a funzionare da li'.
    //   - restano due rotture vere: un identificatore nudo della pagina
    //     (`page.evaluate(() => miaFunzioneGlobale())`) non risolve piu', e un
    //     oggetto creato NELLA sandbox passato a codice della pagina viene
    //     rifiutato con "Permission denied to access property". Il secondo non e'
    //     un difetto: e' la stessa barriera che rende invisibili i nostri global.
    this._createWorldContext('', 'main');
    for (const [name, world] of this._frameTree._isolatedWorlds) {
      if (name)
        this._createIsolatedContext(name);
      const executionContext = this._worldNameToContext.get(name);
      // Add bindings before evaluating scripts.
      for (const [name, script] of world._bindings)
        executionContext.addBinding(name, script);
      for (const script of world._scriptsToEvaluateOnNewDocument)
        executionContext.evaluateScriptSafely(script);
    }

    const url = this.domWindow().location?.href;
    if (url === 'about:blank' && !this._url) {
      // Sometimes FrameTree is created too early, before the location has been set.
      this._url = url;
      this._frameTree.emit(FrameTree.Events.NavigationCommitted, this);
    }

    this._updateJavaScriptDisabled();
  }

  _updateJavaScriptDisabled() {
    if (this._browsingContext.currentWindowContext)
      this._browsingContext.currentWindowContext.allowJavascript = !this._frameTree._javaScriptDisabled;
  }

  mainExecutionContext() {
    return this._worldNameToContext.get('');
  }

  textInputProcessor() {
    if (!this._textInputProcessor) {
      this._textInputProcessor = Cc["@mozilla.org/text-input-processor;1"].createInstance(Ci.nsITextInputProcessor);
    }
    this._textInputProcessor.beginInputTransactionForTests(this.docShell().DOMWindow);
    return this._textInputProcessor;
  }

  pendingNavigationId() {
    return this._pendingNavigationId;
  }

  lastCommittedNavigationId() {
    return this._lastCommittedNavigationId;
  }

  docShell() {
    return this._browsingContext.docShell;
  }

  domWindow() {
    return this.docShell()?.domWindow;
  }

  name() {
    const frameElement = this.domWindow()?.frameElement;
    let name = '';
    if (frameElement)
      name = frameElement.getAttribute('name') || frameElement.getAttribute('id') || '';
    return name;
  }

  parentFrame() {
    return this._parentFrame;
  }

  id() {
    return this._frameId;
  }

  url() {
    return this._url;
  }

}

class Worker {
  constructor(frame, workerDebugger) {
    this._frame = frame;
    this._workerId = helper.generateId();
    this._workerDebugger = workerDebugger;

    workerDebugger.initialize('chrome://juggler/content/content/WorkerMain.js');

    this._channel = new SimpleChannel(`content::worker[${this._workerId}]`, 'worker-' + this._workerId);
    this._channel.setTransport({
      sendMessage: obj => workerDebugger.postMessage(JSON.stringify(obj)),
      dispose: () => {},
    });
    this._workerDebuggerListener = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIWorkerDebuggerListener]),
      onMessage: msg => void this._channel._onMessage(JSON.parse(msg)),
      onClose: () => void this._channel.dispose(),
      onError: (filename, lineno, message) => {
        dump(`WARNING: Error in worker: ${message} @${filename}:${lineno}\n`);
      },
    };
    workerDebugger.addListener(this._workerDebuggerListener);
  }

  channel() {
    return this._channel;
  }

  frame() {
    return this._frame;
  }

  id() {
    return this._workerId;
  }

  url() {
    return this._workerDebugger.url;
  }

  dispose() {
    this._channel.dispose();
    this._workerDebugger.removeListener(this._workerDebuggerListener);
  }
}

function channelId(channel) {
  if (channel instanceof Ci.nsIIdentChannel) {
    const identChannel = channel.QueryInterface(Ci.nsIIdentChannel);
    return String(identChannel.channelId);
  }
  return helper.generateId();
}


