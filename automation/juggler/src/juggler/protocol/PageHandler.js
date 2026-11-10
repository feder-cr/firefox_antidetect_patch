/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {Helper, EventWatcher} = ChromeUtils.importESModule('resource://gre/modules/juggler/Helper.js');
const {NetUtil} = ChromeUtils.importESModule('resource://gre/modules/NetUtil.sys.mjs');
const {NetworkObserver, PageNetwork} = ChromeUtils.importESModule('resource://gre/modules/juggler/NetworkObserver.js');
const {PageTarget} = ChromeUtils.importESModule('resource://gre/modules/juggler/TargetRegistry.js');
const {setTimeout, clearTimeout} = ChromeUtils.importESModule('resource://gre/modules/Timer.sys.mjs');

const Cc = Components.classes;
const Ci = Components.interfaces;
const Cu = Components.utils;
const XUL_NS = 'http://www.mozilla.org/keymaster/gatekeeper/there.is.only.xul';
const helper = new Helper();

/* STEALTHFOX_HUMANIZE_HOOK v1
 *
 * Expands `mousemove` dispatched by the Juggler into a Bezier trajectory
 * with ~10 ms between intermediate waypoints, so that high-level Playwright
 * actions (page.click, page.hover, locator.click, drag, ...) produce
 * human-like cursor motion instead of a teleport.
 *
 * Gated on the pref `stealthfox.humanize` (bool). Fine-tuning via
 *   stealthfox.humanize.maxTime  (char, seconds, default "1.5")
 *   stealthfox.humanize.stepMs   (int,  ms,      default 10)
 *
 * Algorithm: 2 random knots offset by +/-80 px, Bernstein/Bezier curve
 * sampled at max(4, length^0.25 * 20) points, Gaussian jitter on
 * intermediate Y, ease-out-quad tween.
 *
 * Every number above is a constant, so the shape is identical for every
 * movement of every install. That is an invariant, and it is why the
 * per-session generator lives in the wrapper now: see the wrapper's
 * _motion.py. This path stays as the fallback.
 */
const _stealthfoxHumanize = {
  _factorial(n) { let r = 1; for (let i = 2; i <= n; i++) r *= i; return r; },
  _binom(n, k) { return this._factorial(n) / (this._factorial(k) * this._factorial(n - k)); },
  _bern(t, i, n) { return this._binom(n, i) * Math.pow(t, i) * Math.pow(1 - t, n - i); },
  _bez(t, pts) {
    let x = 0, y = 0; const n = pts.length - 1;
    for (let i = 0; i <= n; i++) {
      const b = this._bern(t, i, n);
      x += pts[i][0] * b;
      y += pts[i][1] * b;
    }
    return [x, y];
  },
  _curve(nPts, pts) {
    const out = [];
    if (nPts < 2) nPts = 2;
    for (let i = 0; i < nPts; i++) out.push(this._bez(i / (nPts - 1), pts));
    return out;
  },
  _easeOut(t) { return -t * (t - 2); },
  _gauss(mu, sigma) {
    const u = 1 - Math.random(), v = Math.random();
    return mu + sigma * Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
  },
  trajectory(fromX, fromY, toX, toY, maxTimeS) {
    const left = Math.min(fromX, toX) - 80;
    const right = Math.max(fromX, toX) + 80;
    const down = Math.min(fromY, toY) - 80;
    const up = Math.max(fromY, toY) + 80;
    const knots = [
      [left + Math.random() * (right - left), down + Math.random() * (up - down)],
      [left + Math.random() * (right - left), down + Math.random() * (up - down)],
    ];
    const ctrl = [[fromX, fromY], ...knots, [toX, toY]];
    const mid = Math.max(Math.abs(fromX - toX), Math.abs(fromY - toY), 2) | 0;
    const curve = this._curve(mid, ctrl);
    for (let i = 1; i < curve.length - 1; i++) {
      if (Math.random() < 0.5) curve[i][1] += Math.round(this._gauss(1, 1));
    }
    let total = 0;
    for (let i = 1; i < curve.length; i++) {
      const dx = curve[i][0] - curve[i - 1][0];
      const dy = curve[i][1] - curve[i - 1][1];
      total += Math.sqrt(dx * dx + dy * dy);
    }
    // The ceiling on the step count is DERIVED from the cadence rather than
    // repeating it: the `100` that used to sit here was "10 ms per step"
    // written a second time, the same fact as `stepMs` in two places. Raising
    // stepMs while leaving the 100 would have overrun `maxTime` with nothing
    // saying so.
    const maxSteps = Math.max(4, Math.floor((maxTimeS || 1.5) * 1000 / this.stepMs()));
    const target = Math.min(maxSteps, Math.max(4, Math.floor(Math.pow(total, 0.25) * 20)));
    const out = [];
    for (let i = 0; i < target; i++) {
      const t = i / (target - 1);
      const e = this._easeOut(t);
      out.push(curve[Math.min(curve.length - 1, Math.floor(e * (curve.length - 1)))]);
    }
    return out;
  },
  enabled() {
    try { return Services.prefs.getBoolPref('stealthfox.humanize', true); }
    catch (e) { return true; }
  },
  maxTimeS() {
    try { return parseFloat(Services.prefs.getCharPref('stealthfox.humanize.maxTime', '1.5')) || 1.5; }
    catch (e) { return 1.5; }
  },
  stepMs() {
    try { return Services.prefs.getIntPref('stealthfox.humanize.stepMs', 10); }
    catch (e) { return 10; }
  },
};

function hashConsoleMessage(params) {
  return params.location.lineNumber + ':' + params.location.columnNumber + ':' + params.location.url;
}

class WorkerHandler {
  constructor(session, contentChannel, workerId) {
    this._session = session;
    this._contentWorker = contentChannel.connect(workerId);
    this._workerConsoleMessages = new Set();
    this._workerId = workerId;

    const emitWrappedProtocolEvent = eventName => {
      return params => {
        this._session.emitEvent('Page.dispatchMessageFromWorker', {
          workerId,
          message: JSON.stringify({method: eventName, params}),
        });
      }
    }

    this._eventListeners = [
      contentChannel.register(workerId, {
        runtimeConsole: (params) => {
          this._workerConsoleMessages.add(hashConsoleMessage(params));
          emitWrappedProtocolEvent('Runtime.console')(params);
        },
        runtimeExecutionContextCreated: emitWrappedProtocolEvent('Runtime.executionContextCreated'),
        runtimeExecutionContextDestroyed: emitWrappedProtocolEvent('Runtime.executionContextDestroyed'),
      }),
    ];
  }

  async sendMessage(message) {
    const [domain, method] = message.method.split('.');
    if (domain !== 'Runtime')
      throw new Error('ERROR: can only dispatch to Runtime domain inside worker');
    const result = await this._contentWorker.send(method, message.params);
    this._session.emitEvent('Page.dispatchMessageFromWorker', {
      workerId: this._workerId,
      message: JSON.stringify({result, id: message.id}),
    });
  }

  dispose() {
    this._contentWorker.dispose();
    helper.removeListeners(this._eventListeners);
  }
}

export class PageHandler {
  constructor(target, session, contentChannel) {
    this._session = session;
    this._contentChannel = contentChannel;
    this._contentPage = contentChannel.connect('page');
    this._workers = new Map();

    this._pageTarget = target;
    this._pageNetwork = PageNetwork.forPageTarget(target);

    const emitProtocolEvent = eventName => {
      return (...args) => this._session.emitEvent(eventName, ...args);
    }

    this._isDragging = false;
    // The watcher that notices a drag being born lives as long as the GESTURE -
    // from the press to the release - not as long as one dispatch call. See
    // `_adoptDragSessionIfStarted` for why that distinction is the whole bug.
    this._dragGestureWatcher = null;
    this._lastMousePosition = { x: 0, y: 0 };
    this._documentScopedWaits = new Set();

    this._reportedFrameIds = new Set();
    this._networkEventsForUnreportedFrameIds = new Map();

    // `Page.ready` protocol event is emitted whenever page has completed initialization, e.g.
    // finished all the transient navigations to the `about:blank`.
    //
    // We'd like to avoid reporting meaningful events before the `Page.ready` since they are likely
    // to be ignored by the protocol clients.
    this._isPageReady = false;

    this._pageEventSink = {};
    helper.decorateAsEventEmitter(this._pageEventSink);

    this._pendingEventWatchers = new Set();
    this._eventListeners = [
      helper.on(this._pageTarget, PageTarget.Events.DialogOpened, this._onDialogOpened.bind(this)),
      helper.on(this._pageTarget, PageTarget.Events.ContentDetached,
                this._onContentDetached.bind(this)),
      helper.on(this._pageTarget, PageTarget.Events.Crashed, () => {
        this._session.emitEvent('Page.crashed', {});
      }),
      helper.on(this._pageTarget, PageTarget.Events.ScreencastFrame, params => {
        this._session.emitEvent('Page.screencastFrame', params);
      }),
      helper.on(this._pageNetwork, PageNetwork.Events.Request, this._handleNetworkEvent.bind(this, 'Network.requestWillBeSent')),
      helper.on(this._pageNetwork, PageNetwork.Events.Response, this._handleNetworkEvent.bind(this, 'Network.responseReceived')),
      helper.on(this._pageNetwork, PageNetwork.Events.RequestFinished, this._handleNetworkEvent.bind(this, 'Network.requestFinished')),
      helper.on(this._pageNetwork, PageNetwork.Events.RequestFailed, this._handleNetworkEvent.bind(this, 'Network.requestFailed')),
      contentChannel.register('page', {
        pageBindingCalled: emitProtocolEvent('Page.bindingCalled'),
        pageEventFired: emitProtocolEvent('Page.eventFired'),
        pageFileChooserOpened: emitProtocolEvent('Page.fileChooserOpened'),
        pageFrameAttached: this._onFrameAttached.bind(this),
        pageFrameDetached: emitProtocolEvent('Page.frameDetached'),
        pageLinkClicked: emitProtocolEvent('Page.linkClicked'),
        pageNavigationAborted: emitProtocolEvent('Page.navigationAborted'),
        pageNavigationCommitted: emitProtocolEvent('Page.navigationCommitted'),
        pageNavigationStarted: emitProtocolEvent('Page.navigationStarted'),
        pageReady: this._onPageReady.bind(this),
        pageInputEvent: (event) => this._pageEventSink.emit(event.type, event),
        pageSameDocumentNavigation: emitProtocolEvent('Page.sameDocumentNavigation'),
        pageUncaughtError: emitProtocolEvent('Page.uncaughtError'),
        pageWorkerCreated: this._onWorkerCreated.bind(this),
        pageWorkerDestroyed: this._onWorkerDestroyed.bind(this),
        runtimeConsole: params => {
          const consoleMessageHash = hashConsoleMessage(params);
          for (const worker of this._workers.values()) {
            if (worker._workerConsoleMessages.has(consoleMessageHash)) {
              worker._workerConsoleMessages.delete(consoleMessageHash);
              return;
            }
          }
          this._session.emitEvent('Runtime.console', params);
        },
        runtimeExecutionContextCreated: emitProtocolEvent('Runtime.executionContextCreated'),
        runtimeExecutionContextDestroyed: emitProtocolEvent('Runtime.executionContextDestroyed'),
        runtimeExecutionContextsCleared: emitProtocolEvent('Runtime.executionContextsCleared'),

        webSocketCreated: emitProtocolEvent('Page.webSocketCreated'),
        webSocketOpened: emitProtocolEvent('Page.webSocketOpened'),
        webSocketClosed: emitProtocolEvent('Page.webSocketClosed'),
        webSocketFrameReceived: emitProtocolEvent('Page.webSocketFrameReceived'),
        webSocketFrameSent: emitProtocolEvent('Page.webSocketFrameSent'),
      }),
    ];
  }

  async dispose() {
    this._contentPage.dispose();
    for (const watcher of this._pendingEventWatchers)
      watcher.dispose();
    helper.removeListeners(this._eventListeners);
  }

  _onPageReady(event) {
    this._isPageReady = true;
    this._session.emitEvent('Page.ready');
    for (const dialog of this._pageTarget.dialogs())
      this._onDialogOpened(dialog);
  }

  _onDialogOpened(dialog) {
    if (!this._isPageReady)
      return;
    this._session.emitEvent('Page.dialogOpened', {
      dialogId: dialog.id(),
      type: dialog.type(),
      message: dialog.message(),
      defaultValue: dialog.defaultValue(),
    });
  }

  _onWorkerCreated({workerId, frameId, url}) {
    const worker = new WorkerHandler(this._session, this._contentChannel, workerId);
    this._workers.set(workerId, worker);
    this._session.emitEvent('Page.workerCreated', {workerId, frameId, url});
  }

  _onWorkerDestroyed({workerId}) {
    const worker = this._workers.get(workerId);
    if (!worker)
      return;
    this._workers.delete(workerId);
    worker.dispose();
    this._session.emitEvent('Page.workerDestroyed', {workerId});
  }

  _handleNetworkEvent(protocolEventName, eventDetails, frameId) {
    if (!this._reportedFrameIds.has(frameId)) {
      let events = this._networkEventsForUnreportedFrameIds.get(frameId);
      if (!events) {
        events = [];
        this._networkEventsForUnreportedFrameIds.set(frameId, events);
      }
      events.push({eventName: protocolEventName, eventDetails});
    } else {
      this._session.emitEvent(protocolEventName, eventDetails);
    }
  }

  _onFrameAttached({frameId, parentFrameId}) {
    this._session.emitEvent('Page.frameAttached', {frameId, parentFrameId});
    this._reportedFrameIds.add(frameId);
    const events = this._networkEventsForUnreportedFrameIds.get(frameId) || [];
    this._networkEventsForUnreportedFrameIds.delete(frameId);
    for (const {eventName, eventDetails} of events)
      this._session.emitEvent(eventName, eventDetails);
  }

  async ['Page.close']({runBeforeUnload}) {
    // Postpone target close to deliver response in session.
    Services.tm.dispatchToMainThread(() => {
      this._pageTarget.close(runBeforeUnload);
    });
  }

  async ['Page.setViewportSize']({viewportSize}) {
    await this._pageTarget.setViewportSize(viewportSize === null ? undefined : viewportSize);
  }
  async ['Runtime.evaluate'](options) {
    return await this._contentPage.send('evaluate', options);
  }

  async ['Runtime.callFunction'](options) {
    return await this._contentPage.send('callFunction', options);
  }

  async ['Runtime.getObjectProperties'](options) {
    return await this._contentPage.send('getObjectProperties', options);
  }

  async ['Runtime.disposeObject'](options) {
    return await this._contentPage.send('disposeObject', options);
  }

  async ['Heap.collectGarbage']() {
    Services.obs.notifyObservers(null, "child-gc-request");
    Cu.forceGC();
    Services.obs.notifyObservers(null, "child-cc-request");
    Cu.forceCC();
  }

  async ['Network.setExtraHTTPHeaders']({headers}) {
    this._pageNetwork.setExtraHTTPHeaders(headers);
  }

  async ['Network.setRequestInterception']({enabled}) {
    if (enabled)
      this._pageNetwork.enableRequestInterception();
    else
      this._pageNetwork.disableRequestInterception();
  }

  async ['Network.resumeInterceptedRequest']({requestId, url, method, headers, postData}) {
    this._pageNetwork.resumeInterceptedRequest(requestId, url, method, headers, postData);
  }

  async ['Network.abortInterceptedRequest']({requestId, errorCode}) {
    this._pageNetwork.abortInterceptedRequest(requestId, errorCode);
  }

  async ['Network.fulfillInterceptedRequest']({requestId, status, statusText, headers, base64body}) {
    this._pageNetwork.fulfillInterceptedRequest(requestId, status, statusText, headers, base64body);
  }

  // Stealthfox 2026-08-30: rimesso insieme al comando nel protocollo.
  async ['Network.getResponseBody']({requestId}) {
    return this._pageNetwork.getResponseBody(requestId);
  }


  async ['Page.setFileInputFiles'](options) {
    return await this._contentPage.send('setFileInputFiles', options);
  }

  async ['Page.setUserInput'](options) {
    return await this._contentPage.send('setUserInput', options);
  }

  async ['Page.selectOptions'](options) {
    return await this._contentPage.send('selectOptions', options);
  }

  async ['Page.setEmulatedMedia']({colorScheme, type, reducedMotion, forcedColors, contrast}) {
    // Stealthfox: reduced motion, forced colors and contrast are NOT imposed
    // from here any more. Their declarations live in invisible_core as prefs,
    // and the BrowsingContext override short-circuited them: Gecko looks at the
    // override first and only reads LookAndFeel when that one is None.
    //
    // It REFUSES rather than ignoring silently. Ignoring would make this API a
    // lie: the caller believes it changed a media feature and the page answers
    // something else. A refusal names the real knob.
    const imposed = Object.entries({reducedMotion, forcedColors, contrast})
        .filter(entry => entry[1] !== undefined && entry[1] !== null && entry[1] !== '')
        .map(entry => entry[0]);
    if (imposed.length)
      throw new Error('Page.setEmulatedMedia: ' + imposti.join(', ') + ' non si impostano da qui; li dichiara invisible_core nel profilo');
    this._pageTarget.setColorScheme(colorScheme || null);
    this._pageTarget.setEmulatedMedia(type);
  }

  async ['Page.bringToFront'](options) {
    await this._pageTarget.activateAndRun(() => {});
  }

  async ['Page.setCacheDisabled']({cacheDisabled}) {
    return await this._pageTarget.setCacheDisabled(cacheDisabled);
  }
  async ['Page.adoptNode'](options) {
    return await this._contentPage.send('adoptNode', options);
  }

  async ['Page.screenshot']({ mimeType, clip, omitDeviceScaleFactor, quality = 80}) {
    const rect = new DOMRect(clip.x, clip.y, clip.width, clip.height);

    const browsingContext = this._pageTarget.linkedBrowser().browsingContext;
    // `win.devicePixelRatio` returns a non-overriden value to priveleged code.
    // See https://bugzilla.mozilla.org/show_bug.cgi?id=1761032
    // See https://phabricator.services.mozilla.com/D141323
    const devicePixelRatio = browsingContext.overrideDPPX || this._pageTarget._window.devicePixelRatio;
    const scale = omitDeviceScaleFactor ? 1 : devicePixelRatio;
    const canvasWidth = rect.width * scale;
    const canvasHeight = rect.height * scale;

    const MAX_CANVAS_DIMENSIONS = 32767;
    const MAX_CANVAS_AREA = 472907776;
    if (canvasWidth > MAX_CANVAS_DIMENSIONS || canvasHeight > MAX_CANVAS_DIMENSIONS)
      throw new Error('Cannot take screenshot larger than ' + MAX_CANVAS_DIMENSIONS);
    if (canvasWidth * canvasHeight > MAX_CANVAS_AREA)
      throw new Error('Cannot take screenshot with more than ' + MAX_CANVAS_AREA + ' pixels');

    let snapshot;
    while (!snapshot) {
      try {
        //TODO(fission): browsingContext will change in case of cross-group navigation.
        snapshot = await browsingContext.currentWindowGlobal.drawSnapshot(
          rect,
          scale,
          "rgb(255,255,255)"
        );
      } catch (e) {
        // The currentWindowGlobal.drawSnapshot might throw
        // NS_ERROR_LOSS_OF_SIGNIFICANT_DATA if called during navigation.
        // wait a little and re-try.
        await new Promise(x => setTimeout(x, 50));
      }
    }

    const win = browsingContext.topChromeWindow;
    const canvas = win.document.createElementNS('http://www.w3.org/1999/xhtml', 'canvas');
    canvas.width = canvasWidth;
    canvas.height = canvasHeight;
    let ctx = canvas.getContext('2d');
    ctx.drawImage(snapshot, 0, 0);
    snapshot.close();

    if (mimeType === 'image/jpeg') {
      if (quality < 0 || quality > 100)
        throw new Error('Quality must be an integer value between 0 and 100; received ' + quality);
      quality /= 100;
    } else {
      quality = undefined;
    }
    const dataURL = canvas.toDataURL(mimeType, quality);
    return { data: dataURL.substring(dataURL.indexOf(',') + 1) };
  }

  async ['Page.getContentQuads'](options) {
    return await this._contentPage.send('getContentQuads', options);
  }

  async ['Page.navigate']({frameId, url, referer}) {
    const browsingContext = this._pageTarget.frameIdToBrowsingContext(frameId);
    let sameDocumentNavigation = false;
    let _uri;
    try {
      _uri = NetUtil.newURI(url);
    } catch (e) {
      throw new Error(`Invalid url: "${url}"`);
    }
    try {
      // This is the same check that verifies browser-side if this is the same-document navigation.
      // See CanonicalBrowsingContext::SupportsLoadingInParent.
      // equalsExceptRef can throw if currentURI is a special/incompatible type (e.g. during
      // proxy auth error or initial page load) - treat as cross-document navigation in that case.
      sameDocumentNavigation = browsingContext.currentURI && _uri.hasRef && _uri.equalsExceptRef(browsingContext.currentURI);
    } catch (e) {
      sameDocumentNavigation = false;
    }
    let referrerURI = null;
    let referrerInfo = null;
    if (referer) {
      try {
        referrerURI = NetUtil.newURI(referer);
        const ReferrerInfo = Components.Constructor(
          '@mozilla.org/referrer-info;1',
          'nsIReferrerInfo',
          'init'
        );
        referrerInfo = new ReferrerInfo(Ci.nsIReferrerInfo.UNSAFE_URL, true, referrerURI);
      } catch (e) {
        throw new Error(`Invalid referer: "${referer}"`);
      }
    }

    let navigationId;
    const unsubscribe = helper.addObserver((browsingContext, topic, loadIdentifier) => {
      navigationId = helper.toProtocolNavigationId(loadIdentifier);
    }, 'juggler-navigation-started-browser');
    browsingContext.loadURI(Services.io.newURI(url), {
      triggeringPrincipal: Services.scriptSecurityManager.getSystemPrincipal(),
      loadFlags: Ci.nsIWebNavigation.LOAD_FLAGS_IS_LINK,
      referrerInfo,
      // postData: null,
      // headers: null,
      // Fake user activation.
      hasValidUserGestureActivation: true,
    });
    unsubscribe();

    return {
      navigationId: sameDocumentNavigation ? null : navigationId,
    };
  }

  async ['Page.goBack']({}) {
    const browsingContext = this._pageTarget.linkedBrowser().browsingContext;
    // goBack() below traverses to index - 1 like history.back(), so the guard
    // must ask the same question. canGoBack is the back BUTTON's rule: with
    // browser.navigation.requireUserInteraction it skips entries nobody
    // interacted with, and refused a go_back over an entry the page left by
    // script (or one whose interaction mark was lost under load). Marionette's
    // WebDriver Back uses the same getter.
    if (!browsingContext.embedderElement?.canGoBackIgnoringUserInteraction)
      return { success: false };
    browsingContext.goBack();
    return { success: true };
  }

  async ['Page.goForward']({}) {
    const browsingContext = this._pageTarget.linkedBrowser().browsingContext;
    if (!browsingContext.embedderElement?.canGoForward)
      return { success: false };
    browsingContext.goForward();
    return { success: true };
  }

  async ['Page.reload']() {
    await this._pageTarget.activateAndRun(() => {
      const doc = this._pageTarget._tab.linkedBrowser.ownerDocument;
      doc.getElementById('Browser:Reload').doCommand();
    });
  }

  async ['Page.elementAtom'](options) {
    return await this._contentPage.send('elementAtom', options);
  }

  // The title as Marionette reads it, in the parent: the top document's
  // window global, so a page busy in a script does not hold the answer.
  ['Page.title']() {
    const browsingContext = this._pageTarget.linkedBrowser().browsingContext;
    return {title: browsingContext?.currentWindowGlobal?.documentTitle ?? ''};
  }

  async ['Page.describeNode'](options) {
    return await this._contentPage.send('describeNode', options);
  }

  async ['Page.scrollIntoViewIfNeeded'](options) {
    return await this._contentPage.send('scrollIntoViewIfNeeded', options);
  }

  async ['Page.setInitScripts']({ scripts }) {
    return await this._pageTarget.setInitScripts(scripts);
  }

  // THE KEYBOARD AND THE MOUSE TOOK DIFFERENT ROADS, AND THE PAGE COULD SEE
  // THE DIFFERENCE.
  //
  // `dispatchMouseEvent`, `dispatchWheelEvent`, `reload` and `bringToFront` all
  // go through `activateAndRun()`, which calls `window.focus()` and selects the
  // tab BEFORE acting - the upstream comment on the mouse path says why ("We
  // must switch to proper tab..."). The keyboard path did not: it went straight
  // to the content process, so keys reached a page that did NOT have focus. A
  // human cannot do that: typing into a window means bringing it forward first,
  // which is exactly what our mouse already did.
  //
  // Measured 2026-08-23 on the product, reading `document.hasFocus()` INSIDE the
  // keydown handler - five cases, and only the last one was healthy:
  //   1) one page, typing                       -> true   (the common, healthy case)
  //   2) two pages, typing into the front one    -> true
  //   3) two pages, typing into the BACK one     -> FALSE  <- the tell
  //   4) same back page, but with the MOUSE      -> true   (activateAndRun)
  //   5) keyboard right after that click         -> true   (the click had activated it)
  // So the defect is not "focus is missing", it is the ASYMMETRY: the same page
  // at the same instant answers true to the mouse and false to the keyboard, and
  // one `document.hasFocus()` inside a keydown is enough to see it.
  //
  // The remedy is at the origin and is not a patch: focus is not faked and
  // `hasFocus` is not corrected, the keyboard's input is routed through the SAME
  // activation the mouse already uses. When the tab is already the selected one,
  // `activateAndRun` only calls `window.focus()` on an already focused window,
  // which emits nothing and costs nothing.
  //
  // It holds for all direct input - keys, inserted text, touch - not only for
  // the measured case: the defect belongs to the class, and fixing one member
  // would leave the others to diverge on their own (rule 16).
  async ['Page.dispatchKeyEvent']({type, keyCode, code, key, repeat, location, text, latency}) {
    // key events don't fire if we are dragging.
    if (this._isDragging) {
      if (type === 'keydown' && key === 'Escape') {
        await this._sendToCurrentDocument('dispatchDragEvent', {
          type: 'dragover',
          x: this._lastMousePosition.x,
          y: this._lastMousePosition.y,
          modifiers: 0
        });
        await this._sendToCurrentDocument('dispatchDragEvent', {type: 'dragend'});
        // Escape ends the gesture as surely as the release does, and the
        // watcher goes with it: it still holds the `dragstart` that started
        // this drag, so without this the next `mousemove` would adopt the very
        // session Escape just cancelled.
        this._endGesture();
      }
      return;
    }
    return await this._pageTarget.activateAndRun(() =>
      this._contentPage.send('dispatchKeyEvent', {type, keyCode, code, key, repeat, location, text, latency}));
  }

  /**
   * The content end we were talking to is gone, so everything aimed at it ends.
   *
   * ⛔ A REQUEST AIMED AT A DOCUMENT USED TO OUTLIVE THE DOCUMENT. SimpleChannel
   * keeps a pending request across a transport reset so that a NEW actor can
   * replay it - right for a transport hiccup, wrong for anything only the old
   * document could answer. Observed both ways: a `dispatchDragEvent` replayed
   * into the NEXT document, and - on a reload during a drag - the actor removed
   * with no replacement, the request pending for the life of the page, and
   * `Page.dispatchMouseEvent` never answering again while `evaluate` and `url`
   * kept working. [B216]
   *
   * The transport is not the place to fix that: it does not know what a
   * document is and should not. The caller does, and says so through
   * `_sendToCurrentDocument`. This is the signal that ends those waits, and it
   * is `PageTarget.removeActor` because that is the one measured to still
   * arrive: it runs in the parent and does not depend on content speaking.
   * `pageNavigationCommitted` and `pageReady` were tried first and do not
   * arrive in that case - they come from content, which has stopped.
   *
   * ⛔ AND IT ENDS ONLY THE GESTURE AND THESE WAITS. An earlier version also
   * disposed every pending event watcher here, on every actor swap - which is
   * every navigation - and that broke input delivery: full e2e red 2 runs out
   * of 2 on two different pointer actions, green 2 out of 2 without it. The
   * gesture's own watcher already ends with the gesture.
   */
  _onContentDetached() {
    this._endGesture();
    const waits = [...this._documentScopedWaits];
    this._documentScopedWaits.clear();
    for (const fail of waits)
      fail(new Error('the document this was aimed at is gone'));
  }

  /**
   * Send to content, and give up if the document it was aimed at goes away.
   * The plain `send` has no end of its own: see `_onContentDetached`.
   */
  async _sendToCurrentDocument(methodName, params) {
    let fail;
    const abandoned = new Promise((_, reject) => { fail = reject; });
    this._documentScopedWaits.add(fail);
    try {
      return await Promise.race([this._contentPage.send(methodName, params), abandoned]);
    } finally {
      this._documentScopedWaits.delete(fail);
    }
  }

  _disposeDragGestureWatcher() {
    if (!this._dragGestureWatcher)
      return;
    this._dragGestureWatcher.dispose();
    this._dragGestureWatcher = null;
  }

  /**
   * The gesture is over. Every piece of state that describes it ends here.
   *
   * ⛔ `_isDragging` DESCRIBES A GESTURE AND USED TO BE CLEARED ON A SUCCESS
   * PATH, which is the same defect the watcher had, one line away, and it
   * survived the fix that named it. The release cleared the flag as the last
   * statement of the drag branch, so anything that threw before it - a
   * `dragover` that never came back, a content side that found the session
   * already gone and raised on `session.dataTransfer` - left the flag TRUE with
   * no gesture to end it.
   *
   * A page in that state is not degraded, it is dead to the pointer: every
   * later `mousedown` returns at once, every `mousemove` is redirected to
   * `dispatchDragEvent`, and the page sees neither `mouseover` nor a click.
   * Nothing recovers it, because the only two places that cleared the flag both
   * require being in a drag that ends well. [B216]
   */
  _endGesture() {
    this._isDragging = false;
    this._disposeDragGestureWatcher();
  }

  /**
   * Adopt the drag session if one was born anywhere in this gesture.
   *
   * ⛔ WHY THIS IS NOT INLINE IN THE `mousemove` HANDLER, WHERE IT USED TO BE.
   * `_isDragging` describes the whole gesture, but it was derived inside the
   * handler of a SINGLE event, from a watcher that the same call created and
   * disposed. That only holds while the dispatch is acknowledged: upstream sent
   * mouse events through `win.synthesizeMouseEvent`, which takes a completion
   * callback, and closed `sendEvents` with `await Promise.all(promises)` - so by
   * the time the check ran, the renderer had processed the event and `dragstart`
   * had already been observed. The comment saying "[mousemove] - always emitted.
   * This was awaited as part of `sendEvents` call" is that contract.
   *
   * This fork dispatches through `jugglerSendMouseEvent`, which has no
   * completion callback, so the await went away and only the comment stayed.
   * From then on `dragstart` could arrive after its watcher had been disposed,
   * and nothing would ever set `_isDragging`: every later move stayed a plain
   * `mousemove`, the release stayed a plain `mouseup`, and the drop was lost.
   *
   * Measured on a 480 px travel: delivered in 5 runs out of 6 when the events
   * were sent back to back, and in 0 out of 6 as soon as they were 5 ms apart -
   * because the gap is exactly where no watcher was alive to catch the news.
   * It is also why a travel of one single `mousemove` never worked while two
   * identical ones did: the second event existed only to give a late
   * `dragstart` a live watcher to land in. [B213]
   *
   * Giving the watcher the lifetime of the gesture removes the hole rather than
   * making it smaller, so WHEN the news arrives stops mattering.
   */
  async _adoptDragSessionIfStarted() {
    if (this._isDragging || !this._dragGestureWatcher)
      return;
    if (!this._dragGestureWatcher.hasEvent('dragstart'))
      return;
    const eventObject = await this._dragGestureWatcher.ensureEvent('juggler-drag-finalized');
    this._isDragging = eventObject.dragSessionStarted;
  }

  /**
   * Wait until the page's renderer has HANDLED mouse event `eventId`.
   *
   * ⛔ THE CONTRACT OF `Page.dispatchMouseEvent`, AND THE FORK HAD LOST IT. When
   * the command answers, the event has happened in the page: its listeners have
   * run, its hit test was made on the layout of that moment. Upstream kept this
   * by awaiting the completion callback of `synthesizeMouseEvent`; this fork
   * dispatches through `jugglerSendMouseEvent`, which has none, so the command
   * answered when the event was HANDED TO THE WIDGET. A mouse event then waits
   * in the content process - a press behind a coalesced move waits for the
   * refresh tick - while the client's next call, on another channel, can run
   * first. [B230], measured under load on Linux: a `mousedown` sent before a
   * `scrollIntoView` was hit-tested after it, 39 px off, on the gap between the
   * drag source and the drop zone, so the drag never started; and a script read
   * sent after a double click saw two clicks out of three. A low-level probe:
   * 3 reads in 300 before the press was handled. The element actions did not
   * show it only because `Page.pointerLanded` used to wait for this same ack
   * on its own; the pointer primitives (Selenium's ActionChains, pyppeteer's
   * Mouse, Playwright's page.mouse) asked nothing.
   *
   * The ack is the one [B217] added: PresShell notifies it AFTER handling the
   * event, and `FrameTree.whenEventHit` resolves on it. Waiting here, once, puts
   * every later call of every client behind the input, instead of each caller
   * remembering to ask - so the two places that did ask (`Page.pointerLanded`
   * and the release-time drag question, both with an `afterEventId`) are gone.
   *
   * With a `landingToken` (from `armLanding`, before the event was sent) the
   * answer also carries where the event of `type` LANDED, judged in the same
   * turn as the ack. Returns that landing, or null: no token, or a dialog
   * ended the wait.
   *
   * ⛔ A DIALOG ENDS THE WAIT. A click that opens `alert()` runs the dialog
   * inside its own handler, so the ack comes only once the dialog is closed -
   * and closing it is the client's job, after this command has answered. A
   * dialog already open means the content process is inside one: nothing it
   * receives now is handled until it closes, so there is nothing to wait for.
   *
   * ⛔ BOUNDED, AND A BOUND THAT EXPIRES IS AN ERROR, not an answer: the event
   * was sent and the page has not handled it, and saying "done" would bring the
   * defect back silently. Five seconds, as the other waits on this ack.
   */
  async _awaitInputHandled(eventId, type, landingToken) {
    if (this._pageTarget.dialogs().length)
      return null;
    const offs = [];
    const dialogOpened = new Promise(resolve => {
      offs.push(helper.on(this._pageTarget, PageTarget.Events.DialogOpened, resolve));
    });
    let timer;
    const expired = new Promise(resolve => { timer = setTimeout(() => resolve('expired'), 5000); });
    try {
      const outcome = await Promise.race([
        this._contentPage.send('awaitInputHandled', { eventId, type, landingToken }),
        dialogOpened.then(() => 'dialog'),
        expired,
      ]);
      if (outcome === 'expired')
        throw new Error('the page did not handle mouse event ' + eventId + ' within 5 s');
      if (outcome === 'dialog')
        return null;
      return (outcome && outcome.landing) || null;
    } finally {
      clearTimeout(timer);
      helper.removeListeners(offs);
    }
  }

  async ['Page.dispatchTapEvent'](options) {
    return await this._pageTarget.activateAndRun(() =>
      this._contentPage.send('dispatchTapEvent', options));
  }

  /**
   * The page's CSS pixels, as this window counts them.
   *
   * Points on this protocol are in the page's CSS pixels, while the mouse and
   * the wheel are dispatched through this chrome window (jugglerSendMouseEvent,
   * sendWheelEvent), which take chrome CSS pixels. THE ONLY PLACE THAT CONVERSION IS WRITTEN. Device
   * scaling needs nothing here, the native dispatch applies it; browser zoom
   * does, and the factor is the zoom IN EFFECT in the content process, not the
   * requested `BrowsingContext.fullZoom`: Firefox rounds it to whole app units
   * per device pixel, so a site saved at 110% (the first Ctrl++ step) is
   * 1.0909 there. Multiplying by 1.1 put a click 15 px off at x = 1740;
   * without any factor, 145 px.
   */
  async _pagePixels() {
    const browser = this._pageTarget._linkedBrowser;
    const { zoom } = await this._contentPage.send('effectiveZoom');
    return {
      // Whether the point is inside the page's viewport.
      contains: (x, y) => {
        const rect = browser.getBoundingClientRect();
        return x >= 0 && y >= 0 && x * zoom <= rect.width && y * zoom <= rect.height;
      },
      // The point in the window. Read when asked, so after any scroll of the
      // <browser> itself.
      inWindow: (x, y) => {
        const rect = browser.getBoundingClientRect();
        return { x: x * zoom + rect.left, y: y * zoom + rect.top };
      },
    };
  }

  async ['Page.dispatchMouseEvent']({type, x, y, button, clickCount, modifiers, buttons, landsOn, latency}) {
    const win = this._pageTarget._window;
    const page = await this._pagePixels();
    // The element this event is meant for, held by the content process BEFORE
    // anything is sent: the event's handler may take it away. [B230]
    const armed = landsOn ? await this._contentPage.send('armLanding', landsOn) : null;
    // The id of the last mouse event handed to the widget, which the renderer
    // acks once it has handled it: the command answers after that ack. [B230]
    let lastEventId = 0;
    // False only for the move sent off the web content (below): the renderer of
    // the page never handles it, so there is no ack to wait for.
    let landsInPage = true;
    const sendEvents = async (types) => {
      // ⛔ NO APZ FLUSH HERE, AND NO SCROLL OF THE <browser> BEFORE IT. Upstream
      // Juggler scrolled the <browser> into the chrome window
      // (scrollRectIntoViewIfNeeded) and then waited for `apz-repaints-flushed`
      // so that scroll reached the main thread before the point was computed.
      // In this tree no binding of Element declares that method (B184), so the
      // scroll never ran and the flush waited for an update that never came.
      // And the wait had no bound: flushApzRepaints() answers true once it has
      // SENT the request, and the compositor drops it in silence when its
      // bridge is not initialized or the layer tree is gone - test-only code on
      // desktop (APZCTreeManager::FlushApzRepaints) - which stopped the
      // activation chain, and with it the input of every tab, for good. The
      // page's own scrolling lives in its process, which this window's flush
      // never touched.
      const point = page.inWindow(x, y);

      for (const type of types) {
        lastEventId = win.windowUtils.jugglerSendMouseEvent(
          type,
          point.x,
          point.y,
          button,
          clickCount,
          modifiers,
          false /* aIgnoreRootScrollFrame */,
          (buttons ? 0.5 : 0.0) /* pressure: real mouse = 0.5 while a button is down */,
          1 /* inputSource: real mouse = MOZ_SOURCE_MOUSE (synthetic was 0 = automation tell) */,
          true /* isDOMEventSynthesized */,
          false /* isWidgetEventSynthesized */,
          buttons,
          win.windowUtils.DEFAULT_MOUSE_POINTER_ID /* pointerIdentifier */,
          latency || 0 /* [B299] born this many ms before the dispatch */
        );
      }
    };

    // We must switch to proper tab in the tabbed browser so that
    // 1. Event is dispatched to a proper renderer.
    // 2. We receive an ack from the renderer for the dispatched event.
    await this._pageTarget.activateAndRun(async () => {
      this._pageTarget.ensureContextMenuClosed();
      // If someone asks us to dispatch mouse event outside of viewport, then we normally would drop it.
      if (!page.contains(x, y)) {
        if (type !== 'mousemove')
          return;

        // A special hack: if someone tries to do `mousemove` outside of
        // viewport coordinates, then move the mouse off from the Web Content.
        // This way we can eliminate all the hover effects.
        landsInPage = false;
        lastEventId = win.windowUtils.jugglerSendMouseEvent(
          'mousemove',
          0 /* x */,
          0 /* y */,
          button,
          clickCount,
          modifiers,
          false /* aIgnoreRootScrollFrame */,
          (buttons ? 0.5 : 0.0) /* pressure: real mouse = 0.5 while a button is down */,
          1 /* inputSource: real mouse = MOZ_SOURCE_MOUSE (synthetic was 0 = automation tell) */,
          true /* isDOMEventSynthesized */,
          false /* isWidgetEventSynthesized */,
          buttons,
          win.windowUtils.DEFAULT_MOUSE_POINTER_ID /* pointerIdentifier */,
          latency || 0 /* [B299] born this many ms before the dispatch */
        );
        return;
      }

      if (type === 'mousedown') {
        if (this._isDragging)
          return;

        // The gesture starts here, so the watcher for it starts here. Which of
        // the events between this press and the release carries the `dragstart`
        // is not ours to predict, and it used to be predicted.
        this._disposeDragGestureWatcher();
        this._dragGestureWatcher = new EventWatcher(
            this._pageEventSink, ['dragstart', 'juggler-drag-finalized'],
            this._pendingEventWatchers);

        const eventNames = button === 2 ? ['mousedown', 'contextmenu'] : ['mousedown'];
        await sendEvents(eventNames);
        return;
      }

      if (type === 'mousemove') {
        this._lastMousePosition = { x, y };
        // Before deciding what this move is, catch up: the drag may have been
        // born during an earlier call, or in the gap between two of them.
        await this._adoptDragSessionIfStarted();
        if (this._isDragging) {
          // The reply is the acknowledgement: jugglerSendDragEvent dispatches
          // synchronously in the content process, so when it answers the
          // dragover has reached the page or never will. It used to wait as well
          // for the page's `dragover` to be reported, without a bound, and an
          // event that reached no frame stopped the activation chain for good.
          await this._sendToCurrentDocument('dispatchDragEvent', {type:'dragover', x, y, modifiers});
          return;
        }

        /* STEALTHFOX_HUMANIZE_HOOK: expand mousemove into a Bezier trajectory. */
        if (_stealthfoxHumanize.enabled()) {
          const path = _stealthfoxHumanize.trajectory(
              this._humanizeFromX || 0, this._humanizeFromY || 0,
              x, y, _stealthfoxHumanize.maxTimeS());
          const stepDelayMs = _stealthfoxHumanize.stepMs();
          for (let i = 1; i < path.length - 1; i++) {
            const [px, py] = path[i];
            if (!page.contains(px, py)) continue;
            const point = page.inWindow(px, py);
            try {
              lastEventId = win.windowUtils.jugglerSendMouseEvent('mousemove',
                  point.x, point.y, button, clickCount, modifiers,
                  false, (buttons ? 0.5 : 0.0) /* pressure */, 1 /* inputSource: real mouse */, true, false, buttons,
                  win.windowUtils.DEFAULT_MOUSE_POINTER_ID, latency || 0);
            } catch (e) { /* ignore per-step errors */ }
            // Jittered inter-sample delay: a real human's pointer dt is non-uniform;
            // a fixed stepMs was a behavioral tell. Gaussian around stepMs.
            //
            // The floor is ONE FRAME at 60 Hz, not 2 ms. Firefox coalesces
            // mousemove at the refresh rate, so a page on a real 60 Hz display
            // cannot see two of them 2 ms apart: that floor produced intervals
            // no real hardware generates. Measured 2026-08-24 with stepMs=10:
            // 79% of the dt below 16.7 ms, minimum 2 ms. The wrapper's Python
            // generator, which is the default path and the reference, gives a
            // mean of 31.9 ms and a minimum of 16 on the same movement.
            const d = Math.max(16, Math.round(_stealthfoxHumanize._gauss(stepDelayMs, stepDelayMs * 0.4)));
            await new Promise(r => setTimeout(r, d));
          }
          this._humanizeFromX = x;
          this._humanizeFromY = y;
        }
        await sendEvents(['mousemove']);

        // And catch up again, in case this move is the one that gave birth to
        // the drag. `sendEvents` does NOT wait for the renderer here - see
        // `_adoptDragSessionIfStarted` - so this check can miss, and the gesture
        // watcher is what makes a miss harmless instead of final.
        await this._adoptDragSessionIfStarted();
        return;
      }

      if (type === 'mouseup') {
        // The last chance to notice, and what the watcher holds here is
        // complete: every move of this gesture was answered only once the page
        // had handled it (`_awaitInputHandled`), and a `dragstart` it caused
        // reached this process ahead of that answer, on the same channel. So a
        // drag born by the last move is seen here, and the question this used
        // to ask the content process at this point ([B213]) closed a gap that
        // no longer exists. [B230]
        await this._adoptDragSessionIfStarted();
        try {
          if (this._isDragging) {
            // Each reply is the acknowledgement (see the mousemove above).
            await this._sendToCurrentDocument('dispatchDragEvent', {type: 'dragover', x, y, modifiers});
            await this._sendToCurrentDocument('dispatchDragEvent', {type: 'drop', x, y, modifiers});
            await this._sendToCurrentDocument('dispatchDragEvent', {type: 'dragend', x, y, modifiers});
            // NOTE:
            // - 'drop' event might not be dispatched at all, depending on dropAction.
            // - 'dragend' event might not be dispatched at all, if the source element was removed
            //   during drag. However, it'll be dispatched synchronously in the renderer.
          } else {
            await sendEvents(['mouseup']);
          }
        } finally {
          // The gesture is over either way - including when the branch above
          // threw - so everything that describes it ends here.
          this._endGesture();
        }
        return;
      }
    }, { muteNotificationsPopup: true });
    if (!lastEventId || !landsInPage) {
      if (!armed)
        return {};
      return { landing: { type, landed: false, seen: 0,
                          on: landsInPage ? 'no mouse event was dispatched' : 'a point outside the page' } };
    }
    const landing = await this._awaitInputHandled(lastEventId, type, armed ? armed.token : 0);
    return landing ? { landing } : {};
  }

  async ['Page.dispatchWheelEvent']({x, y, button, deltaX, deltaY, deltaZ, modifiers, latency }) {
    const deltaMode = 0; // WheelEvent.DOM_DELTA_PIXEL
    const lineOrPageDeltaX = deltaX > 0 ? Math.floor(deltaX) : Math.ceil(deltaX);
    const lineOrPageDeltaY = deltaY > 0 ? Math.floor(deltaY) : Math.ceil(deltaY);

    const page = await this._pagePixels();
    await this._pageTarget.activateAndRun(async () => {
      this._pageTarget.ensureContextMenuClosed();

      // Where the point is in the window. No scroll and no APZ flush before
      // it: see Page.dispatchMouseEvent.
      const point = page.inWindow(x, y);

      const win = this._pageTarget._window;

      win.windowUtils.sendWheelEvent(
        point.x,
        point.y,
        deltaX,
        deltaY,
        deltaZ,
        deltaMode,
        modifiers,
        lineOrPageDeltaX,
        lineOrPageDeltaY,
        0 /* options */,
        null /* callback */,
        latency || 0 /* [B301] born this many ms before the dispatch */);
    }, { muteNotificationsPopup: true });
  }

  // Same class as `Page.dispatchKeyEvent` above, and the same reason: it is
  // input reaching the page, so it must find the window focused.
  async ['Page.insertText'](options) {
    return await this._pageTarget.activateAndRun(() =>
      this._contentPage.send('insertText', options));
  }

  async ['Page.handleDialog']({dialogId, accept, promptText}) {
    const dialog = this._pageTarget.dialog(dialogId);
    if (!dialog)
      throw new Error('Failed to find dialog with id = ' + dialogId);
    if (accept)
      dialog.accept(promptText);
    else
      dialog.dismiss();
  }

  async ['Page.setInterceptFileChooserDialog']({ enabled }) {
    return await this._pageTarget.setInterceptFileChooserDialog(enabled);
  }

  async ['Page.sendMessageToWorker']({workerId, message}) {
    const worker = this._workers.get(workerId);
    if (!worker)
      throw new Error('ERROR: cannot find worker with id ' + workerId);
    return await worker.sendMessage(JSON.parse(message));
  }

  async ['Page.startScreencast'](options) {
    return await this._pageTarget.startScreencast(options);
  }

  async ['Page.screencastFrameAck'](options) {
    this._pageTarget.screencastFrameAck(options);
  }

  async ['Page.stopScreencast']() {
    this._pageTarget.stopScreencast();
  }
}
