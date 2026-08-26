/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";
// Note: this file should be loadabale with eval() into worker environment.
// Avoid Components.*, ChromeUtils and global const variables.

if (!this.Debugger) {
  // Worker has a Debugger defined already.
  const {addDebuggerToGlobal} = ChromeUtils.importESModule("resource://gre/modules/jsdebugger.sys.mjs");
  addDebuggerToGlobal(Components.utils.getGlobalForObject(globalThis));
}

let lastId = 0;
function generateId() {
  return 'id-' + (++lastId);
}

// Stealth: JSON stringify helper source, re-evaluated on debugger re-attach.
//
// ⛔ SI LEGGE E SI RISCRIVE IL DESCRITTORE, MAI LA PROPRIETA'. MISURATO, NON
// SUPPOSTO. La versione precedente faceva:
//     const oldToJSON = Date.prototype?.toJSON;   // LETTURA   -> chiama il getter
//     Date.prototype.toJSON = undefined;          // SCRITTURA -> chiama il setter
//     ... Date.prototype.toJSON = oldToJSON;      // SCRITTURA -> chiama il setter
// Se la PAGINA ha ridefinito `Date.prototype.toJSON` come accessore, quelle tre
// operazioni entrano nel suo codice. La trappola sta in due righe:
//     Object.defineProperty(Date.prototype, 'toJSON', {get(){...}, set(v){...}})
// Misura 2026-08-19: registra `get, set:undefined, set:function` per OGNI
// `evaluate` che ritorna un valore e ZERO per una che non serializza; sei scatti
// dopo la prima, dodici dopo la seconda. Si accumula a ogni giro.
//
// `getOwnPropertyDescriptor` non invoca il getter e `defineProperty` non invoca
// il setter, quindi nessuna delle tre operazioni tocca piu' il codice della
// pagina. Il comportamento non cambia: durante la serializzazione `toJSON` e'
// una proprieta' dato con valore `undefined` come prima, e alla fine il
// descrittore originale torna identico, accessore compreso.
//
// Le tre native si catturano al momento della `bind`, cosi' un aggancio
// installato DOPO non le intercetta. Residuo dichiarato: una pagina che
// agganci `Object.defineProperty` PRIMA ci vedrebbe comunque - e' un aggancio
// molto piu' invasivo, che rompe siti veri, e non e' stato misurato.
const _STEALTH_JSON_STRINGIFY_SRC = `((stringify, getDesc, defProp, object) => {
  const DESC_VUOTO = {value: undefined, writable: true, enumerable: false, configurable: true};
  const dDate = getDesc(Date.prototype, 'toJSON');
  if (dDate)
    defProp(Date.prototype, 'toJSON', DESC_VUOTO);
  const dArray = getDesc(Array.prototype, 'toJSON');
  if (dArray)
    defProp(Array.prototype, 'toJSON', DESC_VUOTO);

  let hasSymbol = false;
  const result = stringify(object, (key, value) => {
    if (typeof value === 'symbol')
      hasSymbol = true;
    return value;
  });

  if (dDate)
    defProp(Date.prototype, 'toJSON', dDate);
  if (dArray)
    defProp(Array.prototype, 'toJSON', dArray);

  return hasSymbol ? undefined : result;
}).bind(null, JSON.stringify.bind(JSON), Object.getOwnPropertyDescriptor, Object.defineProperty)`;

const consoleLevelToProtocolType = {
  'dir': 'dir',
  'log': 'log',
  'debug': 'debug',
  'info': 'info',
  'error': 'error',
  'warn': 'warning',
  'dirxml': 'dirxml',
  'table': 'table',
  'trace': 'trace',
  'clear': 'clear',
  'group': 'startGroup',
  'groupCollapsed': 'startGroupCollapsed',
  'groupEnd': 'endGroup',
  'assert': 'assert',
  'profile': 'profile',
  'profileEnd': 'profileEnd',
  'count': 'count',
  'countReset': 'countReset',
  'time': null,
  'timeLog': 'timeLog',
  'timeEnd': 'timeEnd',
  'timeStamp': 'timeStamp',
};

const disallowedMessageCategories = new Set([
  'XPConnect JavaScript',
  'component javascript',
  'chrome javascript',
  'chrome registration',
  'XBL',
  'XBL Prototype Handler',
  'XBL Content Sink',
  'xbl javascript',
]);

class Runtime {
  constructor(isWorker = false) {
    this._debugger = new Debugger();
    // Stealth: skip Realm debug mode activation so the JS engine runs at full
    // speed (Ion JIT enabled, no slowPathOnNativeCall overhead). The Debugger
    // still tracks globals and executeInGlobal works — only the performance-
    // degrading debug instrumentation is skipped.
    this._debugger.stealthMode = true;
    this._pendingPromises = new Map();
    this._executionContexts = new Map();
    this._windowToExecutionContext = new Map();
    this._eventListeners = [];
    if (isWorker) {
      this._registerWorkerConsoleHandler();
    } else {
      this._registerConsoleServiceListener(Services);
      this._registerConsoleAPIListener(Services);
    }
    // We can't use event listener here to be compatible with Worker Global Context.
    // Use plain callbacks instead.
    this.events = {
      onConsoleMessage: createEvent(),
      onRuntimeError: createEvent(),
      onErrorFromWorker: createEvent(),
      onExecutionContextCreated: createEvent(),
      onExecutionContextDestroyed: createEvent(),
      onBindingCalled: createEvent(),
    };
  }

  executionContexts() {
    return [...this._executionContexts.values()];
  }

  async evaluate({executionContextId, expression, returnByValue}) {
    const executionContext = this.findExecutionContext(executionContextId);
    if (!executionContext)
      throw new Error('Failed to find execution context with id = ' + executionContextId);
    const exceptionDetails = {};
    let result = await executionContext.evaluateScript(expression, exceptionDetails);
    if (!result)
      return {exceptionDetails};
    if (returnByValue)
      result = executionContext.ensureSerializedToValue(result);
    return {result};
  }

  async callFunction({executionContextId, functionDeclaration, args, returnByValue}) {
    const executionContext = this.findExecutionContext(executionContextId);
    if (!executionContext)
      throw new Error('Failed to find execution context with id = ' + executionContextId);
    const exceptionDetails = {};
    let result = await executionContext.evaluateFunction(functionDeclaration, args, exceptionDetails);
    if (!result)
      return {exceptionDetails};
    if (returnByValue)
      result = executionContext.ensureSerializedToValue(result);
    return {result};
  }

  async getObjectProperties({executionContextId, objectId}) {
    const executionContext = this.findExecutionContext(executionContextId);
    if (!executionContext)
      throw new Error('Failed to find execution context with id = ' + executionContextId);
    return {properties: executionContext.getObjectProperties(objectId)};
  }

  async disposeObject({executionContextId, objectId}) {
    const executionContext = this.findExecutionContext(executionContextId);
    if (!executionContext)
      throw new Error('Failed to find execution context with id = ' + executionContextId);
    return executionContext.disposeObject(objectId);
  }

  _registerConsoleServiceListener(Services) {
    const Ci = Components.interfaces;
    const consoleServiceListener = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIConsoleListener]),

      observe: message => {
        if (!(message instanceof Ci.nsIScriptError) || !message.outerWindowID ||
            !message.category || disallowedMessageCategories.has(message.category)) {
          return;
        }
        const errorWindow = Services.wm.getOuterWindowWithId(message.outerWindowID);
        // Note: error locations are one-based, while console locations are zero-based in
        // Firefox.  We want to report all of them as zero-based.  Newer Playwright
        // clients read `pageError.location.url` strictly and crash when location
        // is undefined (issue #13).
        const errorLocation = {
          lineNumber: message.lineNumber - 1,
          columnNumber: message.columnNumber - 1,
          url: message.sourceName,
        };
        if (message.category === 'Web Worker' && message.logLevel === Ci.nsIConsoleMessage.error) {
          emitEvent(this.events.onErrorFromWorker, errorWindow, message.message, '' + message.stack, errorLocation);
          return;
        }
        const executionContext = this._windowToExecutionContext.get(errorWindow);
        if (!executionContext) {
          return;
        }
        const typeNames = {
          [Ci.nsIConsoleMessage.debug]: 'debug',
          [Ci.nsIConsoleMessage.info]: 'info',
          [Ci.nsIConsoleMessage.warn]: 'warn',
          [Ci.nsIConsoleMessage.error]: 'error',
        };
        if (!message.hasException) {
          emitEvent(this.events.onConsoleMessage, {
            args: [{
              value: message.message,
            }],
            type: typeNames[message.logLevel],
            executionContextId: executionContext.id(),
            location: {
              lineNumber: message.lineNumber,
              columnNumber: message.columnNumber,
              url: message.sourceName,
            },
          });
        } else {
          emitEvent(this.events.onRuntimeError, {
            executionContext,
            message: message.errorMessage,
            stack: message.stack ? message.stack.toString() : '',
            location: errorLocation,
          });
        }
      },
    };
    Services.console.registerListener(consoleServiceListener);
    this._eventListeners.push(() => Services.console.unregisterListener(consoleServiceListener));
  }

  _registerConsoleAPIListener(Services) {
    const Ci = Components.interfaces;
    const Cc = Components.classes;
    const ConsoleAPIStorage = Cc["@mozilla.org/consoleAPI-storage;1"].getService(Ci.nsIConsoleAPIStorage);
    const onMessage = ({ wrappedJSObject }) => {
      const executionContext = Array.from(this._executionContexts.values()).find(context => {
        // There is no easy way to determine isolated world context and we normally don't write
        // objects to console from utility worlds so we always return main world context here.
        if (context._isIsolatedWorldContext())
          return false;
        const domWindow = context._domWindow;
        try {
          // `windowGlobalChild` might be dead already; accessing it will throw an error, message in a console,
          // and infinite recursion.
          return domWindow && domWindow.windowGlobalChild.innerWindowId === wrappedJSObject.innerID;
        } catch (e) {
          return false;
        }
      });
      if (!executionContext)
        return;
      this._onConsoleMessage(executionContext, wrappedJSObject);
    }
    ConsoleAPIStorage.addLogEventListener(
      onMessage,
      Cc["@mozilla.org/systemprincipal;1"].createInstance(Ci.nsIPrincipal)
    );
    this._eventListeners.push(() => ConsoleAPIStorage.removeLogEventListener(onMessage));
  }

  _registerWorkerConsoleHandler() {
    setConsoleEventHandler(message => {
      const executionContext = Array.from(this._executionContexts.values())[0];
      this._onConsoleMessage(executionContext, message);
    });
    this._eventListeners.push(() => setConsoleEventHandler(null));
  }

  _onConsoleMessage(executionContext, message) {
    const type = consoleLevelToProtocolType[message.level];
    if (!type)
      return;
    const args = message.arguments.map(arg => executionContext.rawValueToRemoteObject(arg));
    emitEvent(this.events.onConsoleMessage, {
      args,
      type,
      executionContextId: executionContext.id(),
      location: {
        lineNumber: message.lineNumber - 1,
        columnNumber: message.columnNumber - 1,
        url: message.filename,
      },
    });
  }

  dispose() {
    for (const tearDown of this._eventListeners)
      tearDown.call(null);
    this._eventListeners = [];
  }

  async _awaitPromise(executionContext, obj, exceptionDetails = {}) {
    if (obj.promiseState === 'fulfilled')
      return {success: true, obj: obj.promiseValue};
    if (obj.promiseState === 'rejected') {
      const debuggee = executionContext._debuggee;
      exceptionDetails.text = debuggee.executeInGlobalWithBindings('e.message', {e: obj.promiseReason}, {useInnerBindings: true}).return;
      exceptionDetails.stack = debuggee.executeInGlobalWithBindings('e.stack', {e: obj.promiseReason}, {useInnerBindings: true}).return;
      return {success: false, obj: null};
    }
    // Stealth: onPromiseSettled requires Realm::isDebuggee() which we skip in
    // stealth mode. Track settlement via .then() on the content promise instead.
    // Also register in _pendingPromises so destroyExecutionContext can reject
    // if the page navigates before the promise settles (prevents hang).
    let resolve, reject;
    const promise = new Promise((a, b) => { resolve = a; reject = b; });
    const promiseID = obj.promiseID || generateId();
    this._pendingPromises.set(promiseID, {resolve: null, reject, executionContext, exceptionDetails});

    const rawPromise = obj.unsafeDereference();
    // NIENTE `Cu.waiveXrays` QUI, ED E' LA DIFFERENZA FRA UN `then` NATIVO E
    // QUELLO DELLA PAGINA.
    //
    // Qui c'era `Cu.waiveXrays(rawPromise).then(...)`. Il waiver toglie l'Xray,
    // quindi `.then` si risolveva sulla catena di prototipi VERA del sito: se
    // una pagina rimpiazza `Promise.prototype.then`, il suo codice veniva
    // eseguito e poteva CONTARE. Misurato il 2026-08-23 con
    // `tests/gates/observable_crossings.py`: una `page.evaluate` asincrona
    // faceva scattare la trappola **1 volta su 1**, ed era l'unico
    // attraversamento rimasto dell'intero flusso.
    //
    // Senza waiver, `rawPromise` resta dietro l'Xray e `.then` risolve al
    // metodo NATIVO, che il sito non puo' sviare. Il comportamento non cambia:
    // `Promise.prototype.then` opera sullo slot interno, non sul prototipo.
    // Verificato su 7 casi - valore, ritardo, oggetto, promise gia' risolta,
    // catena di await, fetch, e il RIFIUTO col messaggio intatto - 7 su 7, e il
    // gate torna a zero attraversamenti.
    rawPromise.then(
      () => {
        this._pendingPromises.delete(promiseID);
        resolve({success: true, obj: obj.promiseValue});
      },
      () => {
        this._pendingPromises.delete(promiseID);
        try {
          const debuggee = executionContext._debuggee;
          exceptionDetails.text = debuggee.executeInGlobalWithBindings('e.message', {e: obj.promiseReason}, {useInnerBindings: true}).return;
          exceptionDetails.stack = debuggee.executeInGlobalWithBindings('e.stack', {e: obj.promiseReason}, {useInnerBindings: true}).return;
        } catch(e) {}
        resolve({success: false, obj: null});
      }
    );
    return await promise;
  }

  createExecutionContext(domWindow, contextGlobal, auxData, serializationGlobal) {
    // Note: domWindow is null for workers.
    const context = new ExecutionContext(this, domWindow, contextGlobal, auxData, serializationGlobal);
    this._executionContexts.set(context._id, context);
    if (domWindow)
      this._windowToExecutionContext.set(domWindow, context);
    emitEvent(this.events.onExecutionContextCreated, context);
    return context;
  }

  findExecutionContext(executionContextId) {
    const executionContext = this._executionContexts.get(executionContextId);
    if (!executionContext)
      throw new Error('Failed to find execution context with id = ' + executionContextId);
    return executionContext;
  }

  destroyExecutionContext(destroyedContext) {
    for (const [promiseID, {reject, executionContext}] of this._pendingPromises) {
      if (executionContext === destroyedContext) {
        reject(new Error('Execution context was destroyed!'));
        this._pendingPromises.delete(promiseID);
      }
    }
    this._debugger.removeDebuggee(destroyedContext._contextGlobal);
    if (destroyedContext._serializationGlobal)
      this._debugger.removeDebuggee(destroyedContext._serializationGlobal);
    this._executionContexts.delete(destroyedContext._id);
    if (destroyedContext._domWindow)
      this._windowToExecutionContext.delete(destroyedContext._domWindow);
    emitEvent(this.events.onExecutionContextDestroyed, destroyedContext);
  }
}

class ExecutionContext {
  constructor(runtime, domWindow, contextGlobal, auxData, serializationGlobal) {
    this._runtime = runtime;
    this._domWindow = domWindow;
    this._contextGlobal = contextGlobal;
    this._debuggee = runtime._debugger.addDebuggee(contextGlobal);
    // ⛔ DOVE SI SERIALIZZA E' UNA DECISIONE, NON UN DETTAGLIO.
    //
    // Il valore di ritorno di `evaluate` veniva serializzato NEL REALM DELLA
    // PAGINA, e `JSON.stringify` legge `toJSON` su ogni valore che tocca: se il
    // sito ha messo un accessore su `Object.prototype.toJSON`, quelle letture
    // sono sue e le puo' CONTARE. Misurato il 2026-08-23 su un flusso normale:
    // click, `page.title()`, `page.content()`, i locator e una `evaluate` che
    // torna un numero fanno **zero** attraversamenti; una `evaluate` che torna
    // `{a:1, b:[1,2,3]}` ne fa **sei** su `toJSON` piu' **due** su
    // `Object.defineProperty`. Erano l'unico attraversamento osservabile
    // dell'intero flusso.
    //
    // I due `defineProperty` erano gia' un residuo DICHIARATO nel commento di
    // `_STEALTH_JSON_STRINGIFY_SRC` ("una pagina che agganci Object.defineProperty
    // PRIMA ci vedrebbe comunque - non e' stato misurato"). Adesso e' misurato.
    //
    // Il rimedio non e' spostare `evaluate`: e' serializzare in un global che
    // NON e' quello del sito. Da li' un oggetto della pagina si legge attraverso
    // un Xray, che non espone la catena dei prototipi, quindi
    // `Object.prototype.toJSON` del sito non esiste per chi serializza e il
    // getter non parte. `wantXrays` qui non e' una scelta: e' il meccanismo.
    //
    // Nei worker `serializationGlobal` e' assente e si ricade sul global del
    // contesto, che nei worker e' gia' l'unico che c'e'.
    // ⛔ SI COSTRUISCE ALLA PRIMA SERIALIZZAZIONE, NON A OGNI FRAME.
    //
    // Prima il global arrivava gia' costruito, perche' era un ARGOMENTO: quindi
    // una sandbox in piu' nasceva per ogni frame di ogni documento, anche dove
    // nessuno serializzava mai niente. Su una pagina con cinquanta iframe sono
    // cinquanta global pagati per nulla. Adesso arriva una FABBRICA e si chiama
    // al primo uso vero.
    //
    // `_ensureJsonStringify` era gia' pigra per una ragione diversa e piu'
    // stretta - non toccare gli intrinseci prima del primo script della pagina -
    // ma la sandbox sotto di lei non lo era, quindi meta' del costo restava.
    // ⛔ E' SEMPRE UNA FABBRICA, mai un global gia' costruito. La prima stesura
    // accettava tutti e due e distingueva con `typeof === 'function'`: un
    // parametro che significa due cose diverse a seconda del tipo e' esattamente
    // cio' che questo progetto ha gia' pagato una volta - `push_range` che
    // tornava uno o tre token senza dirlo - e prima o poi qualcuno lo legge
    // nell'altro senso. I worker non ne passano nessuna e ricadono sul proprio
    // global, che li' e' l'unico che esista.
    this._fabbricaSerializzazione = serializationGlobal || null;
    this._serializationGlobal = null;
    this._serializationDebuggeeCache = null;
    this._remoteObjects = new Map();
    this._id = generateId();
    this._auxData = auxData;
    // STEALTHFOX: do NOT run any code in the page realm before the page's own
    // scripts. Building this JSON-stringify helper eagerly would touch page-realm
    // intrinsics (JSON / Date.prototype / Array.prototype) before the first page
    // script, an observable difference from a never-automated page. It is only
    // needed at serialization time (always post-page); build it lazily on first use.
    this._jsonStringifyObject = null;
  }

  // Il debuggee della serializzazione, risolto al primo uso. Nei worker non
  // c'e' nessuna fabbrica e si ricade sul global del contesto, che li' e' l'unico
  // che esista.
  _serializzatore() {
    if (!this._serializationDebuggeeCache) {
      const global = this._fabbricaSerializzazione
          ? this._fabbricaSerializzazione()
          : null;
      if (global) {
        this._serializationGlobal = global;
        this._serializationDebuggeeCache =
            this._runtime._debugger.addDebuggee(global);
      } else {
        this._serializationDebuggeeCache = this._debuggee;
      }
    }
    return this._serializationDebuggeeCache;
  }

  _ensureJsonStringify() {
    if (!this._jsonStringifyObject)
      this._jsonStringifyObject = this._serializzatore().executeInGlobal(_STEALTH_JSON_STRINGIFY_SRC).return;
    return this._jsonStringifyObject;
  }

  id() {
    return this._id;
  }

  auxData() {
    return this._auxData;
  }

  _isIsolatedWorldContext() {
    return !!this._auxData.name;
  }

  async evaluateScript(script, exceptionDetails = {}) {

    // Stealth: skip user gesture activation so content can't detect automation
    // via navigator.userActivation.isActive/hasBeenActive after evaluate.
    // Set zoom.stealth.juggler.user_gesture_in_evaluate=true to restore
    // original behavior (required for popups/fullscreen/autoplay via evaluate).
    let allowUserGesture = false;
    try { allowUserGesture = Services.prefs.getBoolPref("zoom.stealth.juggler.user_gesture_in_evaluate", false); } catch(e) {}
    const userInputHelper = (allowUserGesture && this._domWindow) ? this._domWindow.windowUtils.setHandlingUserInput(true) : null;
    if (allowUserGesture && this._domWindow && this._domWindow.document)
      this._domWindow.document.notifyUserGestureActivation();

    let {success, obj} = this._getResult(this._debuggee.executeInGlobal(script), exceptionDetails);
    userInputHelper && userInputHelper.destruct();
    if (!success)
      return null;
    if (obj && obj.isPromise) {
      const awaitResult = await this._runtime._awaitPromise(this, obj, exceptionDetails);
      if (!awaitResult.success)
        return null;
      obj = awaitResult.obj;
    }
    return this._createRemoteObject(obj);
  }

  evaluateScriptSafely(script) {

    try {
      this._debuggee.executeInGlobal(script);
    } catch (e) {
      dump(`WARNING: ${e.message}\n${e.stack}\n`);
    }
  }

  async evaluateFunction(functionText, args, exceptionDetails = {}) {

    const funEvaluation = this._getResult(this._debuggee.executeInGlobal('(' + functionText + ')'), exceptionDetails);
    if (!funEvaluation.success)
      return null;
    if (!funEvaluation.obj.callable)
      throw new Error('functionText does not evaluate to a function!');
    args = args.map(arg => {
      if (arg.objectId) {
        if (!this._remoteObjects.has(arg.objectId))
          throw new Error('Cannot find object with id = ' + arg.objectId);
        return this._remoteObjects.get(arg.objectId);
      }
      switch (arg.unserializableValue) {
        case 'Infinity': return Infinity;
        case '-Infinity': return -Infinity;
        case '-0': return -0;
        case 'NaN': return NaN;
        default: return this._toDebugger(arg.value);
      }
    });
    // Stealth: same gate as evaluateScript (see comment above).
    let allowUserGesture2 = false;
    try { allowUserGesture2 = Services.prefs.getBoolPref("zoom.stealth.juggler.user_gesture_in_evaluate", false); } catch(e) {}
    const userInputHelper = (allowUserGesture2 && this._domWindow) ? this._domWindow.windowUtils.setHandlingUserInput(true) : null;
    if (allowUserGesture2 && this._domWindow && this._domWindow.document)
      this._domWindow.document.notifyUserGestureActivation();
    let {success, obj} = this._getResult(funEvaluation.obj.apply(null, args), exceptionDetails);
    userInputHelper && userInputHelper.destruct();
    if (!success)
      return null;
    if (obj && obj.isPromise) {
      const awaitResult = await this._runtime._awaitPromise(this, obj, exceptionDetails);
      if (!awaitResult.success)
        return null;
      obj = awaitResult.obj;
    }
    return this._createRemoteObject(obj);
  }

  addBinding(name, script) {
    Cu.exportFunction((...args) => {
      emitEvent(this._runtime.events.onBindingCalled, {
        executionContextId: this._id,
        name,
        payload: args[0],
      });
    }, this._contextGlobal, {
      defineAs: name,
    });
    // STEALTHFOX: non enumerabile.
    //
    // exportFunction crea la proprieta' ENUMERABILE, quindi finiva dentro
    // for..in e Object.keys(window): la scansione piu' economica che un sito
    // possa fare. Qui passa il TRASPORTO interno di Playwright, non il nome
    // che l'utente ha scelto (quello lo crea il controller iniettato dal lato
    // JavaScript), quindi nasconderlo non cambia nessuna API.
    //
    // Resta visibile a getOwnPropertyNames, e non puo' essere altrimenti: la
    // pagina deve poterla chiamare. Misurato il 2026-08-24: un lancio normale
    // non arriva mai qui, ci arriva solo chi chiama expose_function.
    // ⛔ SENZA L'XRAY. La prima stesura leggeva il descrittore dal global
    // avvolto, e l'Xray non lascia toccare la proprieta' sottostante: misurato,
    // il controller diventava non enumerabile (quello lo definisce il JS
    // iniettato, dal lato contenuto) e il canale restava dentro Object.keys.
    // Il waiver serve a operare sull'oggetto vero, e vale solo qui: la
    // proprieta' l'abbiamo appena creata noi, non e' del sito.
    try {
      const nudo = Cu.waiveXrays(this._contextGlobal);
      const descr = Object.getOwnPropertyDescriptor(nudo, name);
      if (descr && descr.enumerable) {
        descr.enumerable = false;
        Object.defineProperty(nudo, name, descr);
      }
    } catch (e) {}
    this.evaluateScriptSafely(script);
  }

  unsafeObject(objectId) {
    if (!this._remoteObjects.has(objectId))
      return;
    return { object: this._remoteObjects.get(objectId).unsafeDereference() };
  }

  rawValueToRemoteObject(rawValue) {

    const debuggerObj = this._debuggee.makeDebuggeeValue(rawValue);
    return this._createRemoteObject(debuggerObj);
  }

  _instanceOf(debuggerObj, rawObj, className) {
    if (this._domWindow)
      return rawObj instanceof this._domWindow[className];
    return this._debuggee.executeInGlobalWithBindings('o instanceof this[className]', {o: debuggerObj, className: this._debuggee.makeDebuggeeValue(className)}, {useInnerBindings: true}).return;
  }

  _createRemoteObject(debuggerObj) {
    if (debuggerObj instanceof Debugger.Object) {
      const objectId = generateId();
      this._remoteObjects.set(objectId, debuggerObj);
      const rawObj = debuggerObj.unsafeDereference();
      const type = typeof rawObj;
      let subtype = undefined;
      if (debuggerObj.isProxy)
        subtype = 'proxy';
      else if (Array.isArray(rawObj))
        subtype = 'array';
      else if (Object.is(rawObj, null))
        subtype = 'null';
      else if (typeof Node !== 'undefined' && Node.isInstance(rawObj))
        subtype = 'node';
      else if (this._instanceOf(debuggerObj, rawObj, 'RegExp'))
        subtype = 'regexp';
      else if (this._instanceOf(debuggerObj, rawObj, 'Date'))
        subtype = 'date';
      else if (this._instanceOf(debuggerObj, rawObj, 'Map'))
        subtype = 'map';
      else if (this._instanceOf(debuggerObj, rawObj, 'Set'))
        subtype = 'set';
      else if (this._instanceOf(debuggerObj, rawObj, 'WeakMap'))
        subtype = 'weakmap';
      else if (this._instanceOf(debuggerObj, rawObj, 'WeakSet'))
        subtype = 'weakset';
      else if (this._instanceOf(debuggerObj, rawObj, 'Error'))
        subtype = 'error';
      else if (this._instanceOf(debuggerObj, rawObj, 'Promise'))
        subtype = 'promise';
      else if ((this._instanceOf(debuggerObj, rawObj, 'Int8Array')) || (this._instanceOf(debuggerObj, rawObj, 'Uint8Array')) ||
               (this._instanceOf(debuggerObj, rawObj, 'Uint8ClampedArray')) || (this._instanceOf(debuggerObj, rawObj, 'Int16Array')) ||
               (this._instanceOf(debuggerObj, rawObj, 'Uint16Array')) || (this._instanceOf(debuggerObj, rawObj, 'Int32Array')) ||
               (this._instanceOf(debuggerObj, rawObj, 'Uint32Array')) || (this._instanceOf(debuggerObj, rawObj, 'Float32Array')) ||
               (this._instanceOf(debuggerObj, rawObj, 'Float64Array'))) {
        subtype = 'typedarray';
      }
      return {objectId, type, subtype};
    }
    if (typeof debuggerObj === 'symbol') {
      const objectId = generateId();
      this._remoteObjects.set(objectId, debuggerObj);
      return {objectId, type: 'symbol'};
    }

    let unserializableValue = undefined;
    if (Object.is(debuggerObj, NaN))
      unserializableValue = 'NaN';
    else if (Object.is(debuggerObj, -0))
      unserializableValue = '-0';
    else if (Object.is(debuggerObj, Infinity))
      unserializableValue = 'Infinity';
    else if (Object.is(debuggerObj, -Infinity))
      unserializableValue = '-Infinity';
    return unserializableValue ? {unserializableValue} : {value: debuggerObj};
  }

  ensureSerializedToValue(protocolObject) {
    if (!protocolObject.objectId)
      return protocolObject;
    const obj = this._remoteObjects.get(protocolObject.objectId);
    this._remoteObjects.delete(protocolObject.objectId);
    return {value: this._serialize(obj)};
  }

  _toDebugger(obj) {
    if (typeof obj !== 'object')
      return obj;
    if (obj === null)
      return obj;
    const properties = {};
    for (let [key, value] of Object.entries(obj)) {
      properties[key] = {
        configurable: true,
        writable: true,
        enumerable: true,
        value: this._toDebugger(value),
      };
    }
    const baseObject = Array.isArray(obj) ? '([])' : '({})';
    const debuggerObj = this._debuggee.executeInGlobal(baseObject).return;
    debuggerObj.defineProperties(properties);
    return debuggerObj;
  }

  _serialize(obj) {
    const result = this._serializzatore().executeInGlobalWithBindings('stringify(e)', {e: obj, stringify: this._ensureJsonStringify()}, {useInnerBindings: true});
    if (result.throw)
      throw new Error('Object is not serializable');
    return result.return === undefined ? undefined : JSON.parse(result.return);
  }

  disposeObject(objectId) {
    this._remoteObjects.delete(objectId);
  }

  getObjectProperties(objectId) {
    if (!this._remoteObjects.has(objectId))
      throw new Error('Cannot find object with id = ' + arg.objectId);
    const result = [];
    for (let obj = this._remoteObjects.get(objectId); obj; obj = obj.proto) {
      for (const propertyName of obj.getOwnPropertyNames()) {
        const descriptor = obj.getOwnPropertyDescriptor(propertyName);
        if (!descriptor.enumerable)
          continue;
        result.push({
          name: propertyName,
          value: this._createRemoteObject(descriptor.value),
        });
      }
    }
    return result;
  }

  _getResult(completionValue, exceptionDetails = {}) {
    if (!completionValue)
      throw new Error('evaluation terminated');
    if (completionValue.throw) {
      if (this._debuggee.executeInGlobalWithBindings('e instanceof Error', {e: completionValue.throw}, {useInnerBindings: true}).return) {
        exceptionDetails.text = this._debuggee.executeInGlobalWithBindings('e.message', {e: completionValue.throw}, {useInnerBindings: true}).return;
        exceptionDetails.stack = this._debuggee.executeInGlobalWithBindings('e.stack', {e: completionValue.throw}, {useInnerBindings: true}).return;
      } else {
        exceptionDetails.value = this._serialize(completionValue.throw);
      }
      return {success: false, obj: null};
    }
    return {success: true, obj: completionValue.return};
  }
}

const listenersSymbol = Symbol('listeners');

function createEvent() {
  const listeners = new Set();
  const subscribeFunction = listener => {
    listeners.add(listener);
    return () => listeners.delete(listener);
  }
  subscribeFunction[listenersSymbol] = listeners;
  return subscribeFunction;
}

function emitEvent(event, ...args) {
  let listeners = event[listenersSymbol];
  if (!listeners || !listeners.size)
    return;
  listeners = new Set(listeners);
  for (const listener of listeners)
    listener.call(null, ...args);
}

// Export Runtime to global.
globalThis.Runtime = Runtime;
