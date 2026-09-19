/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

const {Helper} = ChromeUtils.importESModule('chrome://juggler/content/Helper.js');
const {NetUtil} = ChromeUtils.importESModule('resource://gre/modules/NetUtil.sys.mjs');
const {setTimeout} = ChromeUtils.importESModule('resource://gre/modules/Timer.sys.mjs');

const dragService = Cc["@mozilla.org/widget/dragservice;1"].getService(
  Ci.nsIDragService
);
const obs = Cc["@mozilla.org/observer-service;1"].getService(
  Ci.nsIObserverService
);

const helper = new Helper();

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

    const docShell = frameTree.mainFrame().docShell();
    this._docShell = docShell;

    // Dispatch frameAttached events for all initial frames
    for (const frame of this._frameTree.frames()) {
      this._onFrameAttached(frame);
      if (frame.url())
        this._onNavigationCommitted(frame);
      if (frame.pendingNavigationId())
        this._onNavigationStarted(frame);
    }

    // Report created workers.
    for (const worker of this._frameTree.workers())
      this._onWorkerCreated(worker);

    // Report execution contexts.
    this._browserPage.emit('runtimeExecutionContextsCleared', {});
    for (const context of this._runtime.executionContexts())
      this._onExecutionContextCreated(context);

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
      helper.on(this._frameTree, 'documentrestored', frame => this._emitAllEvents(frame)),
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
        dispatchKeyEvent: this._dispatchKeyEvent.bind(this),
        dispatchDragEvent: this._dispatchDragEvent.bind(this),
        isDragSessionLive: this._isDragSessionLive.bind(this),
        pointerLanded: this._pointerLanded.bind(this),
        dispatchTapEvent: this._dispatchTapEvent.bind(this),
        getContentQuads: this._getContentQuads.bind(this),
        insertText: this._insertText.bind(this),
        scrollIntoViewIfNeeded: this._scrollIntoViewIfNeeded.bind(this),
        setFileInputFiles: this._setFileInputFiles.bind(this),
        dispatchTrustedInputEvents: this._dispatchTrustedInputEvents.bind(this),
        evaluate: this._runtime.evaluate.bind(this._runtime),
        callFunction: this._runtime.callFunction.bind(this._runtime),
        getObjectProperties: this._runtime.getObjectProperties.bind(this._runtime),
        disposeObject: this._runtime.disposeObject.bind(this._runtime),
      }),
    ];
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

  async _dispatchTrustedInputEvents({objectId, frameId, types}) {
    // The chrome-side twin of the dispatch the injected script used to do on
    // its own, content-side, for select_option and non-textual fill. The
    // difference is dispatchDOMEventViaPresShellForTesting instead of
    // element.dispatchEvent: the former calls SetTrusted(true) on the event
    // before delivering it and the latter does not - which is why the measured
    // divergence existed, rather than it being a deliberate choice.
    const frame = this._frameTree.frame(frameId);
    if (!frame)
      throw new Error('Failed to find frame with id = ' + frameId);
    const unsafeObject = frame.unsafeObject(objectId);
    if (!unsafeObject)
      throw new Error('Object not found for id = ' + objectId);
    const utils = frame.domWindow().windowUtils;
    for (const type of types) {
      const event = new (frame.domWindow().Event)(type, { bubbles: true, cancelable: true, composed: true });
      utils.dispatchDOMEventViaPresShellForTesting(unsafeObject, event);
    }
  }

  async _setFileInputFiles({objectId, frameId, files}) {
    const frame = this._frameTree.frame(frameId);
    if (!frame)
      throw new Error('Failed to find frame with id = ' + frameId);
    const unsafeObject = frame.unsafeObject(objectId);
    if (!unsafeObject)
      throw new Error('Object is not input!');
    let nsFiles;
    if (unsafeObject.webkitdirectory) {
      nsFiles = await new Directory(files[0]).getFiles(true);
    } else {
      nsFiles = await Promise.all(files.map(filePath => File.createFromFileName(filePath)));
    }
    unsafeObject.mozSetFileArray(nsFiles);
    const events = [
      new (frame.domWindow().Event)('input', { bubbles: true, cancelable: true, composed: true }),
      new (frame.domWindow().Event)('change', { bubbles: true, cancelable: true, composed: true }),
    ];
    for (const event of events)
      unsafeObject.dispatchEvent(event);
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

  async _dispatchKeyEvent({type, keyCode, code, key, repeat, location, text}) {
    const frame = this._frameTree.mainFrame();
    const tip = frame.textInputProcessor();
    let keyEvent = new (frame.domWindow().KeyboardEvent)("", {
      key,
      code,
      location,
      repeat,
      keyCode
    });
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
   * Is a drag session live RIGHT NOW? Asked by the parent at the release.
   *
   * ⛔ WHY ASKING BEATS WATCHING, AT THIS ONE POINT. The parent learns about a
   * drag by observing the `dragstart` we emit, which is cheap and needs no round
   * trip - but observing can only ever be as fresh as the last thing that
   * arrived. At the release there is no later event to catch up on, so a miss
   * there is final: the release goes out as a plain `mouseup`, the drop never
   * happens, and the session is not even closed. That is the case of a travel
   * made of ONE movement, where the drag is born by the last event of the
   * gesture. [B213]
   *
   * So at that one point the parent stops inferring and asks the side that
   * knows. The answer is ordered behind the mouse event that may have started
   * the drag: both ride the same channel to this process, and input is not
   * delivered later than what was sent after it.
   */
  async _isDragSessionLive() {
    return { live: !!this._getCurrentDragSession() };
  }

  /**
   * Did the last pointer event of each given type land on this element?
   *
   * ⛔ THE ANSWER THAT LETS AN ACTION STOP LYING. The driver checks the hit
   * target BEFORE it acts and, deliberately, never re-reads the geometry after:
   * a read after the event cannot distinguish a miss from a hit whose target
   * moved by its own effect (`_act_on_target` in the wrapper says why). What it
   * can be told instead is where the event itself landed, recorded at dispatch
   * by `FrameTree`. A text node counts through its parent; anything inside the
   * element counts as the element, which is also how the DOM `click` composes
   * from `mousedown` and `mouseup`.
   *
   * Ordered behind the events it is about, for the same reason as
   * `_isDragSessionLive`: the question rides the same channel as the input.
   * [B217]
   */
  async _pointerLanded({frameId, objectId, types, afterEventId}) {
    const frame = this._frameTree.frame(frameId);
    if (!frame)
      throw new Error('Failed to find frame with id = ' + frameId);
    const node = frame.unsafeObject(objectId);
    if (!node)
      throw new Error('Object not found for id = ' + objectId);
    // ⛔ WAIT FOR THE RENDERER'S ACK OF THE LAST EVENT SENT, OR THE ANSWER IS
    // ABOUT THE WRONG MOMENT. The question and the input do not share a
    // queue: a `mousemove` is coalesced and dispatched at the next refresh
    // tick, and this method ran first two times out of four, answering "no
    // mousemove has reached the page" while the page had already seen the
    // `mouseover` of that very move. The ack is exact; a wait on frames would
    // be a guess. Bounded, and a bound that expires is reported as what it is.
    if (afterEventId) {
      const acked = await Promise.race([
        this._frameTree.whenEventHit(afterEventId).then(() => true),
        new Promise(resolve => setTimeout(() => resolve(false), 5000)),
      ]);
      if (!acked) {
        return { landings: types.map(type => ({
          type, landed: false, seen: 0,
          on: 'event ' + afterEventId + ' was never acked by the renderer',
        })) };
      }
    }
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
    const document = frame.domWindow().document;
    const landings = [];
    for (const type of types) {
      const landing = this._frameTree.pointerLanding(type);
      if (!landing) {
        landings.push({ type, landed: false, seen: 0, on: 'no ' + type + ' has reached the page' });
        continue;
      }
      if (landing.document !== document) {
        landings.push({ type, landed: false, seen: landing.seen,
                        on: 'a previous document; none has reached this one' });
        continue;
      }
      const target = landing.target;
      const t = target && target.nodeType === 3 ? target.parentNode : target;
      const landed = !!t && (t === node || node.contains(t));
      landings.push({ type, landed, seen: landing.seen, on: landed ? '' : describe(t) });
    }
    return { landings };
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

