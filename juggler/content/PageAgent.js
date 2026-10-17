/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

const {Helper} = ChromeUtils.importESModule('resource://gre/modules/juggler/Helper.js');
const {NetUtil} = ChromeUtils.importESModule('resource://gre/modules/NetUtil.sys.mjs');
const {setTimeout} = ChromeUtils.importESModule('resource://gre/modules/Timer.sys.mjs');

const dragService = Cc["@mozilla.org/widget/dragservice;1"].getService(
  Ci.nsIDragService
);
const obs = Cc["@mozilla.org/observer-service;1"].getService(
  Ci.nsIObserverService
);

const helper = new Helper();

// The <input> types besides the text fields that hold a value a user sets by
// typing or picking, and so take `setUserInput`.
const kUserValueTypes = new Set(['number', 'date', 'time', 'datetime-local', 'month', 'week', 'color', 'range']);

class WorkerData {
  constructor(pageAgent, browserChannel, worker) {
    this._workerRuntime = worker.channel().connect('runtime');
    this._browserWorker = browserChannel.connect(worker.id());
    this._worker = worker;
    const emit = name => {
      return (...args) => this._browserWorker.emit(name, ...args);
    };
    this._eventListeners = [
      worker.channel().register('runtime', {
        runtimeConsole: emit('runtimeConsole'),
        runtimeExecutionContextCreated: emit('runtimeExecutionContextCreated'),
        runtimeExecutionContextDestroyed: emit('runtimeExecutionContextDestroyed'),
      }),
      browserChannel.register(worker.id(), {
        evaluate: (options) => this._workerRuntime.send('evaluate', options),
        callFunction: (options) => this._workerRuntime.send('callFunction', options),
        getObjectProperties: (options) => this._workerRuntime.send('getObjectProperties', options),
        disposeObject: (options) => this._workerRuntime.send('disposeObject', options),
      }),
    ];
  }

  dispose() {
    this._workerRuntime.dispose();
    this._browserWorker.dispose();
    helper.removeListeners(this._eventListeners);
  }
}

export class PageAgent {
  constructor(browserChannel, frameTree) {
    this._browserChannel = browserChannel;
    this._browserPage = browserChannel.connect('page');
    this._frameTree = frameTree;
    this._runtime = frameTree.runtime();

    this._workerData = new Map();
    // Elements pointer events are meant for, resolved BEFORE the events are
    // sent and read when the page has handled them (`_armLanding`). [B230]
    this._armedLandings = new Map();
    this._lastLandingToken = 0;

    const docShell = frameTree.mainFrame().docShell();
    this._docShell = docShell;

    // Dispatch frameAttached events for all initial frames
    for (const frame of this._frameTree.frames())
      this._reportFrame(frame);

    // Report created workers.
    for (const worker of this._frameTree.workers())
      this._onWorkerCreated(worker);

    this._reportExecutionContexts();

    if (this._frameTree.isPageReady()) {
      this._browserPage.emit('pageReady', {});
      const mainFrame = this._frameTree.mainFrame();
      const domWindow = mainFrame.domWindow();
      const document = domWindow ? domWindow.document : null;
      const readyState = document ? document.readyState : null;
      // Sometimes we initialize later than the first about:blank page is opened.
      // In this case, the page might've been loaded already, and we need to issue
      // the `DOMContentLoaded` and `load` events.
      if (mainFrame.url() === 'about:blank' && readyState === 'complete')
        this._emitAllEvents(this._frameTree.mainFrame());
    }

    this._eventListeners = [
      helper.addObserver(this._linkClicked.bind(this, false), 'juggler-link-click'),
      helper.addObserver(this._linkClicked.bind(this, true), 'juggler-link-click-sync'),
      // 'file-input-picker-opening' is the observer Gecko ALREADY notifies by
      // itself, with the element as the subject (upstream uses it for WebDriver
      // BiDi). This used to be 'juggler-file-picker-shown', a name that
      // appeared on this ONE line in the whole tree: nothing ever notified it,
      // so `page.on('filechooser')` could not fire. Listening to upstream's
      // instead of inventing our own removes a divergence at every rebase; the
      // only thing our C++ adds is NOT opening the native dialog while
      // interception is on.
      helper.addObserver(this._filePickerShown.bind(this), 'file-input-picker-opening'),
      helper.addObserver(this._onDocumentOpenLoad.bind(this), 'juggler-document-open-loaded'),
      helper.on(this._frameTree, 'frameattached', this._onFrameAttached.bind(this)),
      helper.on(this._frameTree, 'framedetached', this._onFrameDetached.bind(this)),
      helper.on(this._frameTree, 'navigationstarted', this._onNavigationStarted.bind(this)),
      helper.on(this._frameTree, 'navigationcommitted', this._onNavigationCommitted.bind(this)),
      // A document restored from the back-forward cache is loaded
      // already: its two lifecycle events fired the first time round and
      // will not fire again, so they are reported here or a caller waits
      // for something that can never arrive.
      helper.on(this._frameTree, 'documentrestored', this._onDocumentRestored.bind(this)),
      helper.on(this._frameTree, 'navigationaborted', this._onNavigationAborted.bind(this)),
      helper.on(this._frameTree, 'samedocumentnavigation', this._onSameDocumentNavigation.bind(this)),
      helper.on(this._frameTree, 'pageready', () => this._browserPage.emit('pageReady', {})),
      helper.on(this._frameTree, 'workercreated', this._onWorkerCreated.bind(this)),
      helper.on(this._frameTree, 'workerdestroyed', this._onWorkerDestroyed.bind(this)),
      helper.on(this._frameTree, 'websocketcreated', event => this._browserPage.emit('webSocketCreated', event)),
      helper.on(this._frameTree, 'websocketopened', event => this._browserPage.emit('webSocketOpened', event)),
      helper.on(this._frameTree, 'websocketframesent', event => this._browserPage.emit('webSocketFrameSent', event)),
      helper.on(this._frameTree, 'websocketframereceived', event => this._browserPage.emit('webSocketFrameReceived', event)),
      helper.on(this._frameTree, 'websocketclosed', event => this._browserPage.emit('webSocketClosed', event)),
      helper.on(this._frameTree, 'inputevent', inputEvent => {
        this._browserPage.emit('pageInputEvent', inputEvent);
        if (inputEvent.type === 'dragstart') {
          // After the dragStart event is dispatched and handled by Web,
          // it might or might not create a new drag session, depending on its preventing default.
          //
          // [B212]: this `setTimeout(0)` LOOKS like the race that loses the
          // drop, and it was tested as such - the engine was made to report
          // `dragStarted` from EventStateManager at the instant it decides,
          // and this poll was replaced by an observer on that fact. The
          // delivery rate did NOT improve (14/20 against 17/20, same bench,
          // same twenty seeds), so the session is genuinely not being created
          // in those runs rather than being created and read too early. The
          // change was reverted: it moved no measure and would have cost a
          // permanent divergence in EventStateManager.cpp.
          setTimeout(() => {
            const session = this._getCurrentDragSession();
            this._browserPage.emit('pageInputEvent', { type: 'juggler-drag-finalized', dragSessionStarted: !!session });
          }, 0);
        }
      }),
      helper.addObserver(this._onWindowOpen.bind(this), 'webNavigation-createdNavigationTarget-from-js'),
      this._runtime.events.onErrorFromWorker((domWindow, message, stack, location) => {
        const frame = this._frameTree.frameForDocShell(domWindow.docShell);
        if (!frame)
          return;
        this._browserPage.emit('pageUncaughtError', {
          frameId: frame.id(),
          message,
          stack,
          location,
        });
      }),
      this._runtime.events.onConsoleMessage(msg => this._browserPage.emit('runtimeConsole', msg)),
      this._runtime.events.onRuntimeError(this._onRuntimeError.bind(this)),
      this._runtime.events.onExecutionContextCreated(this._onExecutionContextCreated.bind(this)),
      this._runtime.events.onExecutionContextDestroyed(this._onExecutionContextDestroyed.bind(this)),
      this._runtime.events.onBindingCalled(this._onBindingCalled.bind(this)),
      browserChannel.register('page', {
        adoptNode: this._adoptNode.bind(this),
        describeNode: this._describeNode.bind(this),
        elementAtom: this._elementAtom.bind(this),
        dispatchKeyEvent: this._dispatchKeyEvent.bind(this),
        dispatchDragEvent: this._dispatchDragEvent.bind(this),
        awaitInputHandled: this._awaitInputHandled.bind(this),
        armLanding: this._armLanding.bind(this),
        dispatchTapEvent: this._dispatchTapEvent.bind(this),
        getContentQuads: this._getContentQuads.bind(this),
        insertText: this._insertText.bind(this),
        scrollIntoViewIfNeeded: this._scrollIntoViewIfNeeded.bind(this),
        setFileInputFiles: this._setFileInputFiles.bind(this),
        setUserInput: this._setUserInput.bind(this),
        selectOptions: this._selectOptions.bind(this),
        effectiveZoom: this._effectiveZoom.bind(this),
        evaluate: this._runtime.evaluate.bind(this._runtime),
        callFunction: this._runtime.callFunction.bind(this._runtime),
        getObjectProperties: this._runtime.getObjectProperties.bind(this._runtime),
        disposeObject: this._runtime.disposeObject.bind(this._runtime),
      }),
    ];
  }

  // A frame as it is: attached, its committed document, its navigation in
  // flight. What the client is told of a page it attaches to.
  _reportFrame(frame) {
    this._onFrameAttached(frame);
    if (frame.url())
      this._onNavigationCommitted(frame);
    if (frame.pendingNavigationId())
      this._onNavigationStarted(frame);
  }

  // Every execution context there is, after the client drops the ones it held.
  _reportExecutionContexts() {
    this._browserPage.emit('runtimeExecutionContextsCleared', {});
    for (const context of this._runtime.executionContexts())
      this._onExecutionContextCreated(context);
  }

  /**
   * The main frame's document came back from the back-forward cache, and its
   * navigation is committed (FrameTree.announceRestoredDocument). The client
   * dropped this page's iframes and contexts when the document that replaced
   * it committed, and they are all still here, so the page is reported as it
   * is - the iframes, the contexts it really has - and then as loaded: its
   * `DOMContentLoaded` and `load` fired the first time round and do not fire
   * again, so a caller waiting for them would wait for good.
   */
  _onDocumentRestored(mainFrame) {
    for (const frame of this._frameTree.frames()) {
      if (frame !== mainFrame)
        this._reportFrame(frame);
    }
    this._reportExecutionContexts();
    this._emitAllEvents(mainFrame);
  }

  _emitAllEvents(frame) {
    this._browserPage.emit('pageEventFired', {
      frameId: frame.id(),
      name: 'DOMContentLoaded',
    });
    this._browserPage.emit('pageEventFired', {
      frameId: frame.id(),
      name: 'load',
    });
  }

  _onExecutionContextCreated(executionContext) {
    this._browserPage.emit('runtimeExecutionContextCreated', {
      executionContextId: executionContext.id(),
      auxData: executionContext.auxData(),
    });
  }

  _onExecutionContextDestroyed(executionContext) {
    this._browserPage.emit('runtimeExecutionContextDestroyed', {
      executionContextId: executionContext.id(),
    });
  }

  _onWorkerCreated(worker) {
    const workerData = new WorkerData(this, this._browserChannel, worker);
    this._workerData.set(worker.id(), workerData);
    this._browserPage.emit('pageWorkerCreated', {
      workerId: worker.id(),
      frameId: worker.frame().id(),
      url: worker.url(),
    });
  }

  _onWorkerDestroyed(worker) {
    const workerData = this._workerData.get(worker.id());
    if (!workerData)
      return;
    this._workerData.delete(worker.id());
    workerData.dispose();
    this._browserPage.emit('pageWorkerDestroyed', {
      workerId: worker.id(),
    });
  }

  _onWindowOpen(subject) {
    if (!(subject instanceof Ci.nsIPropertyBag2))
      return;
    const props = subject.QueryInterface(Ci.nsIPropertyBag2);
    const hasUrl = props.hasKey('url');
    const createdDocShell = props.getPropertyAsInterface('createdTabDocShell', Ci.nsIDocShell);
    if (!hasUrl && createdDocShell === this._docShell && this._frameTree.forcePageReady())
      this._emitAllEvents(this._frameTree.mainFrame());
  }

  _linkClicked(sync, anchorElement) {
    if (anchorElement.ownerGlobal.docShell !== this._docShell)
      return;
    this._browserPage.emit('pageLinkClicked', { phase: sync ? 'after' : 'before' });
  }

  _filePickerShown(inputElement) {
    const frame = this._findFrameForNode(inputElement);
    if (!frame)
      return;
    this._browserPage.emit('pageFileChooserOpened', {
      executionContextId: frame.mainExecutionContext().id(),
      element: frame.mainExecutionContext().rawValueToRemoteObject(inputElement)
    });
  }

  _findFrameForNode(node) {
    return this._frameTree.frames().find(frame => {
      const doc = frame.domWindow().document;
      return node === doc || node.ownerDocument === doc;
    });
  }

  onWindowEvent(event) {
    if (event.type !== 'DOMContentLoaded' && event.type !== 'load')
      return;
    if (!event.target.ownerGlobal)
      return;
    const docShell = event.target.ownerGlobal.docShell;
    const frame = this._frameTree.frameForDocShell(docShell);
    if (!frame)
      return;
    this._browserPage.emit('pageEventFired', {
      frameId: frame.id(),
      name: event.type,
    });
  }

  _onRuntimeError({ executionContext, message, stack, location }) {
    this._browserPage.emit('pageUncaughtError', {
      frameId: executionContext.auxData().frameId,
      message: message.toString(),
      stack: stack.toString(),
      location,
    });
  }

  _onDocumentOpenLoad(document) {
    const docShell = document.ownerGlobal.docShell;
    const frame = this._frameTree.frameForDocShell(docShell);
    if (!frame)
      return;
    this._browserPage.emit('pageEventFired', {
      frameId: frame.id(),
      name: 'load'
    });
  }

  _onNavigationStarted(frame) {
    this._browserPage.emit('pageNavigationStarted', {
      frameId: frame.id(),
      navigationId: frame.pendingNavigationId(),
    });
  }

  _onNavigationAborted(frame, navigationId, errorText) {
    this._browserPage.emit('pageNavigationAborted', {
      frameId: frame.id(),
      navigationId,
      errorText,
    });
    if (!frame._initialNavigationDone && frame !== this._frameTree.mainFrame())
      this._emitAllEvents(frame);
    frame._initialNavigationDone = true;
  }

  _onSameDocumentNavigation(frame) {
    this._browserPage.emit('pageSameDocumentNavigation', {
      frameId: frame.id(),
      url: frame.url(),
    });
  }

  _onNavigationCommitted(frame) {
    this._browserPage.emit('pageNavigationCommitted', {
      frameId: frame.id(),
      navigationId: frame.lastCommittedNavigationId() || undefined,
      url: frame.url(),
      name: frame.name(),
    });
    frame._initialNavigationDone = true;
  }

  _onFrameAttached(frame) {
    this._browserPage.emit('pageFrameAttached', {
      frameId: frame.id(),
      parentFrameId: frame.parentFrame() ? frame.parentFrame().id() : undefined,
    });
  }

  _onFrameDetached(frame) {
    this._browserPage.emit('pageFrameDetached', {
      frameId: frame.id(),
    });
  }

  _onBindingCalled({executionContextId, name, payload}) {
    this._browserPage.emit('pageBindingCalled', {
      executionContextId,
      name,
      payload
    });
  }

  dispose() {
    for (const workerData of this._workerData.values())
      workerData.dispose();
    this._workerData.clear();
    helper.removeListeners(this._eventListeners);
  }

  async _adoptNode({frameId, objectId, executionContextId}) {
    const frame = this._frameTree.frame(frameId);
    if (!frame)
      throw new Error('Failed to find frame with id = ' + frameId);
    let unsafeObject;
    if (!objectId) {
      unsafeObject = frame.domWindow().frameElement;
    } else {
      unsafeObject = frame.unsafeObject(objectId);
    }
    const context = this._runtime.findExecutionContext(executionContextId);
    const fromPrincipal = unsafeObject.nodePrincipal;
    const toFrame = this._frameTree.frame(context.auxData().frameId);
    const toPrincipal = toFrame.domWindow().document.nodePrincipal;
    if (!toPrincipal.subsumes(fromPrincipal))
      return { remoteObject: null };
    return { remoteObject: context.rawValueToRemoteObject(unsafeObject) };
  }

  /**
   * The element behind `objectId`, as the Xray this chrome code must hold.
   *
   * Everything below calls [ChromeOnly] members (`setUserInput`,
   * `userFinishedInteracting`, `mozSetDndFilesAndDirectories`), which exist
   * only on the Xray: through a waived wrapper the call would run as the
   * page and find nothing. It is also what makes an event this file has to
   * build itself trusted - EventDispatcher marks it so because the caller is
   * chrome - so it is checked here, BEFORE anything reaches the page, rather
   * than read back from `isTrusted` after the page has already seen it.
   */
  /**
   * Marionette's own answer for an element: the Selenium atoms Firefox ships
   * with the remote agent (`atom.sys.mjs`), getVisibleText for an element's
   * text and isElementDisplayed for whether it is shown. Marionette runs
   * them in a sandbox with the Xrays waived, so a page that replaced
   * `getComputedStyle` or `getBoundingClientRect` sees every call; here the
   * same sources run through Xrays. Measured on 24 elements with both atoms
   * against Marionette's run in the same engine: 48 answers of 48 equal, and
   * the page's own functions called 0 times (1001 for Marionette's run).
   */
  _elementAtom({frameId, objectId, atom}) {
    const element = this._xrayElement(frameId, objectId);
    const win = element.ownerGlobal;
    const {ATOM_SOURCES} = ChromeUtils.importESModule(
        'chrome://remote/content/marionette/atom.sys.mjs');
    const sandbox = Cu.Sandbox(win, {sandboxPrototype: win, sameZoneAs: win,
                                     wantXrays: true, wantComponents: false});
    const run = Cu.evalInSandbox('(' + ATOM_SOURCES[atom] + ')', sandbox);
    const args = Cu.cloneInto([element], sandbox, {wrapReflectors: true});
    return {result: run(args[0])};
  }

  _xrayElement(frameId, objectId) {
    const frame = this._frameTree.frame(frameId);
    if (!frame)
      throw new Error('Failed to find frame with id = ' + frameId);
    const unsafeObject = frame.unsafeObject(objectId);
    if (!unsafeObject)
      throw new Error('Object not found for id = ' + objectId);
    const element = Cu.isXrayWrapper(unsafeObject) ? unsafeObject : Cu.unwaiveXrays(unsafeObject);
    if (!Cu.isXrayWrapper(element))
      throw new Error('Object ' + objectId + ' is not reachable through an Xray');
    return element;
  }

  /**
   * The one place this engine BUILDS an input event, for the one control
   * Firefox gives chrome no user path into (see `_setUserInput`, range).
   *
   * The init dict is what Firefox's own dispatch produces, measured with real
   * input on 151: `input` is nsContentUtils::DispatchInputEvent on a control
   * without an editor (bubbles, composed, not cancelable) and `change` is
   * DispatchTrustedEvent(CanBubble::eYes, Cancelable::eNo), so not composed: a
   * document listener must not see a `change` from inside a shadow root.
   */
  _fireAsFirefoxDoes(element, type) {
    const init = { input: { bubbles: true, composed: true }, change: { bubbles: true } }[type];
    if (!init)
      throw new Error('No native shape known for "' + type + '"');
    element.dispatchEvent(new element.ownerGlobal.Event(type, init));
  }

  /**
   * Commit a value into an <input>/<textarea> through the path Firefox itself
   * takes when a USER produces that value, so that Firefox, not this file,
   * decides which events the page gets and what they look like.
   *
   * `setUserInput` is that path: the datetime box calls it when the user
   * finishes typing a date, the password manager and form autofill call it to
   * fill a field, the in-content color picker calls it on every pick. It fires
   * `beforeinput`/`input` (an InputEvent `insertReplacementText` on a text
   * control, a plain Event elsewhere) and `change` by Firefox's own rule.
   *
   * Where that path stops short of what a user produces, the gap is closed
   * per control, and each gap is a measured one:
   *  - date/time/datetime-local: `setUserInput` assumes the datetime box is
   *    the caller and does not tell it the value changed, so its fields would
   *    keep showing the old value (and a later commit from them would undo
   *    ours). It is told with the engine's own notification for any value set
   *    from outside it, `MozDateTimeValueChanged`, non-bubbling, inside the UA
   *    widget: the page cannot hear it.
   *  - color: the release build's picker is the native dialog, whose `Done`
   *    fires `change` from C++; `setUserInput` never does for a color (it is
   *    not a change-on-blur control). It is fired here, as Firefox's own
   *    in-content picker does (ColorPickerChild.closeImpl).
   *  - range: Firefox moves a range for a user only from a pointer drag or a
   *    key, both in C++, and `setUserInput` on a FOCUSED range defers `change`
   *    to blur - where a user's drag fires it at the release. Firing it here
   *    as well would make that blur fire a second one. So the value is set as
   *    a script would and both events are fired with Firefox's shape: the
   *    least distant path, and the only one built here.
   * A value equal to the current one commits nothing, as for a user: the
   * datetime box and the color dialog both stop there.
   */
  async _setUserInput({frameId, objectId, value}) {
    const element = this._xrayElement(frameId, objectId);
    const win = element.ownerGlobal;
    const isInput = win.HTMLInputElement.isInstance(element);
    if (!isInput && !win.HTMLTextAreaElement.isInstance(element))
      throw new Error('Element is not an <input> or <textarea>');
    const type = element.type;
    if (isInput && !element.mozIsTextField(false) && !kUserValueTypes.has(type))
      throw new Error('Input of type "' + type + '" has no user value to set');
    if (isInput) {
      // What the control would hold: a value it sanitizes away is refused
      // before the page sees anything. Firefox's own sanitizer decides, run on
      // a detached <input> carrying the same attributes (`min`/`max`/`step`
      // move a range, `alpha`/`colorspace` decide what a color accepts). Not a
      // clone: cloning runs a customized built-in's constructor, which the
      // page sees. Not the handlers either: nothing on a probe may compile.
      const probe = element.ownerDocument.createElement('input');
      for (const name of element.getAttributeNames()) {
        if (!name.startsWith('on'))
          probe.setAttribute(name, element.getAttribute(name));
      }
      probe.value = value;
      if (probe.value !== value)
        throw new Error('Malformed value');
    }
    if (element.value === value)
      return { changed: false };
    if (type === 'range') {
      element.value = value;
      this._fireAsFirefoxDoes(element, 'input');
      this._fireAsFirefoxDoes(element, 'change');
      return { changed: true };
    }
    element.setUserInput(value);
    if (type === 'color') {
      this._fireAsFirefoxDoes(element, 'change');
    } else if (isInput && element.dateTimeBoxElement) {
      element.dateTimeBoxElement.dispatchEvent(new win.Event('MozDateTimeValueChanged'));
    }
    return { changed: true };
  }

  /**
   * Select options the way the dropdown does when a user picks one.
   *
   * SelectChild sets the selection and then calls `userFinishedInteracting`,
   * under `setHandlingUserInput`: Firefox fires `input` and `change` from there
   * (nsContentUtils::DispatchInputEvent, DispatchTrustedEvent), and marks the
   * control user-interacted for :user-valid/:user-invalid. The listbox, the
   * keyboard and the dropdown all end in that same call. Nothing is fired
   * when the selection did not change, as for a user picking the option that
   * was already selected.
   *
   * `indices` are positions in `select.options`, resolved by the caller: which
   * option a label or a value names is the caller's matching, not ours.
   */
  async _selectOptions({frameId, objectId, indices}) {
    const select = this._xrayElement(frameId, objectId);
    const win = select.ownerGlobal;
    if (!win.HTMLSelectElement.isInstance(select))
      throw new Error('Element is not a <select> element');
    const options = [...select.options];
    for (const index of indices) {
      if (!Number.isInteger(index) || index < 0 || index >= options.length)
        throw new Error('No option at index ' + index);
    }
    if (!select.multiple && indices.length > 1)
      throw new Error('A single <select> takes one option, got ' + indices.length);
    const before = options.map(option => option.selected);
    if (select.multiple)
      options.forEach((option, index) => option.selected = indices.includes(index));
    else
      select.selectedIndex = indices.length ? indices[0] : -1;
    const changed = options.some((option, index) => option.selected !== before[index]);
    const handlingUserInput = win.windowUtils.setHandlingUserInput(changed);
    try {
      select.userFinishedInteracting(changed);
    } finally {
      handlingUserInput.destruct();
    }
    return { changed };
  }

  /**
   * Put files into an <input type=file> the way Firefox does when a user
   * hands them over.
   *
   * `mozSetDndFilesAndDirectories` is the drop path, and it ends in the same
   * DispatchChangeEventCallback the file picker's `Done` uses: Firefox fires
   * `input` and `change` itself and marks the control user-interacted. The
   * picker also grants user activation when it closes, so a `change` handler
   * may open a popup; that is granted here first, as `Done` does.
   *
   * ⛔ THE FILES ARE BORN IN THE PAGE'S GLOBAL, through the Xray of the page's
   * own `File`/`Directory`. The drop path keeps the objects it is given, so a
   * File made with this actor's `File` ended up in `input.files` as a chrome
   * object: the page could count it and not read it, and a `change` handler
   * touching `file.name` threw "Permission denied" - measured, every listener
   * that read the file died. `mozSetFileArray` hid this by copying each file
   * into the input's global, which the drop path does not do.
   */
  async _setFileInputFiles({objectId, frameId, files}) {
    const input = this._xrayElement(frameId, objectId);
    const win = input.ownerGlobal;
    if (!win.HTMLInputElement.isInstance(input) || input.type !== 'file')
      throw new Error('Element is not an <input type=file>');
    let nsFiles;
    if (input.webkitdirectory) {
      nsFiles = await new win.Directory(files[0]).getFiles(true);
    } else {
      nsFiles = await Promise.all(files.map(filePath => win.File.createFromFileName(filePath)));
    }
    input.ownerDocument.notifyUserGestureActivation();
    input.mozSetDndFilesAndDirectories(nsFiles);
  }

  /**
   * The full zoom actually in effect for this page, which is not always the
   * one requested: the device context rounds it to whole app units per device
   * pixel, so a requested 1.1 is 60/55 = 1.0909. A point in the page's CSS
   * pixels lands at that factor in the chrome window, so the parent asks for
   * this number instead of reading the requested `BrowsingContext.fullZoom`.
   */
  _effectiveZoom() {
    const viewer = this._docShell.docViewer;
    if (!viewer)
      throw new Error('The page has no document viewer to read its zoom from');
    return { zoom: viewer.deviceFullZoomForTest };
  }

  _getContentQuads({objectId, frameId}) {
    const frame = this._frameTree.frame(frameId);
    if (!frame)
      throw new Error('Failed to find frame with id = ' + frameId);
    const unsafeObject = frame.unsafeObject(objectId);
    if (!unsafeObject.getBoxQuads)
      throw new Error('RemoteObject is not a node');
    const quads = unsafeObject.getBoxQuads({relativeTo: this._frameTree.mainFrame().domWindow().document, recurseWhenNoFrame: true}).map(quad => {
      return {
        p1: {x: quad.p1.x, y: quad.p1.y},
        p2: {x: quad.p2.x, y: quad.p2.y},
        p3: {x: quad.p3.x, y: quad.p3.y},
        p4: {x: quad.p4.x, y: quad.p4.y},
      };
    });
    return {quads};
  }

  _describeNode({objectId, frameId}) {
    const frame = this._frameTree.frame(frameId);
    if (!frame)
      throw new Error('Failed to find frame with id = ' + frameId);
    const unsafeObject = frame.unsafeObject(objectId);
    const browsingContextGroup = frame.docShell().browsingContext.group;
    const frames = this._frameTree.allFramesInBrowsingContextGroup(browsingContextGroup);
    let contentFrame;
    let ownerFrame;
    for (const frame of frames) {
      if (unsafeObject.contentWindow && frame.docShell() === unsafeObject.contentWindow.docShell)
        contentFrame = frame;
      const document = frame.domWindow().document;
      if (unsafeObject === document || unsafeObject.ownerDocument === document)
        ownerFrame = frame;
    }
    return {
      contentFrameId: contentFrame ? contentFrame.id() : undefined,
      ownerFrameId: ownerFrame ? ownerFrame.id() : undefined,
    };
  }

  async _scrollIntoViewIfNeeded({objectId, frameId, rect}) {
    const frame = this._frameTree.frame(frameId);
    if (!frame)
      throw new Error('Failed to find frame with id = ' + frameId);
    const unsafeObject = frame.unsafeObject(objectId);
    if (!unsafeObject.isConnected)
      throw new Error('Node is detached from document');
    if (!rect)
      rect = { x: -1, y: -1, width: -1, height: -1};
    if (unsafeObject.scrollRectIntoViewIfNeeded)
      unsafeObject.scrollRectIntoViewIfNeeded(rect.x, rect.y, rect.width, rect.height);
    else
      throw new Error('Node does not have a layout object');
  }

  _getNodeBoundingBox(unsafeObject) {
    if (!unsafeObject.getBoxQuads)
      throw new Error('RemoteObject is not a node');
    const quads = unsafeObject.getBoxQuads({relativeTo: this._frameTree.mainFrame().domWindow().document});
    if (!quads.length)
      return;
    let x1 = Infinity;
    let y1 = Infinity;
    let x2 = -Infinity;
    let y2 = -Infinity;
    for (const quad of quads) {
      const boundingBox = quad.getBounds();
      x1 = Math.min(boundingBox.x, x1);
      y1 = Math.min(boundingBox.y, y1);
      x2 = Math.max(boundingBox.x + boundingBox.width, x2);
      y2 = Math.max(boundingBox.y + boundingBox.height, y2);
    }
    return {x: x1, y: y1, width: x2 - x1, height: y2 - y1};
  }

  async _dispatchKeyEvent({type, keyCode, code, key, repeat, location, text, latency}) {
    const frame = this._frameTree.mainFrame();
    const tip = frame.textInputProcessor();
    let keyEvent = new (frame.domWindow().KeyboardEvent)("", {
      key,
      code,
      location,
      repeat,
      keyCode
    });
    // Stealthfox [B299]: the event is born above, and handed over `latency`
    // ms later, as an input event is born in the system before a page's
    // handler runs. TextInputProcessor keeps the time the event was born
    // with (KeydownInternal copies it, TextEventDispatcher assigns it), so
    // the page reads a `timeStamp` that many ms below its `performance.now()`;
    // born and handed over in one go they were equal, and a tracker that keeps
    // only events from the past dropped them. The chrome timer, not the
    // page's.
    if (latency > 0)
      await new Promise(f => setTimeout(f, latency));
    if (type === 'keydown') {
      if (text && text !== key) {
        tip.commitCompositionWith(text, keyEvent);
      } else {
        const flags = 0;
        tip.keydown(keyEvent, flags);
      }
    } else if (type === 'keyup') {
      if (text)
        throw new Error(`keyup does not support text option`);
      const flags = 0;
      tip.keyup(keyEvent, flags);
    } else {
      throw new Error(`Unknown type ${type}`);
    }
  }

  async _dispatchTouchEvent({type, touchPoints, modifiers}) {
    const frame = this._frameTree.mainFrame();
    const defaultPrevented = frame.domWindow().windowUtils.sendTouchEvent(
      type.toLowerCase(),
      touchPoints.map((point, id) => id),
      touchPoints.map(point => point.x),
      touchPoints.map(point => point.y),
      touchPoints.map(point => point.radiusX === undefined ? 1.0 : point.radiusX),
      touchPoints.map(point => point.radiusY === undefined ? 1.0 : point.radiusY),
      touchPoints.map(point => point.rotationAngle === undefined ? 0.0 : point.rotationAngle),
      touchPoints.map(point => point.force === undefined ? 1.0 : point.force),
      touchPoints.map(point => 0),
      touchPoints.map(point => 0),
      touchPoints.map(point => 0),
      modifiers);
    return {defaultPrevented};
  }

  async _dispatchTapEvent({x, y, modifiers}) {
    // Force a layout at the point in question, because touch events
    // do not seem to trigger one like mouse events.
    this._frameTree.mainFrame().domWindow().windowUtils.elementFromPoint(
      x,
      y,
      false /* aIgnoreRootScrollFrame */,
      true /* aFlushLayout */);

    await this._dispatchTouchEvent({
      type: 'touchstart',
      modifiers,
      touchPoints: [{x, y}]
    });
    await this._dispatchTouchEvent({
      type: 'touchend',
      modifiers,
      touchPoints: [{x, y}]
    });
  }

  _getCurrentDragSession() {
    const frame = this._frameTree.mainFrame();
    const domWindow = frame?.domWindow();
    return domWindow ? dragService.getCurrentSession(domWindow) : undefined;
  }

  /**
   * Hold the element a pointer event is meant for, BEFORE the event is sent.
   *
   * ⛔ RESOLVED NOW, NOT WHEN THE ANSWER IS WANTED. The event's own handler may
   * take the element away - a button that hides on click, a submit that
   * navigates - and a lookup made afterwards then finds nothing, which is how
   * `Page.pointerLanded` failed: asked after the dispatch answered, it met a
   * document a form submission had already replaced ("Cannot find object").
   * Held here, the node is compared with where the event landed whatever the
   * page did next. [B230]
   */
  _armLanding({frameId, objectId}) {
    const frame = this._frameTree.frame(frameId);
    if (!frame)
      throw new Error('Failed to find frame with id = ' + frameId);
    const node = frame.unsafeObject(objectId);
    if (!node)
      throw new Error('Object not found for id = ' + objectId);
    const token = ++this._lastLandingToken;
    this._armedLandings.set(token, node);
    return { token };
  }

  /**
   * Resolves once the renderer has handled mouse event `eventId`; with a
   * `landingToken`, also says whether the event of `type` LANDED on the
   * element armed under it. The parent awaits this before
   * `Page.dispatchMouseEvent` answers, and bounds it there, where a dialog
   * that suspends this process can also end the wait. [B230]
   *
   * ⛔ THE VERDICT IS READ IN THE SAME TURN AS THE ACK. The record of where the
   * event landed is written by `FrameTree` while the page handles it; reading
   * it here, before anything else runs in this process, is what makes it
   * about THIS event.
   */
  async _awaitInputHandled({eventId, type, landingToken}) {
    const node = landingToken ? this._armedLandings.get(landingToken) : null;
    this._armedLandings.delete(landingToken);
    await this._frameTree.whenEventHit(eventId);
    return node ? { landing: this._landingOn(node, type) } : {};
  }

  /**
   * Did the last pointer event of `type` land on `node`?
   *
   * ⛔ THE ANSWER THAT LETS AN ACTION STOP LYING. The driver checks the hit
   * target BEFORE it acts and, deliberately, never re-reads the geometry after:
   * a read after the event cannot distinguish a miss from a hit whose target
   * moved by its own effect. What it can be told instead is where the event
   * itself landed, recorded at dispatch by `FrameTree`. A text node counts
   * through its parent; anything inside the element counts as the element,
   * which is also how the DOM `click` composes from `mousedown` and `mouseup`.
   * [B217]
   */
  _landingOn(node, type) {
    const describe = (t) => {
      if (!t)
        return 'nothing';
      if (t.nodeType === 3)
        t = t.parentNode;
      if (!t || !t.tagName)
        return t && t.nodeName ? t.nodeName : 'nothing';
      const id = t.id ? '#' + t.id : '';
      const cls = t.classList && t.classList.length ? '.' + [...t.classList].join('.') : '';
      return t.tagName.toLowerCase() + id + cls;
    };
    const landing = this._frameTree.pointerLanding(type);
    if (!landing)
      return { type, landed: false, seen: 0, on: 'no ' + type + ' has reached the page' };
    if (landing.document !== node.ownerDocument)
      return { type, landed: false, seen: landing.seen,
               on: 'a previous document; none has reached this one' };
    const target = landing.target;
    const t = target && target.nodeType === 3 ? target.parentNode : target;
    const landed = !!t && (t === node || node.contains(t));
    return { type, landed, seen: landing.seen, on: landed ? '' : describe(t) };
  }


  async _dispatchDragEvent({type, x, y, modifiers}) {
    const session = this._getCurrentDragSession();
    const dropEffect = session.dataTransfer.dropEffect;

    if ((type === 'drop' && dropEffect !== 'none') || type ===  'dragover') {
      // [B212]: this used to call jugglerSendMouseEvent, which could never
      // work. A drag event is a WidgetDragEvent, and the mouse synthesizer
      // rejects every type outside its ten mouse strings with
      // NS_ERROR_FAILURE - so `dragover` failed on every call, the drop never
      // reached the target, and the failure also stopped the mouseup behind
      // it. jugglerSendDragEvent is the drag door, built to mirror the
      // platform's own drop target rather than the synthesized-for-tests path.
      const win = this._frameTree.mainFrame().domWindow();
      win.windowUtils.jugglerSendDragEvent(type, x, y, modifiers);
      return;
    }
    if (type === 'dragend') {
      const session = this._getCurrentDragSession();
      session?.endDragSession(true);
      return;
    }
  }

  async _insertText({text}) {
    const frame = this._frameTree.mainFrame();
    frame.textInputProcessor().commitCompositionWith(text);
  }
}

