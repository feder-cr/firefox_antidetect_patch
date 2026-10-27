/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";
const Ci = Components.interfaces;
const Cr = Components.results;
const Cu = Components.utils;

const {Helper} = ChromeUtils.importESModule('resource://gre/modules/juggler/Helper.js');

const helper = new Helper();

//: One per restored document, so that two restores never share a
//: navigation id. A restore has no load identifier of its own to borrow.
let restoredDocuments = 0;

// STEALTHFOX [B322]: the kinds of world a client can declare with
// `Page.setInitScripts` (`worldKind`). A world's kind is fixed by its first
// declaration; Frame._globalForWorld says what each one is.
const WORLD_KINDS = new Set(['isolated', 'caller']);

export class FrameTree {
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

    // A SERVICE from Firefox 157 (dom/workers/WorkerDebuggerManager.cpp): a
    // second createInstance fails with NS_ERROR_XPC_CI_RETURNED_FAILURE, so
    // the frame tree of every page after the first never came up and
    // Page.setInitScripts went unanswered until the page "crashed".
    this._wdm = Cc["@mozilla.org/dom/workers/workerdebuggermanager;1"].getService(Ci.nsIWorkerDebuggerManager);
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
        const id = +(jugglerEventId ?? '0');
        // The renderer's ack, restored with [B217]: ids only grow, so "has
        // event N been handled" is "is the last acked id at least N".
        if (id > this._lastHitEventId)
          this._lastHitEventId = id;
        this.emit(FrameTree.Events.InputEvent, { type, jugglerEventId: id });
      }, 'juggler-mouse-event-hit-renderer'),
      helper.addProgressListener(webProgress, this, flags),
    ];

    this._inputEventListeners = [];
    // Where the last pointer event of each type actually landed, per
    // document. Read back by `PageAgent._landingOn`. [B217]
    this._pointerLanding = new Map();
    this._lastHitEventId = 0;
  }

  pointerLanding(type) {
    return this._pointerLanding.get(type) || null;
  }

  /**
   * Resolves once the renderer has acked the juggler mouse event `id` (or a
   * later one). This is the ordering the landing question needs: the input
   * and the question do not share a queue - a `mousemove` is coalesced and
   * dispatched at the next refresh tick - so "sent before" is not "handled
   * before". Measured: the question answered from an empty record while the
   * page had already seen the `mouseover` of the very move it asked about.
   */
  whenEventHit(id) {
    if (this._lastHitEventId >= id)
      return Promise.resolve();
    return new Promise(resolve => {
      const off = helper.on(this, FrameTree.Events.InputEvent, () => {
        if (this._lastHitEventId >= id) {
          off();
          resolve();
        }
      });
    });
  }

  /**
   * The privileged listeners on the main frame's chrome event handler: drag
   * events for the drag door, pointer landings for [B217]. Installed on
   * `DOMDocElementInserted` of the main frame, as they always were.
   *
   * ⛔ A VERSION OF THIS ALSO INSTALLED FROM THE CONSTRUCTOR, on the theory
   * that a tree built after the document's root element was inserted had no
   * listeners. Measured false: at construction the docShell has no chrome
   * event handler yet in every run traced, so that call never installed
   * anything; and the hover whose `mouseover` the page saw with no `mousemove`
   * recorded was ORDERING - the question reached this process before the
   * coalesced `mousemove` was dispatched - which the renderer ack fixes.
   */
  _installMainFrameInputListeners(docShell) {
    helper.removeListeners(this._inputEventListeners);
    const chromeEventHandler = docShell.chromeEventHandler;
    const options = {
      mozSystemGroup: true,
      capture: true,
    };
    const emitInputEvent = (event) => this.emit(FrameTree.Events.InputEvent, { type: event.type, jugglerEventId: 0 });
    // ⛔ WHERE A POINTER EVENT LANDED IS RECORDED AT DISPATCH, NOT RE-READ
    // AFTERWARDS. A geometry read after the event cannot tell "the event
    // missed" from "the event hit and the target moved because of it"; the
    // event's own target can. Capture on the chrome event handler, in the
    // system group: it runs before any page listener and no page script can
    // stop it or see it. `composedTarget` is the node inside a shadow tree,
    // where `target` would already be retargeted to the host. [B217]
    //
    // The record carries the DOCUMENT the target belongs to, and the reader
    // compares it with the document it is asked about: a landing from a
    // previous document is then rejected by identity, not by clearing a map
    // in a lifecycle callback whose timing is not ours.
    const recordLanding = (event) => {
      const target = event.composedTarget || event.target;
      const seen = (this._pointerLanding.get(event.type)?.seen || 0) + 1;
      this._pointerLanding.set(event.type, {
        target,
        document: target && target.ownerDocument ? target.ownerDocument : target,
        seen,
      });
    };
    // Drag events are dispatched from content process, so these we don't see in the
    // `juggler-mouse-event-hit-renderer` instrumentation.
    this._inputEventListeners = [
      helper.addEventListener(chromeEventHandler, 'dragstart', emitInputEvent, options),
      helper.addEventListener(chromeEventHandler, 'mousemove', recordLanding, options),
      helper.addEventListener(chromeEventHandler, 'mousedown', recordLanding, options),
      helper.addEventListener(chromeEventHandler, 'mouseup', recordLanding, options),
    ];
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

    for (let { worldName, worldKind, script } of scripts) {
      worldName = worldName || '';
      const existing = this._isolatedWorlds.has(worldName);
      const world = this._ensureWorld(worldName, worldKind);
      world._scriptsToEvaluateOnNewDocument.push(script);
      // FIXME: 'should inherit http credentials from browser context' fails without this
      if (worldName && !existing) {
        for (const frame of this.frames())
          frame._createIsolatedContext(worldName);
      }
    }
  }

  // `kind` is what the client declared, or nothing for a world named by a
  // binding or by an older client: an existing world keeps its kind, a new one
  // is isolated. Declaring an existing name with ANOTHER kind is refused rather
  // than ignored: the client would believe its code runs where it does not.
  _ensureWorld(worldName, kind) {
    worldName = worldName || '';
    if (kind !== undefined && (!worldName || !WORLD_KINDS.has(kind)))
      throw new Error(`worldKind "${kind}" needs a worldName and one of: ${[...WORLD_KINDS].join(', ')}`);
    let world = this._isolatedWorlds.get(worldName);
    if (!world) {
      world = new IsolatedWorld(worldName, worldName ? (kind || 'isolated') : null);
      this._isolatedWorlds.set(worldName, world);
    } else if (kind !== undefined && kind !== world._kind) {
      throw new Error(`world "${worldName}" is already a ${world._kind} world`);
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

  /**
   * The frame of THIS tree whose context is `browsingContext`, or null.
   *
   * ⛔ BY THE CONTEXT ITSELF, NOT BY THE FRAME ID ALONE. A frame id names the
   * frame, not one context: the main frame's is the browser's
   * (`mainframe-<browserId>`), so every top-level context the tab has had
   * resolves to it, and with the back-forward cache in the parent each
   * navigation gives the tab a new top-level context and a new tree in the
   * same process. Looked up by id, every tree took every top-level docShell of
   * the tab for its own main frame: a tree whose document was in the cache
   * built contexts and started navigations for the page that replaced it, from
   * the process-wide observers that call this, and on a restore the page's
   * console was reported in a context of the other page (measured, B303 E3);
   * and when the cache let an old context go, every tree detached its own
   * children [B292]. That second case had a guard of its own in
   * `_onBrowsingContextDetached`; it belongs here, for every caller.
   */
  frameForBrowsingContext(browsingContext) {
    if (!browsingContext)
      return null;
    const frame = this._frameIdToFrame.get(helper.browsingContextToFrameId(browsingContext));
    return frame && frame._browsingContext.id === browsingContext.id ? frame : null;
  }

  frameForDocShell(docShell) {
    return docShell ? this.frameForBrowsingContext(docShell.browsingContext) : null;
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
      helper.removeListeners(this._inputEventListeners);
    } catch (e) {
      if (e) dump(`[FrameTree] removeListeners(_inputEventListeners) failed: ${e.message}\n`);
    }
  }

  onWindowEvent(event) {
    if (event.type !== 'DOMDocElementInserted' || !event.target.documentGlobal)
      return;

    const docShell = event.target.documentGlobal.docShell;
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
      this._installMainFrameInputListeners(docShell);
    }
  }

  _frameNavigationCommitted(frame, url) {
    for (const subframe of frame._children)
      this._detachFrame(subframe);
    this._commit(frame, url);
  }

  // The commit itself, without what a NEW document does to the tree: a
  // document restored from the back-forward cache keeps its children.
  _commit(frame, url) {
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
      // Navigation is aborted.
      const navigationId = frame._pendingNavigationId;
      frame._pendingNavigationId = null;
      // Always report download navigation as failure to match other browsers.
      const errorText = helper.getNetworkErrorStatusText(status);
      // Stealthfox [B302]: the main frame's NS_BINDING_ABORTED is SPURIOUS
      // when a Fission process swap takes the navigation to another process:
      // the old channel is disconnected there, by
      // BrowsingContext::DeactivateDocuments, which then marks the context in
      // the BFCache in the same task, or, for a document that cannot enter it,
      // while its docShell is destroyed. Dropping every such abort (the FF150
      // workaround this replaces) also dropped the real ones: document.open()
      // and window.stop() stop the ongoing navigation of a document that stays
      // where it is. The navigation then stayed pending, and the
      // document.write() after a document.open() inserts a root element into
      // the OLD document, which onWindowEvent took for the navigation's
      // commit. So the pending navigation is cleared at once, and the abort is
      // reported unless, by the end of this task, the document has gone the
      // swap's way.
      if (frame === this._mainFrame && status === Cr.NS_BINDING_ABORTED) {
        if (docShell.isBeingDestroyed())
          return;
        Services.tm.dispatchToMainThread(() => {
          let swapped;
          try {
            swapped = docShell.isBeingDestroyed() || docShell.browsingContext.isInBFCache;
          } catch (e) {
            swapped = true;   // the docShell is gone: the swap's way
          }
          if (!swapped)
            this.emit(FrameTree.Events.NavigationAborted, frame, navigationId, errorText);
        });
        return;
      }
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
      return;
    }
  }

  /**
   * The main frame's document came back from the back-forward cache, and the
   * channel is attached to it again: content/main.js runs this as the
   * channel's reattachment hook, before anything the document said meanwhile
   * goes out. A reattachment IS a restore - the channel detaches only on the
   * pagehide of a document going into the cache - so nothing here guesses.
   */
  announceRestoredDocument() {
    const frame = this._mainFrame;
    if (!frame || !this._frameIdToFrame.has(frame.id()))
      return;
    const url = frame.domWindow()?.location?.href;
    if (!url)
      return;
    // A navigation started and committed, for the client that went back: the
    // navigation id is synthetic because a restore has no load identifier to
    // borrow; it only has to be new.
    //
    // ⛔ AND NOTHING THAT A NEW DOCUMENT DOES. Its globals, its iframes and its
    // execution contexts are the ones it had: clearing the global object made
    // new contexts under new ids - and ran the init scripts again in a page
    // that did not load again - while what the page said from its pageshow,
    // kept by the channel until now, named the contexts it really has, and
    // the client dropped it for an unknown context; and the commit of a new
    // document detached the restored page's own iframes from this tree. The
    // children and the contexts are reported again by PageAgent on
    // DocumentRestored, as when it attaches to a page.
    frame._pendingNavigationId =
        helper.toProtocolNavigationId('bfcache-' + (++restoredDocuments));
    this.emit(FrameTree.Events.NavigationStarted, frame);
    this._commit(frame, url);
    // ⛔ AND THE DOCUMENT IS ALREADY LOADED, WHICH NOTHING WOULD EVER SAY.
    // `load` and `DOMContentLoaded` fired when this document was first built
    // and do not fire again on a restore, so a caller waiting for either -
    // which is what `go_back()` does by default - would wait for an event that
    // can never come. Reporting the fact is this class's job; turning it into
    // protocol events is PageAgent's, which is why this is an event and not a
    // call into the agent.
    this.emit(FrameTree.Events.DocumentRestored, frame);
  }

  _onBrowsingContextAttached(browsingContext) {
    // If this browsing context doesn't belong to our frame tree - do nothing.
    if (browsingContext.top !== this._rootBrowsingContext)
      return;
    this._createFrame(browsingContext);
  }

  _onBrowsingContextDetached(browsingContext) {
    // Only the frame whose context went: frameForBrowsingContext matches the
    // context itself [B292].
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
  DocumentRestored: 'documentrestored',
  NavigationCommitted: 'navigationcommitted',
  NavigationAborted: 'navigationaborted',
  SameDocumentNavigation: 'samedocumentnavigation',
  PageReady: 'pageready',
  InputEvent: 'inputevent',
};

class IsolatedWorld {
  constructor(name, kind) {
    this._name = name;
    // 'isolated' or 'caller' for a named world; the main world ('') is the
    // page's own global and has no kind of its own.
    this._kind = kind;
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

  // STEALTHFOX: THE ONE PLACE THAT KNOWS HOW A WORLD IS BUILT. A world is its
  // global, and three things tell the globals apart: the principal, and the
  // two views of the page's objects a sandbox can ask for.
  //
  //   'main'     the page's own window. Only what the page must see runs here:
  //              the clients' init scripts and bindings.
  //   'isolated' a sandbox with the extended principal [dw] and plain Xrays:
  //              the page's objects as the standards define them, nothing the
  //              page added or replaced. What the MACHINERY needs (selectors,
  //              actionability, serialization): a site that replaces
  //              Element.prototype.matches must not steer it. It also sees
  //              closed shadow roots, which the page's principal does not.
  //   'caller'   [B322] a sandbox with the page's principal and hybrid Xrays
  //              (`hybridXrays`, js/xpconnect/wrappers/XrayWrapper.h): a name
  //              the standard defines resolves to the standard definition,
  //              every other name as the page defined it. Where the CALLER'S
  //              code runs (page.evaluate and the like): `window.__APP_STATE__`,
  //              `el.__vue__` and the page's functions are the page's, while
  //              `document.querySelector` is the native method even when the
  //              page wrapped it in a Proxy that counts who calls it. A
  //              bot-challenge page does exactly that, and one lookup of its
  //              token field from the page's realm got its clearance refused.
  //
  // ⛔ THE PRINCIPAL DECIDES AS MUCH AS THE FLAGS, measured on 2026-08-23: a
  // sandbox with `wantXrays: false` and the extended principal still sees
  // Xrays, because the page's principal does not subsume it. Hence the caller's
  // world takes the page's principal, and the page in turn reads the caller's
  // objects transparently, as it did when this code ran in the page's realm.
  //
  // ⛔ AND IT REPLACES THE "SCRIPT WORLD" (`STEALTHFOX_SCRIPT_WORLD=1`, page's
  // principal and `wantXrays: false`), which never shipped: its view of the
  // page was transparent, so `document.querySelector` went through the page's
  // Proxy exactly as in the page's realm. A JS membrane choosing Xray or waiver
  // per property was tried before that and cannot work: the invariants of
  // scripted proxies force the raw value on non-configurable properties, and
  // WebIDL refuses a scripted Proxy as `this`. The hybrid view lives in the
  // wrapper itself for that reason.
  _createGlobal(kind) {
    const dw = this.domWindow();
    if (kind === 'main')
      return dw;
    const options = {
      sandboxPrototype: dw,
      wantComponents: false,
      wantExportHelpers: false,
    };
    if (kind === 'caller')
      return Cu.Sandbox(dw, { ...options, wantXrays: false, hybridXrays: true });
    return Cu.Sandbox([dw], { ...options, wantXrays: true });
  }

  // The fourth argument is the global the return values are SERIALIZED in,
  // never the page's: the reasons, with the numbers, are in the constructor of
  // ExecutionContext in Runtime.js. One sandbox per frame, shared by every
  // world, created lazily because a world that never serializes must not pay
  // for it. It stays for the caller's world too: a returned value can be the
  // PAGE's object (`() => window.something`), and JSON.stringify would walk its
  // prototype chain and call its `toJSON`; the serialization sandbox looks
  // through plain Xrays, which do not expose that chain (P5.2x).
  _createWorldContext(name, kind) {
    const global = this._createGlobal(kind);
    const world = this._runtime.createExecutionContext(this.domWindow(), global, {
      frameId: this.id(),
      name,
    }, () => this._ensureSerializationSandbox());
    this._worldNameToContext.set(name, world);
    return world;
  }

  _createIsolatedContext(name) {
    const declared = this._frameTree._isolatedWorlds.get(name);
    return this._createWorldContext(name, declared ? declared._kind : 'isolated');
  }

  _ensureSerializationSandbox() {
    if (!this._serializationSandbox)
      this._serializationSandbox = this._createGlobal('isolated');
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

    // ⛔ THE "main" WORLD IS THE PAGE'S OWN GLOBAL, and stays so: the clients'
    // init scripts and bindings run here because the page must see them. The
    // caller's code does not: it runs in a 'caller' world the client declares
    // (_createGlobal). Measured on 2026-08-23 (90-firefox-internals.md) why the
    // main world itself could not simply become a sandbox: with the extended
    // principal the page's additions disappear, with the page's principal and
    // no Xrays its replacements stay in the way.
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

    workerDebugger.initialize('resource://gre/modules/juggler/content/WorkerMain.js');

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


