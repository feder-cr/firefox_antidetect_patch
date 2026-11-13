/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const {Helper} = ChromeUtils.importESModule('resource://gre/modules/juggler/Helper.js');
const {setTimeout} = ChromeUtils.importESModule('resource://gre/modules/Timer.sys.mjs');
const {StealthCursor} = ChromeUtils.importESModule('resource://gre/modules/juggler/StealthCursor.js');
const {ContentMux} = ChromeUtils.importESModule('resource://gre/modules/juggler/ContentMux.js');
const {GoneContexts} = ChromeUtils.importESModule('resource://gre/modules/juggler/GoneContexts.js');
const {newProxyInfoFor, proxyForHost} = ChromeUtils.importESModule('resource://gre/modules/juggler/SessionRoute.js');
const {syncTrrPrefs} = ChromeUtils.importESModule('resource://gre/modules/juggler/TrrPrefs.js');
const {Preferences} = ChromeUtils.importESModule("resource://gre/modules/Preferences.sys.mjs");
const {ContextualIdentityService} = ChromeUtils.importESModule("moz-src:///toolkit/components/contextualidentity/ContextualIdentityService.sys.mjs");
const {NetUtil} = ChromeUtils.importESModule('resource://gre/modules/NetUtil.sys.mjs');
const {AppConstants} = ChromeUtils.importESModule("resource://gre/modules/AppConstants.sys.mjs");

const Cr = Components.results;

const helper = new Helper();

// A root is told apart by its page and by its browsing context.
function rootKey(browserId, rootId) {
  return browserId + '/' + rootId;
}


// Chrome flag of a popup whose window.open features give a size: the value
// of kJugglerWindowExplicitSize in toolkit/components/windowwatcher/
// JugglerWindowSize.h, a bit nsIWebBrowserChrome leaves free
// (juggler/tests/explicit_size_flag_is_free.test.mjs).
const JUGGLER_WINDOW_EXPLICIT_SIZE = 1 << 1;

// The size the page's <browser> is pinned to, or undefined when it follows the
// window. A window in fullscreen is the screen, content included, as in every
// Firefox (outer == inner == screen): pinned, the page kept the maximized
// window's viewport inside a fullscreen window
// (juggler/tests/fullscreen_releases_the_viewport.test.mjs).
function pinnedViewportSize(windowInFullscreen, pageViewportSize, contextViewportSize) {
  if (windowInFullscreen)
    return undefined;
  return pageViewportSize || contextViewportSize;
}
const IDENTITY_NAME = 'JUGGLER ';

// How long a context that is being removed waits for its content processes to exit once its pages are closed,
// how long it waits again after killing the ones that did not, and how often it looks.
const PROCESS_EXIT_WAIT_MS = 3000;
const PROCESS_KILL_WAIT_MS = 2000;
const PROCESS_POLL_MS = 20;
const HUNDRED_YEARS = 60 * 60 * 24 * 365 * 100;

// The name of an nsICookieValidation result, e.g. eRejectedNoneRequiresSecure.
function validationName(result) {
  for (const key of Object.keys(Ci.nsICookieValidation)) {
    if (key.startsWith('eRejected') && Ci.nsICookieValidation[key] === result)
      return key;
  }
  return String(result);
}

const ALL_PERMISSIONS = [
  'geo',
  'desktop-notification',
];

let globalTabAndWindowActivationChain = Promise.resolve();
// This is a workaround for https://github.com/microsoft/playwright/issues/34586
let didCreateFirstPage = false;
let globalNewPageChain = Promise.resolve();

class DownloadInterceptor {
  constructor(registry) {
    this._registry = registry
    this._handlerToUuid = new Map();
    this._uuidToHandler = new Map();
  }

  //
  // nsIDownloadInterceptor implementation.
  //
  interceptDownloadRequest(externalAppHandler, request, browsingContext, outFile) {
    if (!(request instanceof Ci.nsIChannel))
      return false;
    const channel = request.QueryInterface(Ci.nsIChannel);
    let pageTarget = this._registry._browserIdToTarget.get(channel.loadInfo.browsingContext.top.browserId);
    if (!pageTarget)
      return false;

    const browserContext = pageTarget.browserContext();
    const options = browserContext.downloadOptions;
    if (!options)
      return false;

    const uuid = helper.generateId();
    let file = null;
    if (options.behavior === 'saveToDisk') {
      file = Cc["@mozilla.org/file/local;1"].createInstance(Ci.nsIFile);
      file.initWithPath(options.downloadsDir);
      file.append(uuid);

      try {
        file.create(Ci.nsIFile.NORMAL_FILE_TYPE, 0o600);
      } catch (e) {
        dump(`WARNING: interceptDownloadRequest failed to create file: ${e}\n`);
        return false;
      }
    }
    outFile.value = file;
    this._handlerToUuid.set(externalAppHandler, uuid);
    this._uuidToHandler.set(uuid, externalAppHandler);
    const downloadInfo = {
      uuid,
      browserContextId: browserContext.browserContextId,
      pageTargetId: pageTarget.id(),
      frameId: helper.browsingContextToFrameId(channel.loadInfo.browsingContext),
      url: request.name,
      suggestedFileName: externalAppHandler.suggestedFileName,
    };
    this._registry.emit(TargetRegistry.Events.DownloadCreated, downloadInfo);
    return true;
  }

  // nsIDownloadInterceptor.onDownloadComplete(aHandler, aErrorName): the error
  // name is empty on success and NS_BINDING_ABORTED for a cancel.
  onDownloadComplete(externalAppHandler, errorName) {
    const uuid = this._handlerToUuid.get(externalAppHandler);
    if (!uuid)
      return;
    this._handlerToUuid.delete(externalAppHandler);
    this._uuidToHandler.delete(uuid);
    const downloadInfo = { uuid };
    if (errorName === 'NS_BINDING_ABORTED')
      downloadInfo.canceled = true;
    else if (errorName)
      downloadInfo.error = errorName;
    this._registry.emit(TargetRegistry.Events.DownloadFinished, downloadInfo);
  }

  async cancelDownload(uuid) {
    const externalAppHandler = this._uuidToHandler.get(uuid);
    if (!externalAppHandler) {
      return;
    }
    await externalAppHandler.cancel(Cr.NS_BINDING_ABORTED);
  }
}

/**
 * The window capture service, or null when this build has none.
 *
 * ⛔ NULL, NEVER A STUB OF FUNCTIONS THAT RETURN 0. That is what stood here
 * until this subsystem was rebuilt, and it is why a caller asking for a video
 * got a white file and no error: the contract id always threw, because
 * `juggler/screencast` was not in any `DIRS` and so was never compiled, and
 * the stub then swallowed every call. A missing capability has to be
 * ANNOUNCED - the three commands below refuse with a sentence naming the
 * cause - because a silent no-op is indistinguishable from a working feature
 * right up until somebody opens the output.
 */
const screencastService = (() => {
  try {
    return Cc['@mozilla.org/juggler/screencast;1'].getService(Ci.nsIScreencastService);
  } catch (e) {
    return null;
  }
})();

export class TargetRegistry {
  static instance() {
    return TargetRegistry._instance || null;
  }

  // The browser was started with the Juggler pipe: its session has a proxy to
  // be decided by a command, even before the registry exists (it is built at
  // final-ui-startup).
  static markDriven() {
    TargetRegistry._driven = true;
  }

  /**
   * The road of the requests that have no page (the pingsender at exit), as
   * `{decided, proxy}`. `proxy` is null for the default road. A driven session
   * whose proxy no command has decided is `{decided: false}`: the caller must
   * send nothing, because null would mean DIRECT. A browser Juggler does not
   * drive has nothing to wait for, and its road is the default one.
   */
  static roadOfSession(userContextId) {
    const registry = TargetRegistry._instance;
    if (!registry)
      return TargetRegistry._driven ? { decided: false } : { decided: true, proxy: null };
    if (!registry.browserProxyDecided)
      return { decided: false };
    return { decided: true, proxy: registry.proxyRecordFor(userContextId) };
  }

  /**
   * @param {{survivesLastWindow: boolean}} options
   *   survivesLastWindow: the browser was started with -silent, so Juggler
   *   holds it in the last-window-closing survival area and closing every
   *   window does not quit it. Without -silent (a persistent context) the
   *   last window to close quits the browser.
   */
  constructor({ survivesLastWindow }) {
    helper.decorateAsEventEmitter(this);
    TargetRegistry._instance = this;
    this._survivesLastWindow = survivesLastWindow;

    this._browserContextIdToBrowserContext = new Map();
    this._userContextIdToBrowserContext = new Map();
    this._browserToTarget = new Map();
    this._browserIdToTarget = new Map();
    // The Juggler actor of the current top-level document of each <browser>,
    // keyed by browserId, whether or not its PageTarget exists yet: the actor
    // of a new tab's about:blank usually arrives BEFORE the TabOpen that
    // creates the target, and the target picks it up from here.
    //
    // ⛔ browserId IS THE ONLY KEY. It is shared by every document of one
    // <browser> and survives the BrowsingContext replacement of a navigation
    // out of a bfcache-eligible page. A BrowsingContext id is a different
    // counter: an earlier version also matched actors on it, set the target's
    // entry under the FIRST context id and deleted it under the CURRENT one,
    // so every page that navigated twice left a stale entry behind, and the
    // first new tab whose browserId equalled that number lost its actor.
    // Browser.newPage then never answered (the fifth page, measured).
    //
    // A page has one actor per fragment root: the top, and every frame whose parent is in another
    // process. The key is the page and the root together (see rootKey).
    this._rootActors = new Map();

    this._proxiesWithClashingAuthCacheKeys = new Set();
    this._browserProxy = null;
    // Whether the first command has decided the road of the session (a proxy,
    // or none): until then the channel filter holds every channel unanswered.
    this._browserProxyDecided = false;
    this._browserProxyListeners = [];

    // Cleanup containers from previous runs (if any)
    for (const identity of ContextualIdentityService.getPublicIdentities()) {
      if (identity.name && identity.name.startsWith(IDENTITY_NAME)) {
        ContextualIdentityService.remove(identity.userContextId);
        ContextualIdentityService.closeContainerTabs(identity.userContextId);
      }
    }

    this._defaultContext = new BrowserContext(this, undefined, undefined);

    Services.obs.addObserver({
      observe: (subject, topic, data) => this._onFrameLoaderCrashed(subject, data),
    }, 'oop-frameloader-crashed');

    // A root whose browsing context is discarded has no process to wait for: without this its endpoint
    // would stay in the page's mux with no actor. The main root is not touched here; it changes its
    // browsing context with the top and ends with the page.
    Services.obs.addObserver({
      observe: (browsingContext, topic, data) => {
        let browserId;
        try {
          browserId = browsingContext.browserId;
        } catch (e) {
          return;
        }
        this._browserIdToTarget.get(browserId)?._contentMux.onRootRemoved(browsingContext.id);
        this._rootActors.delete(rootKey(browserId, browsingContext.id));
      }
    }, 'browsing-context-discarded');

    // A document that is not a process root, created for the browsing context of a root: the frame turned local (it
    // navigated to the site of its parent). The browsing context stays, so the root would stay with it.
    Services.obs.addObserver({
      observe: (windowGlobal, topic, data) => {
        try {
          const browsingContext = windowGlobal.browsingContext;
          if (!browsingContext.parent || windowGlobal.isProcessRoot)
            return;
          this._browserIdToTarget.get(browsingContext.browserId)?._contentMux.onRootTurnedLocal(browsingContext.id);
        } catch (e) {
          // a window global of a context that is going: nothing to do
        }
      }
    }, 'window-global-created');

    const onTabOpenListener = (appWindow, window, event) => {
      const tab = event.target;
      const userContextId = tab.userContextId;
      const browserContext = this._userContextIdToBrowserContext.get(userContextId);
      // Set by nsWindowWatcher::CalculateChromeFlagsForContent when the
      // window.open features give a size. Such a popup keeps the size the page
      // asked for, as in every Firefox; the context's default viewport is for
      // tabs and for popups that did not name one.
      const hasExplicitSize = appWindow && (appWindow.chromeFlags & JUGGLER_WINDOW_EXPLICIT_SIZE) !== 0;
      const openerContext = tab.linkedBrowser.browsingContext.opener;
      let openerTarget;
      if (openerContext) {
        // Popups usually have opener context. Get top context for the case when opener is
        // an iframe.
        openerTarget = this._browserIdToTarget.get(openerContext.top.browserId);
      } else if (tab.openerTab) {
        // Noopener popups from the same window have opener tab instead.
        openerTarget = this._browserToTarget.get(tab.openerTab.linkedBrowser);
      }
      if (!browserContext)
        throw new Error(`Internal error: cannot find context for userContextId=${userContextId}`);
      const target = new PageTarget(this, window, tab, browserContext, openerTarget);
      target.updateOverridesForBrowsingContext(tab.linkedBrowser.browsingContext);
      if (!hasExplicitSize)
        target.updateViewportSize();
    };

    const onTabCloseListener = event => {
      const tab = event.target;
      const linkedBrowser = tab.linkedBrowser;
      const target = this._browserToTarget.get(linkedBrowser);
      if (target)
          target.dispose();
    };

    const domWindowTabListeners = new Map();

    const onOpenWindow = async (appWindow) => {
      let domWindow;
      if (appWindow instanceof Ci.nsIAppWindow) {
        domWindow = appWindow.QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIDOMWindowInternal || Ci.nsIDOMWindow);
      } else {
        domWindow = appWindow;
        appWindow = null;
      }
      if (!domWindow.isChromeWindow)
        return;
      // In persistent mode, window might be opened long ago and might be
      // already initialized.
      //
      // In this case, we want to keep this callback synchronous so that we will call
      // `onTabOpenListener` synchronously and before the sync IPc message `juggler:content-ready`.
      if (domWindow.document.readyState === 'uninitialized' || domWindow.document.readyState === 'loading') {
        // For non-initialized windows, DOMContentLoaded initializes gBrowser
        // and starts tab loading (see //browser/base/content/browser.js), so we
        // are guaranteed to call `onTabOpenListener` before the sync IPC message
        // `juggler:content-ready`.
        await helper.awaitEvent(domWindow, 'DOMContentLoaded');
      }

      if (!domWindow.gBrowser)
        return;
      setupWindowTabListeners(appWindow, domWindow);
    };

    const setupWindowTabListeners = (appWindow, domWindow) => {
      if (domWindowTabListeners.has(domWindow))
        return;
      const tabContainer = domWindow.gBrowser.tabContainer;
      domWindowTabListeners.set(domWindow, [
        helper.addEventListener(tabContainer, 'TabOpen', event => onTabOpenListener(appWindow, domWindow, event)),
        helper.addEventListener(tabContainer, 'TabClose', onTabCloseListener),
      ]);
      for (const tab of domWindow.gBrowser.tabs)
        onTabOpenListener(appWindow, domWindow, { target: tab });
    };

    // FF150: Services.ww.openWindow() returns the window while it is still
    // about:blank (readyState=complete, gBrowser=null). onOpenWindow fires at
    // that point and returns early. This observer catches the window once
    // browser.xhtml finishes delayed startup and gBrowser is available.
    Services.obs.addObserver({
      observe(aSubject) {
        const domWindow = aSubject;
        if (!domWindow.isChromeWindow || !domWindow.gBrowser)
          return;
        // The tree owner of a browser window is an nsChromeTreeOwner, which
        // does not QueryInterface to nsIAppWindow: it hands out its
        // AppWindow through getInterface. The QueryInterface that was here
        // threw, the catch swallowed it, and every window that arrived this
        // way - every window.open popup does - came with a null appWindow.
        // onTabOpenListener reads the explicit-size chrome flag from it, so a
        // popup opened with 'width=500,height=400' was resized to the
        // context's default viewport like a tab.
        const appWindow = domWindow.docShell.treeOwner
            .QueryInterface(Ci.nsIInterfaceRequestor)
            .getInterface(Ci.nsIAppWindow);
        setupWindowTabListeners(appWindow, domWindow);
      }
    }, 'browser-delayed-startup-finished');

    const onCloseWindow = window => {
      const domWindow = window.QueryInterface(Ci.nsIInterfaceRequestor).getInterface(Ci.nsIDOMWindowInternal || Ci.nsIDOMWindow);
      if (!domWindow.isChromeWindow)
        return;
      if (!domWindow.gBrowser)
        return;

      const listeners = domWindowTabListeners.get(domWindow) || [];
      domWindowTabListeners.delete(domWindow);
      helper.removeListeners(listeners);
      for (const tab of domWindow.gBrowser.tabs)
        onTabCloseListener({ target: tab });
    };

    const extHelperAppSvc = Cc["@mozilla.org/uriloader/external-helper-app-service;1"].getService(Ci.nsIExternalHelperAppService);
    this._downloadInterceptor = new DownloadInterceptor(this);
    extHelperAppSvc.setDownloadInterceptor(this._downloadInterceptor);

    Services.wm.addListener({ onOpenWindow, onCloseWindow });
    for (const win of Services.wm.getEnumerator(null))
      onOpenWindow(win);
  }

  // Firefox uses nsHttpAuthCache to cache authentication to the proxy.
  // If we're provided with a single proxy with a multiple different authentications, then
  // we should clear the nsHttpAuthCache on every request.
  shouldBustHTTPAuthCacheForProxy(proxy) {
    return this._proxiesWithClashingAuthCacheKeys.has(proxy);
  }

  _updateProxiesWithSameAuthCacheAndDifferentCredentials() {
    const proxyIdToCredentials = new Map();
    const allProxies = [...this._browserContextIdToBrowserContext.values()].map(bc => bc._proxy).filter(Boolean);
    if (this._browserProxy)
      allProxies.push(this._browserProxy);
    const proxyAuthCacheKeyAndProxy = allProxies.map(proxy => [
      JSON.stringify({
        type: proxy.type,
        host: proxy.host,
        port: proxy.port,
      }),
      proxy,
    ]);
    this._proxiesWithClashingAuthCacheKeys.clear();

    proxyAuthCacheKeyAndProxy.sort(([cacheKey1], [cacheKey2]) => cacheKey1 < cacheKey2 ? -1 : 1);
    for (let i = 0; i < proxyAuthCacheKeyAndProxy.length - 1; ++i) {
      const [cacheKey1, proxy1] = proxyAuthCacheKeyAndProxy[i];
      const [cacheKey2, proxy2] = proxyAuthCacheKeyAndProxy[i + 1];
      if (cacheKey1 !== cacheKey2)
        continue;
      if (proxy1.username === proxy2.username && proxy1.password === proxy2.password)
        continue;
      // `proxy1` and `proxy2` have the same caching key, but serve different credentials.
      // We have to bust HTTP Auth Cache everytime there's a request that will use either of the proxies.
      this._proxiesWithClashingAuthCacheKeys.add(proxy1);
      this._proxiesWithClashingAuthCacheKeys.add(proxy2);
    }
  }

  async cancelDownload(options) {
    this._downloadInterceptor.cancelDownload(options.uuid);
  }

  setBrowserProxy(proxy) {
    // Built and thrown away, for the throw. A proxy that cannot be expressed
    // must fail the command that asked for it: the caller can tell "I set no
    // proxy" from "my proxy was refused", and cannot tell either from a
    // session that quietly goes out in the clear.
    newProxyInfoFor(proxy);
    const previous = this._browserProxy;
    this._browserProxy = proxy;
    try {
      // Before the hold is lifted: no channel leaves with a proxy and without DoH.
      this._syncTrrPrefs();
    } catch (e) {
      this._browserProxy = previous;
      throw e;
    }
    this._decideBrowserProxy();
  }

  // The session goes out with no proxy: an explicit decision, so that the
  // filter answers the channels that waited with the default road.
  setNoBrowserProxy() {
    const previous = this._browserProxy;
    this._browserProxy = null;
    try {
      // DoH stays on while a context still has a proxy; otherwise it goes off
      // before the held channels are answered with the default road.
      this._syncTrrPrefs();
    } catch (e) {
      this._browserProxy = previous;
      throw e;
    }
    this._decideBrowserProxy();
  }

  // The proxy records present: the browser-wide one and those of every live
  // context, each with its userContextId. The DoH prefs, and the list of the
  // contexts that have a proxy, follow this set (see TrrPrefs.js). It is the
  // same registry that proxyRecordFor() answers the channel filter from.
  _proxiesPresent() {
    return {
      browserProxy: this._browserProxy,
      contexts: [...this._browserContextIdToBrowserContext.values()]
          .map(bc => ({ userContextId: bc.userContextId, proxy: bc._proxy })),
    };
  }

  _syncTrrPrefs() {
    const {browserProxy, contexts} = this._proxiesPresent();
    syncTrrPrefs(browserProxy, contexts);
  }

  get browserProxyDecided() {
    return this._browserProxyDecided;
  }

  // `callback` runs every time the browser proxy is decided or changed.
  onBrowserProxyDecided(callback) {
    this._browserProxyListeners.push(callback);
  }

  _decideBrowserProxy() {
    this._updateProxiesWithSameAuthCacheAndDifferentCredentials();
    this._browserProxyDecided = true;
    for (const callback of this._browserProxyListeners)
      callback();
  }

  getProxyInfo(channel) {
    const originAttributes = channel.loadInfo && channel.loadInfo.originAttributes;
    return this.proxyFor(channel.URI.host, originAttributes ? originAttributes.userContextId : undefined);
  }

  /**
   * The proxy record a request to `host` leaves by, or null for the default
   * road. ONE lookup, asked by two callers: the channel filter, through
   * `getProxyInfo`, for every request of the session, and TelemetrySend for
   * the pingsender background task it starts at exit (B266), which must leave
   * by the road the browser's own request to that host would have taken. A
   * request with no origin attributes has no context and takes the browser's
   * proxy, as it always did.
   */
  proxyFor(host, userContextId) {
    return proxyForHost(this.proxyRecordFor(userContextId), host);
  }

  /**
   * The proxy record that governs a context, bypass list included, or null.
   * TelemetrySend hands THIS to the pingsender background task rather than an
   * answer for one host, so the task asks `proxyForHost` itself for whatever
   * it connects to - the ping, and anything the ping's TLS fetches on the way.
   */
  proxyRecordFor(userContextId) {
    const browserContext = userContextId === undefined ? null : this.browserContextForUserContextId(userContextId);
    // Prefer context proxy, then the proxy a context that ended had (its requests in flight still leave by it, never DIRECT),
    // and fallback to browser-level proxy.
    return (browserContext && browserContext._proxy) || GoneContexts.recordFor(userContextId) || this._browserProxy || null;
  }

  defaultContext() {
    return this._defaultContext;
  }

  createBrowserContext(removeOnDetach) {
    return new BrowserContext(this, helper.generateId(), removeOnDetach);
  }

  browserContextForId(browserContextId) {
    return this._browserContextIdToBrowserContext.get(browserContextId);
  }

  browserContextForUserContextId(userContextId) {
    return this._userContextIdToBrowserContext.get(userContextId);
  }

  async newPage({browserContextId}) {
    // When creating the very first page, we cannot create multiple in parallel.
    // See https://github.com/microsoft/playwright/issues/34586.
    if (didCreateFirstPage)
      return this._newPageInternal({browserContextId});
    const result = globalNewPageChain.then(() => this._newPageInternal({browserContextId}));
    globalNewPageChain = result.catch(error => { /* swallow errors to keep chain running */ });
    return result;
  }

  async _newPageInternal({browserContextId}) {
    const browserContext = this.browserContextForId(browserContextId);
    // The message Playwright turns into "Invalid timezone ID": see
    // BrowserContext.setTimezoneOverride. Asked before the window opens, so a
    // refused page leaves no window behind.
    if (browserContext.invalidTimezoneId !== undefined)
      throw new Error('Failed to override timezone');
    const features = "chrome,dialog=no,all";
    // See _callWithURIToLoad in browser.js for the structure of window.arguments
    // window.arguments[1]: unused (bug 871161)
    //                 [2]: referrerInfo (nsIReferrerInfo)
    //                 [3]: postData (nsIInputStream)
    //                 [4]: allowThirdPartyFixup (bool)
    //                 [5]: userContextId (int)
    //                 [6]: originPrincipal (nsIPrincipal)
    //                 [7]: originStoragePrincipal (nsIPrincipal)
    //                 [8]: triggeringPrincipal (nsIPrincipal)
    //                 [9]: allowInheritPrincipal (bool)
    //                 [10]: csp (nsIContentSecurityPolicy)
    //                 [11]: nsOpenWindowInfo
    const args = Cc["@mozilla.org/array;1"].createInstance(Ci.nsIMutableArray);
    const urlSupports = Cc["@mozilla.org/supports-string;1"].createInstance(
      Ci.nsISupportsString
    );
    urlSupports.data = 'about:blank';
    args.appendElement(urlSupports); // 0
    args.appendElement(undefined); // 1
    args.appendElement(undefined); // 2
    args.appendElement(undefined); // 3
    args.appendElement(undefined); // 4
    const userContextIdSupports = Cc[
      "@mozilla.org/supports-PRUint32;1"
    ].createInstance(Ci.nsISupportsPRUint32);
    userContextIdSupports.data = browserContext.userContextId;
    args.appendElement(userContextIdSupports); // 5
    args.appendElement(undefined); // 6
    args.appendElement(undefined); // 7
    args.appendElement(Services.scriptSecurityManager.getSystemPrincipal()); // 8

    const window = Services.ww.openWindow(null, AppConstants.BROWSER_CHROME_URL, '_blank', features, args);
    await waitForWindowReady(window);
    if (window.gBrowser.browsers.length !== 1)
      throw new Error(`Unexpected number of tabs in the new window: ${window.gBrowser.browsers.length}`);
    const browser = window.gBrowser.browsers[0];
    let target = this._browserToTarget.get(browser);
    while (!target) {
      await helper.awaitEvent(this, TargetRegistry.Events.TargetCreated);
      target = this._browserToTarget.get(browser);
    }
    browser.focus();
    didCreateFirstPage = true;
    return target.id();
  }

  targets() {
    return Array.from(this._browserToTarget.values());
  }

  targetForBrowser(browser) {
    return this._browserToTarget.get(browser);
  }

  targetForBrowserId(browserId) {
    return this._browserIdToTarget.get(browserId);
  }

  // Called by JugglerFrameParent for the actor of a CURRENT top-level document
  // only: when it is created, and when a document restored from the bfcache
  // becomes current again.
  onActorCreated(actor, browserId, rootId) {
    this._rootActors.set(rootKey(browserId, rootId), actor);
    this._browserIdToTarget.get(browserId)?.setActor(actor, rootId);
  }

  onActorDestroyed(actor, browserId, rootId) {
    this._browserIdToTarget.get(browserId)?.removeActor(actor, rootId);
    const key = rootKey(browserId, rootId);
    if (this._rootActors.get(key) === actor)
      this._rootActors.delete(key);
  }

  // ⛔ AN IFRAME'S PROCESS CRASHING IS NOT THE TAB CRASHING. The owner element of an out-of-process iframe's
  // BrowserParent is the <browser> of the TAB (BrowserBridgeParent::Init, SetOwnerElement), so the notification comes
  // with the tab's frameloader for either crash, and before the engine passed the crashed BrowsingContext's id as `data`
  // (nsFrameLoader::MaybeNotifyCrashed) the tab branch closed the whole page for a crash of one iframe. `data` is that
  // id, empty when the notification does not say (a frameloader that failed to build: a tab).
  //
  // A context that is named but already gone (the iframe was removed, or the page navigated, in the same turn as the
  // crash) is neither: there is nothing of it to tell, and the tab is not what crashed.
  _onFrameLoaderCrashed(frameLoader, data) {
    const browser = frameLoader.ownerElement;
    if (!browser) {
      return;
    }
    const target = this._browserToTarget.get(browser);
    if (!target) {
      return;
    }
    if (data) {
      const crashed = BrowsingContext.get(data);
      if (!crashed) {
        return;
      }
      if (crashed.parent) {
        target.frameCrashed(crashed);
        return;
      }
    }
    target.emit(PageTarget.Events.Crashed);
    target.dispose();
    target.closeCrashedTab();
  }

  targetForId(targetId) {
    for (const target of this._browserToTarget.values()) {
      if (target.id() === targetId)
        return target;
    }
    return null;
  }
}

export class PageTarget {
  constructor(registry, win, tab, browserContext, opener) {
    helper.decorateAsEventEmitter(this);

    this._targetId = helper.generateId();
    this._registry = registry;
    this._window = win;
    this._gBrowser = win.gBrowser;
    this._tab = tab;
    this._linkedBrowser = tab.linkedBrowser;
    // Kept, not re-read at dispose: the registry entry must be deleted under
    // the same key it was set under.
    this._browserId = this._linkedBrowser.browsingContext.browserId;
    this._browserContext = browserContext;
    this._viewportSize = undefined;
    // { screencastId } while a screencast is running on this page, undefined
    // otherwise. One per window: the capture module holds a single sink.
    this._screencastInfo = undefined;
    // ⛔ The visible pointer overlay, OFF unless `stealthfox.showcursor`
    // says otherwise. It lives in the chrome document of this window, which
    // is a different process from the content: the page cannot see it, and
    // it is absent from `page.screenshot()` by construction. See
    // `StealthCursor.js` for why that is the point and not a limitation.
    this._stealthCursor = StealthCursor.forWindow(win);
    this._initialDPPX = this._linkedBrowser.browsingContext.overrideDPPX;
    this._url = 'about:blank';
    this._openerId = opener ? opener.id() : undefined;
    this._actorSequenceNumber = 0;
    // What the page's handler talks through: one channel to the content process of each fragment root.
    this._contentMux = new ContentMux({
      pageTarget: this,
      name: `browser::page[${this._targetId}]`,
      uid: 'target-' + this._targetId,
      createChannel: (name, uid) => new SimpleChannel(name, uid),
      nextTask: callback => Services.tm.dispatchToMainThread(callback),
      wait: ms => new Promise(resolve => setTimeout(resolve, ms)),
    });
    this._dialogs = new Map();
    this.disableCache = false;
    this.mediumOverride = '';
    this.crossProcessCookie = {
      initScripts: [],
      bindings: [],
      interceptFileChooserDialog: false,
    };

    const navigationListener = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIWebProgressListener, Ci.nsISupportsWeakReference]),
      // The progress of a frame climbs to the browsing contexts above it (BrowsingContextWebProgress): without the
      // test the address of an iframe became the page's url, and the page's permissions were asked for it.
      onLocationChange: (aWebProgress, aRequest, aLocation) => {
        if (aWebProgress.isTopLevel)
          this._onNavigated(aLocation);
      },
    };
    this._eventListeners = [
      helper.addObserver(this._updateModalDialogs.bind(this), 'common-dialog-loaded'),
      helper.addProgressListener(tab.linkedBrowser, navigationListener, Ci.nsIWebProgress.NOTIFY_LOCATION),
      helper.addEventListener(this._linkedBrowser, 'DOMModalDialogClosed', event => this._updateModalDialogs()),
      // The chrome window's "fullscreen" event, on entering and on leaving
      // (nsGlobalWindowOuter::FinishFullscreenChange): the pin follows.
      helper.addEventListener(this._window, 'fullscreen', () => this.updateViewportSize().catch(e => {})),
    ];

    this._disposed = false;
    browserContext.pages.add(this);
    this._registry._browserToTarget.set(this._linkedBrowser, this);
    this._registry._browserIdToTarget.set(this._browserId, this);
    // The actors that were there before the target: the top first, so a root that is not the main one finds
    // the main endpoint made.
    const prefix = this._browserId + '/';
    const actors = [...this._registry._rootActors].filter(([key]) => key.startsWith(prefix));
    actors.sort(([, a], [, b]) => (a.browsingContext.parent ? 1 : 0) - (b.browsingContext.parent ? 1 : 0));
    for (const [, actor] of actors)
      this.setActor(actor, actor.browsingContext.id);

    this._registry.emit(TargetRegistry.Events.TargetCreated, this);
  }

  async activateAndRun(callback = () => {}, { muteNotificationsPopup = false } = {}) {
    const ownerWindow = this._tab.linkedBrowser.documentGlobal;
    const tabBrowser = ownerWindow.gBrowser;
    // Serialize all tab-switching commands per tabbed browser
    // to disallow concurrent tab switching.
    const result = globalTabAndWindowActivationChain.then(async () => {
      this._window.focus();
      if (tabBrowser.selectedTab !== this._tab) {
        const promise = helper.awaitEvent(ownerWindow, 'TabSwitchDone');
        tabBrowser.selectedTab = this._tab;
        await promise;
      }
      const notificationsPopup = muteNotificationsPopup ? this._linkedBrowser?.ownerDocument.getElementById('notification-popup') : null;
      notificationsPopup?.style.setProperty('pointer-events', 'none');
      try {
        await callback();
      } finally {
        notificationsPopup?.style.removeProperty('pointer-events');
      }
    });
    globalTabAndWindowActivationChain = result.catch(error => { /* swallow errors to keep chain running */ });
    return result;
  }

  frameIdToBrowsingContext(frameId) {
    return helper.collectAllBrowsingContexts(this._linkedBrowser.browsingContext).find(bc => helper.browsingContextToFrameId(bc) === frameId);
  }

  nextActorSequenceNumber() {
    return ++this._actorSequenceNumber;
  }

  setActor(actor, rootId = actor.browsingContext.id) {
    // Every actor that comes (a new process, a document restored from the back-forward cache, a new top after a
    // change of browsing context group) finds the setting in place: see updateJavaScriptDisabled.
    this.updateJavaScriptDisabled();
    actor.actorName = `browser::page[${this._targetId}]/${actor.browsingContext.browserId}/${actor.browsingContext.id}/${this.nextActorSequenceNumber()}`;
    const isMain = !actor.browsingContext.parent;
    this._contentMux.onRootAdded(rootId, actor.browsingContext);
    this._contentMux.bindRoot(rootId, actor, {isMain});
  }

  removeActor(actor, rootId) {
    // Note: the order between setActor and removeActor is non-deterministic.
    // Therefore we check that we are still bound to the actor that is being removed. The root comes from
    // the registry: the actor may not be readable any more.
    const endpoint = this._contentMux.unbindRoot(rootId, actor);
    if (!endpoint)
      return;
    // ⛔ THE CHANNEL GOES QUIET HERE AND MIGHT NEVER SPEAK AGAIN. `resetTransport`
    // keeps every pending request in the hope that a new actor binds and
    // replays it; measured 2026-09-19, a reload during a drag removed this
    // actor and NO replacement ever arrived. Whoever holds a request that only
    // the old document could answer has to hear about it, so this is emitted
    // whether or not a replacement follows. [B216]
    this.emit(PageTarget.Events.ContentDetached, {rootId: endpoint.rootId, isMain: endpoint.isMain});
  }

  dialog(dialogId) {
    return this._dialogs.get(dialogId);
  }

  dialogs() {
    return [...this._dialogs.values()];
  }

  async windowReady() {
    await waitForWindowReady(this._window);
  }

  linkedBrowser() {
    return this._linkedBrowser;
  }

  browserContext() {
    return this._browserContext;
  }

  updateOverridesForBrowsingContext(browsingContext = undefined) {
    this.updateTouchOverride(browsingContext);
    this.updateUserAgent(browsingContext);
    this.updateDPPXOverride(browsingContext);
    this.updateEmulatedMedia(browsingContext);
    this.updateColorSchemeOverride(browsingContext);
    this.updateForceOffline(browsingContext);
    this.updateCacheDisabled(browsingContext);
    this.updateLanguageOverride(browsingContext);
    this.updateTimezoneOverride(browsingContext);
    this.updateJavaScriptDisabled(browsingContext);
  }

  // Whether scripts run in the page is the BrowsingContext's AllowJavascript field, written here, by the parent,
  // on the page's top-level context. The field is parent-only, synchronized to every process, inherited by the
  // contexts born under it (an iframe of any process, a popup) and applied by its DidSet to the documents that
  // exist; a document that is created later computes it at birth, so no script of its first load runs. A browsing
  // context group change copies it to the new top (CanonicalBrowsingContext::ReplacedBy). It used to be written
  // by the content process on the WindowContext of each document, which is owner-or-parent-only: a process that
  // does not own the document of an iframe could not write it, and the write on a document that exists comes after
  // its first scripts.
  updateJavaScriptDisabled(browsingContext = undefined) {
    const context = browsingContext || this._linkedBrowser.browsingContext;
    const allowed = !this._browserContext.crossProcessCookie.settings.javaScriptDisabled;
    if (context.allowJavascript !== allowed)
      context.allowJavascript = allowed;
  }

  // The context's language list goes into the BrowsingContext's
  // LanguageOverride field, the one Firefox reads for navigator.language(s),
  // for the realm's default Intl locale (DidSet, and every realm created
  // later) and for the Accept-Language header (nsHttpHandler prepares it like
  // intl.accept_languages). BrowsingContext::Attach seeds that field from
  // juggler.locale.override, so a context that never asked keeps the
  // session's list untouched.
  //
  // This used to go to docShell.languageOverride, which nothing in Firefox
  // reads, and to docShell.overrideLocale on whatever realm was current when
  // the setting arrived, usually the initial about:blank: new_context(locale=
  // "de-DE") inside an it-IT session reported it-IT in navigator.languages,
  // Intl and the header.
  updateLanguageOverride(browsingContext = undefined) {
    if (this._browserContext.languageOverride === undefined)
      return;
    (browsingContext || this._linkedBrowser.browsingContext).languageOverride = this._browserContext.languageOverride;
  }

  // The context's zone goes into the BrowsingContext's TimezoneOverride
  // field, the same channel as the language above: its DidSet re-zones the
  // realms already alive in every process, and every realm, worker and
  // worklet created later takes the field at birth
  // (dom/base/StealthSessionDeclaration.h). BrowsingContext::Attach seeds the
  // field from juggler.timezone.override, so a context that never asked keeps
  // the session's zone.
  //
  // Until firefox-50 this went to docShell.overrideTimezone, from the content
  // process, on whatever realm was current when the setting arrived: the
  // realms created afterwards, the workers and the worklets took the
  // session's zone instead, and so did document.lastModified.
  updateTimezoneOverride(browsingContext = undefined) {
    if (this._browserContext.timezoneOverride === undefined)
      return;
    (browsingContext || this._linkedBrowser.browsingContext).timezoneOverride = this._browserContext.timezoneOverride;
  }

  updateForceOffline(browsingContext = undefined) {
    (browsingContext || this._linkedBrowser.browsingContext).forceOffline = this._browserContext.forceOffline;
  }

  setCacheDisabled(disabled) {
    this.disableCache = disabled;
    this.updateCacheDisabled();
  }

  updateCacheDisabled(browsingContext = this._linkedBrowser.browsingContext) {
    const enableFlags = Ci.nsIRequest.LOAD_NORMAL;
    const disableFlags = Ci.nsIRequest.LOAD_BYPASS_CACHE |
                  Ci.nsIRequest.INHIBIT_CACHING;

    browsingContext.defaultLoadFlags = (this._browserContext.disableCache || this.disableCache) ? disableFlags : enableFlags;
  }

  updateTouchOverride(browsingContext = undefined) {
    (browsingContext || this._linkedBrowser.browsingContext).touchEventsOverride = this._browserContext.touchOverride ? 'enabled' : 'none';
  }

  updateUserAgent(browsingContext = undefined) {
    (browsingContext || this._linkedBrowser.browsingContext).customUserAgent = this._browserContext.defaultUserAgent;
  }

  // Browser zoom is not ours: FullZoom.sys.mjs owns `fullZoom` (a site's
  // saved zoom included) and Gecko composes it with this override natively.
  // There used to be a `_zoom` here, always 1, that was multiplied in, written
  // over `fullZoom` at every new tab, and divided out of the viewport size -
  // which made set_viewport_size wait forever on a zoomed page.
  updateDPPXOverride(browsingContext = undefined) {
    browsingContext ||= this._linkedBrowser.browsingContext;
    browsingContext.overrideDPPX = this._browserContext.deviceScaleFactor || this._initialDPPX;
  }

  _updateModalDialogs() {
    const prompts = new Set(this._linkedBrowser.tabDialogBox.getContentDialogManager().dialogs.map(dialog => dialog.frameContentWindow.Dialog));
    for (const dialog of this._dialogs.values()) {
      if (!prompts.has(dialog.prompt())) {
        this._dialogs.delete(dialog.id());
      } else {
        prompts.delete(dialog.prompt());
      }
    }
    for (const prompt of prompts) {
      const dialog = Dialog.createIfSupported(prompt);
      if (!dialog)
        continue;
      this._dialogs.set(dialog.id(), dialog);
      this.emit(PageTarget.Events.DialogOpened, dialog);
    }
  }

  async updateViewportSize() {
    await waitForWindowReady(this._window);
    this.updateDPPXOverride();

    // Viewport size is defined by three arguments:
    // 1. default size. Could be explicit if set as part of `window.open` call, e.g.
    //   `window.open(url, title, 'width=400,height=400')`
    // 2. page viewport size
    // 3. browserContext viewport size
    //
    // The "default size" (1) is only respected when the page is opened.
    // Otherwise, explicitly set page viewport prevales over browser context
    // default viewport.
    const viewportSize = pinnedViewportSize(this._window.fullScreen, this._viewportSize, this._browserContext.defaultViewportSize);
    if (viewportSize) {
      const {width, height} = viewportSize;
      this._linkedBrowser.style.setProperty('width', width + 'px');
      this._linkedBrowser.style.setProperty('height', height + 'px');
      this._linkedBrowser.style.setProperty('box-sizing', 'content-box');
      this._linkedBrowser.closest('.browserStack').style.setProperty('overflow', 'auto');
      this._linkedBrowser.closest('.browserStack').style.setProperty('contain', 'size');
      this._linkedBrowser.closest('.browserStack').style.setProperty('scrollbar-width', 'none');
      // Stealth: skip RDM when stealth screen prefs are active.
      // inRDMPane=true causes a fingerprint-scanner page to hang on "Collecting Data..."
      // because RDM mode interferes with fingerprint collection.
      const stealthW = Preferences.get('zoom.stealth.screen.width', -1);
      if (stealthW <= 0) {
        this._linkedBrowser.browsingContext.inRDMPane = true;
      }

      const stackRect = this._linkedBrowser.closest('.browserStack').getBoundingClientRect();
      const toolbarTop = stackRect.y;
      this._window.resizeBy(width - this._window.innerWidth, height + toolbarTop - this._window.innerHeight);

      await this._awaitContentSize(width, height);
    } else {
      this._linkedBrowser.style.removeProperty('width');
      this._linkedBrowser.style.removeProperty('height');
      this._linkedBrowser.style.removeProperty('box-sizing');
      this._linkedBrowser.closest('.browserStack').style.removeProperty('overflow');
      this._linkedBrowser.closest('.browserStack').style.removeProperty('contain');
      this._linkedBrowser.closest('.browserStack').style.removeProperty('scrollbar-width');
      this._linkedBrowser.browsingContext.inRDMPane = false;

      const actualSize = this._linkedBrowser.getBoundingClientRect();
      await this._awaitContentSize(actualSize.width, actualSize.height);
    }
  }

  /**
   * Resolves once the content area has the size the <browser> was given.
   *
   * Compared in DEVICE pixels, which neither browser zoom nor the DPPX
   * override touch: the page's CSS size under a zoom is Gecko's own rounding
   * of app units, and a second copy of that arithmetic here would be a second
   * truth. The <browser>'s chrome CSS size times this window's scale is the
   * size the content's document viewer is laid out at.
   */
  async _awaitContentSize(width, height) {
    const scale = this._window.devicePixelRatio;
    await this._contentMux.connect('').send('awaitViewportDimensions', {
      deviceWidth: Math.round(width * scale),
      deviceHeight: Math.round(height * scale),
    });
  }

  setEmulatedMedia(mediumOverride) {
    this.mediumOverride = mediumOverride || '';
    this.updateEmulatedMedia();
  }

  updateEmulatedMedia(browsingContext = undefined) {
    (browsingContext || this._linkedBrowser.browsingContext).mediumOverride = this.mediumOverride;
  }

  setColorScheme(colorScheme) {
    this.colorScheme = fromProtocolColorScheme(colorScheme);
    this.updateColorSchemeOverride();
  }

  updateColorSchemeOverride(browsingContext = undefined) {
    (browsingContext || this._linkedBrowser.browsingContext).prefersColorSchemeOverride = this.colorScheme || this._browserContext.colorScheme || 'none';
  }

  async setInterceptFileChooserDialog(enabled) {
    this.crossProcessCookie.interceptFileChooserDialog = enabled;
    this._updateCrossProcessCookie();
    await this._contentMux.connect('').send('setInterceptFileChooserDialog', enabled).catch(e => {});
  }

  async setViewportSize(viewportSize) {
    this._viewportSize = viewportSize;
    await this.updateViewportSize();
  }

  close(runBeforeUnload = false) {
    this._gBrowser.removeTab(this._tab, {
      skipPermitUnload: !runBeforeUnload,
    });
  }

  /**
   * After its content process crashed and the client was told, the tab is
   * closed (the owner's decision, 2026-10-14, as in another anti-detect Firefox fork's reports).
   * Until firefox-48 the crash only disposed the target: the tab stayed in the
   * parent until the browser exited, 15-60 MB a crash with no bound, and no
   * command reached it any more, not page.close nor the context's close.
   *
   * Called after dispose(), so the TabClose listener finds no target and
   * nothing is disposed twice. Each page has a window of its own, so the tab
   * is usually the last of its window and the window closes with it
   * (browser.tabs.closeWindowWithLastTab). The one tab kept is the last tab of
   * the last window of a browser that does not survive it, a persistent
   * context: closing it would quit the browser under the client. It stays as
   * Firefox keeps every crashed tab, on its crash page.
   */
  closeCrashedTab() {
    if (!this._registry._survivesLastWindow && this._gBrowser.tabs.length === 1) {
      const windows = [...Services.wm.getEnumerator('navigator:browser')];
      if (windows.length === 1)
        return;
    }
    this._gBrowser.removeTab(this._tab, { skipPermitUnload: true });
  }

  /**
   * The process of an iframe crashed; the tab and its top document are alive.
   * The client is told the frame is gone (Page.frameDetached, the event it
   * already knows, for the crashed frame: the client removes the frames below
   * it with it) and no Page.crashed is sent: that event is the tab's.
   *
   * What the crashed process held besides the frame (its execution contexts
   * and workers) is the content multiplexer's to announce as destroyed, when
   * it exists; here the page keeps working, which is the part that was broken.
   */
  frameCrashed(browsingContext) {
    // The roots of the crashed frame and of the frames below it end: what waits for them is refused, what they held is
    // forgotten. Then the client is told.
    this._contentMux.disposeRootsOf(browsingContext);
    this.emit(PageTarget.Events.FrameCrashed, {
      frameId: helper.browsingContextToFrameId(browsingContext),
    });
  }

  channel() {
    return this._contentMux;
  }

  id() {
    return this._targetId;
  }

  info() {
    return {
      targetId: this.id(),
      type: 'page',
      browserContextId: this._browserContext.browserContextId,
      openerId: this._openerId,
    };
  }

  _onNavigated(aLocation) {
    this._url = aLocation.spec;
    this._browserContext.grantPermissionsToOrigin(this._url);
  }

  // ⛔ ONE KEY FOR THE LIFE OF THE PAGE, SET HERE AND DELETED IN dispose():
  // the browserId kept at construction, as the registry's maps do.
  // sharedData is copied whole into every content process at each flush, and
  // until firefox-48 nothing deleted a page's entry, so every page ever opened
  // stayed in it: 31 entries after 30 pages opened and closed, measured.
  _crossProcessCookieKey() {
    return helper.pageCookieKey(this._browserId);
  }

  _updateCrossProcessCookie() {
    Services.ppmm.sharedData.set(this._crossProcessCookieKey(), this.crossProcessCookie);
    Services.ppmm.sharedData.flush();
  }

  async ensurePermissions() {
    await this._contentMux.connect('').send('ensurePermissions', {}).catch(e => void e);
  }

  async setInitScripts(scripts) {
    this.crossProcessCookie.initScripts = scripts;
    this._updateCrossProcessCookie();
    await this.pushInitScripts();
  }

  async pushInitScripts() {
    await this._contentMux.connect('').send('setInitScripts', [...this._browserContext.crossProcessCookie.initScripts, ...this.crossProcessCookie.initScripts]).catch(e => void e);
  }

  async addBinding(worldName, name, script) {
    this.crossProcessCookie.bindings.push({ worldName, name, script });
    this._updateCrossProcessCookie();
    await this._contentMux.connect('').send('addBinding', { worldName, name, script }).catch(e => void e);
  }

  async applyContextSetting(name, value) {
    // The parent's, not the content's: the content processes are not told.
    if (name === 'javaScriptDisabled') {
      this.updateJavaScriptDisabled();
      return;
    }
    await this._contentMux.connect('').send('applyContextSetting', { name, value }).catch(e => void e);
  }

  /**
   * Starts streaming JPEG frames of this page's window.
   *
   * `fullWindow` keeps the browser chrome in the frame instead of cropping it
   * away. It is the reason this exists: the visible pointer is drawn in the
   * chrome document, so it is in these pixels and in no page screenshot.
   *
   * Nothing here reaches the content process. The capture runs in the parent,
   * on the operating system's window capture, and the page is not told.
   */
  async startScreencast({ width, height, quality, fullWindow, fps }) {
    if (!screencastService)
      throw new Error('ERROR: this build has no screencast service (juggler/screencast was not compiled)');
    if (this._screencastInfo)
      throw new Error('ERROR: a screencast is already running on this page');
    // On Mac the window may not be visible yet when the target is created and
    // its NSWindow.windowNumber can be -1, so wait until it is known good.
    await this.windowReady();
    if (width < 10 || width > 10000 || height < 10 || height > 10000)
      throw new Error('ERROR: invalid screencast size');

    const docShell = this._gBrowser.documentGlobal.docShell;
    // How much chrome sits above the content, in device pixels. The service
    // IGNORES it when fullWindow is set - it is computed either way so that
    // the two paths differ in one place only, which is the C++ that decides
    // whether to crop at all.
    const rect = this.linkedBrowser().getBoundingClientRect();
    const devicePixelRatio = this._window.devicePixelRatio;
    const offsetTop = Math.round(devicePixelRatio * rect.top);
    const viewport = this._viewportSize || this._browserContext.defaultViewportSize || { width: 0, height: 0 };
    // ⛔ The viewport clamp is for the CONTENT crop, where an oversized window
    // would otherwise show a strip of chrome. Applied to a full-window frame
    // it would cut the window down to the page again, undoing the flag.
    const clampWidth = fullWindow ? 0 : viewport.width;
    const clampHeight = fullWindow ? 0 : viewport.height;

    const self = this;
    const screencastClient = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIScreencastServiceClient]),
      screencastFrame(data, deviceWidth, deviceHeight) {
        if (self._screencastInfo)
          self.emit(PageTarget.Events.ScreencastFrame, { data, deviceWidth, deviceHeight, timestamp: Date.now() / 1000 });
      },
      screencastStopped() {
      },
    };
    const screencastId = screencastService.startScreencast(
        screencastClient, docShell, width, height, quality || 90,
        clampWidth, clampHeight, offsetTop, !!fullWindow, fps || 25);
    this._screencastInfo = { screencastId };
    return { screencastId };
  }

  screencastFrameAck({ screencastId }) {
    if (!this._screencastInfo || this._screencastInfo.screencastId !== screencastId)
      return;
    screencastService.screencastFrameAck(screencastId);
  }

  stopScreencast() {
    if (!this._screencastInfo)
      throw new Error('ERROR: no screencast is running on this page');
    const { screencastId } = this._screencastInfo;
    this._screencastInfo = undefined;
    screencastService.stopScreencast(screencastId);
  }

  ensureContextMenuClosed() {
    // Close context menu, if any, since it might capture mouse events on Linux
    // and prevent browser shutdown on MacOS.
    const doc = this._linkedBrowser.ownerDocument;
    const contextMenu = doc.getElementById('contentAreaContextMenu');
    if (contextMenu)
      contextMenu.hidePopup();
    const autocompletePopup = doc.getElementById('PopupAutoComplete');
    if (autocompletePopup)
      autocompletePopup.hidePopup();
    const selectPopup = doc.getElementById('ContentSelectDropdown')?.menupopup;
    if (selectPopup)
      selectPopup.hidePopup()
  }

  dispose() {
    this.ensureContextMenuClosed();
    this._disposed = true;
    // ⛔ Before anything else touches the window: the overlay holds
    // listeners on it and a pref observer, and a listener left on a window
    // that is going away is a leak nobody will look for here.
    if (this._stealthCursor) {
      StealthCursor.releaseWindow(this._window);
      this._stealthCursor = null;
    }
    // Same reasoning, one layer down: a live screencast owns a capture thread
    // reading a window that is about to stop existing.
    if (this._screencastInfo) {
      try {
        this.stopScreencast();
      } catch (e) {
        dump('failed to stop the screencast while disposing: ' + e.message + '\n');
      }
    }
    this._browserContext.pages.delete(this);
    this._registry._browserToTarget.delete(this._linkedBrowser);
    this._registry._browserIdToTarget.delete(this._browserId);
    // The page's roots: what waits for them is refused, and the registry forgets their actors.
    for (const rootId of this._contentMux.rootIds())
      this._contentMux.disposeRoot(rootId);
    const prefix = this._browserId + '/';
    for (const key of [...this._registry._rootActors.keys()]) {
      if (key.startsWith(prefix))
        this._registry._rootActors.delete(key);
    }
    if (Services.ppmm.sharedData.has(this._crossProcessCookieKey())) {
      Services.ppmm.sharedData.delete(this._crossProcessCookieKey());
      Services.ppmm.sharedData.flush();
    }
    try {
      helper.removeListeners(this._eventListeners);
    } catch (e) {
      // In some cases, removing listeners from this._linkedBrowser fails
      // because it is already half-destroyed.
      if (e)
        dump(e.message + '\n' + e.stack + '\n');
    }
    this._registry.emit(TargetRegistry.Events.TargetDestroyed, this);
  }
}

PageTarget.Events = {
  ContentDetached: Symbol('PageTarget.ContentDetached'),
  Crashed: Symbol('PageTarget.Crashed'),
  DialogOpened: Symbol('PageTarget.DialogOpened'),
  FrameCrashed: Symbol('PageTarget.FrameCrashed'),
  ScreencastFrame: Symbol('PageTarget.ScreencastFrame'),
};

function fromProtocolColorScheme(colorScheme) {
  if (colorScheme === 'light' || colorScheme === 'dark')
    return colorScheme;
  if (colorScheme === null || colorScheme === 'no-preference')
    return undefined;
  throw new Error('Unknown color scheme: ' + colorScheme);
}

// The zone as ICU spells it, or undefined when it is not an IANA zone. Intl
// matches a zone name without regard to case and answers with ICU's spelling,
// while ICU's own lookup, the one a realm's override goes through, is exact:
// "america/phoenix" passes Intl and fails there. An offset ("+05:00") is a
// valid Intl time zone but not a zone ICU opens by name, so it is refused too.
function ianaZone(timezoneId) {
  let zone;
  try {
    zone = new Intl.DateTimeFormat('en-US', { timeZone: timezoneId }).resolvedOptions().timeZone;
  } catch (e) {
    return undefined;
  }
  if (zone.startsWith('+') || zone.startsWith('-'))
    return undefined;
  return zone;
}

class BrowserContext {
  constructor(registry, browserContextId, removeOnDetach) {
    this._registry = registry;
    this.browserContextId = browserContextId;
    // Default context has userContextId === 0, but we pass undefined to many APIs just in case.
    this.userContextId = 0;
    if (browserContextId !== undefined) {
      const identity = ContextualIdentityService.create(IDENTITY_NAME + browserContextId);
      this.userContextId = identity.userContextId;
    }
    this._principals = [];
    // Maps origins to the permission lists.
    this._permissions = new Map();
    this._registry._browserContextIdToBrowserContext.set(this.browserContextId, this);
    this._registry._userContextIdToBrowserContext.set(this.userContextId, this);
    this._proxy = null;
    this.removeOnDetach = removeOnDetach;
    this.extraHTTPHeaders = undefined;
    this.httpCredentials = undefined;
    this.ignoreHTTPSErrors = undefined;
    this.downloadOptions = undefined;
    this.defaultViewportSize = undefined;
    this.deviceScaleFactor = undefined;
    this.defaultUserAgent = null;
    this.requestInterceptionEnabled = undefined;
    this.serviceWorkersBlocked = false;
    this._serviceWorkerListener = null;
    this._serviceWorkerUnregistrations = new Set();
    this._serviceWorkerError = null;
    this.touchOverride = false;
    this.forceOffline = false;
    this.disableCache = false;
    this.colorScheme = 'none';
    // undefined until Browser.setLocaleOverride: see PageTarget.updateLanguageOverride.
    this.languageOverride = undefined;
    // undefined until Browser.setTimezoneOverride: see setTimezoneOverride.
    this.timezoneOverride = undefined;
    this.invalidTimezoneId = undefined;
    this.crossProcessCookie = {
      initScripts: [],
      bindings: [],
      settings: {},
    };
    this.pages = new Set();
  }

  // Set here, deleted when the context is removed (destroy()): see the page's.
  _crossProcessCookieKey() {
    return helper.contextCookieKey(this.userContextId);
  }

  _updateCrossProcessCookie() {
    Services.ppmm.sharedData.set(this._crossProcessCookieKey(), this.crossProcessCookie);
    Services.ppmm.sharedData.flush();
  }

  setColorScheme(colorScheme) {
    this.colorScheme = fromProtocolColorScheme(colorScheme);
    for (const page of this.pages)
      page.updateColorSchemeOverride();
  }

  // The content processes of this context that are still alive: their remote type carries the container,
  // `web=^userContextId=<id>` (the id is followed by `&` or the end, so 6 is not 60).
  _processesOfContext() {
    const mine = new RegExp('\\^userContextId=' + this.userContextId + '(?:&|$)');
    return ChromeUtils.getAllDOMProcesses().filter(process => process.remoteType && mine.test(process.remoteType));
  }

  async _waitForProcessesToExit(timeoutMs) {
    const deadline = Date.now() + timeoutMs;
    while (this._processesOfContext().length && Date.now() < deadline)
      await new Promise(f => setTimeout(f, PROCESS_POLL_MS));
    return this._processesOfContext();
  }

  // A context ends with its pages; then its content processes are waited for, a short while, and the ones that
  // are still there are killed. A process that does not stop on its own stays for good: a service worker whose
  // script is `for (;;) {}` never answers the shutdown, no shutdown starts for its process, and nothing else ends
  // it (measured with MOZ_LOG: no MaybeBeginShutDown for the child), so each such context left a process alive.
  async destroy() {
    if (this.userContextId !== 0) {
      ContextualIdentityService.remove(this.userContextId);
      for (const page of this.pages)
        page.close();
      if (this.pages.size) {
        await new Promise(f => {
          const listener = helper.on(this._registry, TargetRegistry.Events.TargetDestroyed, () => {
            if (!this.pages.size) {
              helper.removeListeners([listener]);
              f();
            }
          });
        });
      }
      const leftovers = await this._waitForProcessesToExit(PROCESS_EXIT_WAIT_MS);
      if (leftovers.length) {
        const tools = Cc['@mozilla.org/processtools-service;1'].getService(Ci.nsIProcessToolsService);
        for (const process of leftovers) {
          try {
            tools.kill(process.osPid);
          } catch (e) {
            Components.utils.reportError(e);
          }
        }
        await this._waitForProcessesToExit(PROCESS_KILL_WAIT_MS);
      }
    }
    // Before the maps forget it: a request of this context still in flight asks the proxy filter after this point.
    GoneContexts.keep(this.userContextId, this._proxy);
    this._registry._browserContextIdToBrowserContext.delete(this.browserContextId);
    this._registry._userContextIdToBrowserContext.delete(this.userContextId);
    if (Services.ppmm.sharedData.has(this._crossProcessCookieKey())) {
      Services.ppmm.sharedData.delete(this._crossProcessCookieKey());
      Services.ppmm.sharedData.flush();
    }
    this._registry._updateProxiesWithSameAuthCacheAndDifferentCredentials();
    // The proxy of this context leaves the set; the last one out turns DoH off.
    this._registry._syncTrrPrefs();
    await this.setServiceWorkersBlocked(false);
  }

  async setServiceWorkersBlocked(blocked) {
    if (!blocked) {
      if (this._serviceWorkerListener) {
        this._serviceWorkerManager.removeListener(this._serviceWorkerListener);
        this._serviceWorkerListener = null;
      }
      this.serviceWorkersBlocked = false;
      this._serviceWorkerError = null;
      return;
    }
    if (!this.serviceWorkersBlocked) {
      this._serviceWorkerManager = Cc['@mozilla.org/serviceworkers/manager;1'].getService(Ci.nsIServiceWorkerManager);
      this.serviceWorkersBlocked = true;
      this._serviceWorkerListener = {
        QueryInterface: ChromeUtils.generateQI([Ci.nsIServiceWorkerManagerListener]),
        onRegister: info => this._unregisterServiceWorker(info),
        onUnregister() {},
        onQuotaUsageCheckFinish() {},
      };
      this._serviceWorkerManager.addListener(this._serviceWorkerListener);
      const registrations = this._serviceWorkerManager.getAllRegistrations();
      for (let i = 0; i < registrations.length; ++i)
        this._unregisterServiceWorker(registrations.queryElementAt(i, Ci.nsIServiceWorkerRegistrationInfo));
    }
    // The reply is the barrier before the client can open its first page.
    while (this._serviceWorkerUnregistrations.size)
      await Promise.all(this._serviceWorkerUnregistrations);
    if (this._serviceWorkerError)
      throw this._serviceWorkerError;
  }

  _unregisterServiceWorker(info) {
    if (info.principal.originAttributes.userContextId !== this.userContextId)
      return;
    const task = new Promise((resolve, reject) => {
      // unregister alone lets an active worker live until its clients close.
      info.forceShutdown();
      this._serviceWorkerManager.unregister(info.principal, {
        QueryInterface: ChromeUtils.generateQI([Ci.nsIServiceWorkerUnregisterCallback]),
        unregisterSucceeded: () => resolve(),
        unregisterFailed: () => reject(new Error(`Failed to unregister service worker for ${info.scope}`)),
      }, info.scope);
    });
    this._serviceWorkerUnregistrations.add(task);
    task.then(() => this._serviceWorkerUnregistrations.delete(task), error => {
      this._serviceWorkerUnregistrations.delete(task);
      this._serviceWorkerError = error;
      Components.utils.reportError(error);
    });
  }

  setProxy(proxy) {
    // Before the auth cache is touched, and before anything is stored: see the
    // note on TargetRegistry.setBrowserProxy. A context proxy that cannot be
    // expressed has to refuse its command rather than leave the context
    // usable and unproxied.
    newProxyInfoFor(proxy);
    // Clear AuthCache.
    Services.obs.notifyObservers(null, "net:clear-active-logins");
    const previous = this._proxy;
    this._proxy = proxy;
    try {
      // In the same turn as the store: no channel is filtered between the two.
      // The names of this proxy are excluded from DoH, the other names go over it.
      this._registry._syncTrrPrefs();
    } catch (e) {
      // A proxy with no DoH behind it would leak the names to the host's resolver.
      this._proxy = previous;
      throw e;
    }
    this._registry._updateProxiesWithSameAuthCacheAndDifferentCredentials();
  }

  setIgnoreHTTPSErrors(ignoreHTTPSErrors) {
    if (this.ignoreHTTPSErrors === ignoreHTTPSErrors)
      return;
    this.ignoreHTTPSErrors = ignoreHTTPSErrors;
    const certOverrideService = Cc[
      "@mozilla.org/security/certoverride;1"
    ].getService(Ci.nsICertOverrideService);
    if (ignoreHTTPSErrors) {
      // STEALTHFOX: qui c'erano due Preferences.set GLOBALI - la lista HSTS
      // precaricata e il cert pinning - applicate solo in questo ramo e mai
      // ripristinate nell'altro. Un solo contesto che chiedeva di ignorare gli
      // errori di certificato lasciava tutto il browser senza HSTS e senza
      // pinning per il resto della sessione.
      // Il meccanismo vero della funzione e' la riga qui sotto, che e' PER
      // CONTESTO e simmetrica; quelle due erano un colpo di accetta in piu'.
      // Un browser senza la lista HSTS precaricata si comporta sul filo in modo
      // diverso da ogni Firefox vero, e la differenza la vede il SERVER: e' la
      // classe di divergenza che nessuna prova dal lato JavaScript trova.
      // E resta anche piu' corretto: ignorare gli errori di CERTIFICATO non e'
      // la stessa cosa che disattivare HSTS.
      certOverrideService.setDisableAllSecurityChecksAndLetAttackersInterceptMyDataForUserContext(this.userContextId, true);
    } else {
      certOverrideService.setDisableAllSecurityChecksAndLetAttackersInterceptMyDataForUserContext(this.userContextId, false);
    }
  }

  setDefaultUserAgent(userAgent) {
    this.defaultUserAgent = userAgent;
    for (const page of this.pages)
      page.updateUserAgent();
  }

  // A language list ("de-DE, de, en-US, en") or a single tag. Clearing goes
  // back to the session's list, the one BrowsingContext::Attach seeds every
  // top-level context with, not to an empty field: an empty LanguageOverride
  // would hand navigator.languages and Intl back to the build's app locale.
  setLanguageOverride(locale) {
    this.languageOverride = locale === undefined
      ? Services.prefs.getStringPref('juggler.locale.override', '')
      : locale;
    for (const page of this.pages)
      page.updateLanguageOverride();
  }

  // An IANA zone ("America/Phoenix"). Clearing goes back to the session's
  // zone, the one BrowsingContext::Attach seeds every top-level context with,
  // as setLanguageOverride does for the language.
  //
  // A zone ICU cannot open never reaches the field: SpiderMonkey resolves an
  // override it cannot open to the MACHINE's zone, in silence
  // (js/src/vm/DateTime.cpp, DateTimeInfo::timeZone). It is kept aside
  // instead, and the next Browser.newPage answers 'Failed to override
  // timezone', the message Playwright reports as "Invalid timezone ID".
  setTimezoneOverride(timezoneId) {
    if (timezoneId === undefined) {
      this.timezoneOverride = Services.prefs.getStringPref('juggler.timezone.override', '');
      this.invalidTimezoneId = undefined;
    } else {
      const zone = ianaZone(timezoneId);
      if (zone === undefined) {
        this.invalidTimezoneId = timezoneId;
        return;
      }
      this.timezoneOverride = zone;
      this.invalidTimezoneId = undefined;
    }
    for (const page of this.pages)
      page.updateTimezoneOverride();
  }

  setTouchOverride(touchOverride) {
    this.touchOverride = touchOverride;
    for (const page of this.pages)
      page.updateTouchOverride();
  }

  setForceOffline(forceOffline) {
    this.forceOffline = forceOffline;
    for (const page of this.pages)
      page.updateForceOffline();
  }

  setCacheDisabled(disabled) {
    this.disableCache = disabled;
    for (const page of this.pages)
      page.updateCacheDisabled();
  }

  async setDefaultViewport(viewport) {
    this.defaultViewportSize = viewport ? viewport.viewportSize : undefined;
    this.deviceScaleFactor = viewport ? viewport.deviceScaleFactor : undefined;
    await Promise.all(Array.from(this.pages).map(page => page.updateViewportSize()));
  }

  async setInitScripts(scripts) {
    this.crossProcessCookie.initScripts = scripts;
    this._updateCrossProcessCookie();
    await Promise.all(Array.from(this.pages).map(page => page.pushInitScripts()));
  }

  async addBinding(worldName, name, script) {
    this.crossProcessCookie.bindings.push({ worldName, name, script });
    this._updateCrossProcessCookie();
    await Promise.all(Array.from(this.pages).map(page => page.addBinding(worldName, name, script)));
  }

  async applySetting(name, value) {
    this.crossProcessCookie.settings[name] = value;
    this._updateCrossProcessCookie();
    await Promise.all(Array.from(this.pages).map(page => page.applyContextSetting(name, value)));
  }

  async grantPermissions(origin, permissions) {
    this._permissions.set(origin, permissions);
    const promises = [];
    for (const page of this.pages) {
      if (origin === '*' || page._url.startsWith(origin)) {
        this.grantPermissionsToOrigin(page._url);
        promises.push(page.ensurePermissions());
      }
    }
    await Promise.all(promises);
  }

  resetPermissions() {
    for (const principal of this._principals) {
      for (const permission of ALL_PERMISSIONS)
        Services.perms.removeFromPrincipal(principal, permission);
    }
    this._principals = [];
    this._permissions.clear();
  }

  grantPermissionsToOrigin(url) {
    let origin = Array.from(this._permissions.keys()).find(key => url.startsWith(key));
    if (!origin)
      origin = '*';

    const permissions = this._permissions.get(origin);
    if (!permissions)
      return;

    const attrs = { userContextId: this.userContextId || undefined };
    const principal = Services.scriptSecurityManager.createContentPrincipal(NetUtil.newURI(url), attrs);
    this._principals.push(principal);
    for (const permission of ALL_PERMISSIONS) {
      const action = permissions.includes(permission) ? Ci.nsIPermissionManager.ALLOW_ACTION : Ci.nsIPermissionManager.DENY_ACTION;
      Services.perms.addFromPrincipal(principal, permission, action, Ci.nsIPermissionManager.EXPIRE_NEVER, 0 /* expireTime */);
    }
  }

  // Each cookie as Firefox stores it, or the command fails naming the ones
  // Firefox refused and why.
  // - 'None' is SameSite=None, as a site's header sets it, and Firefox refuses
  //   it without Secure (network.cookie.sameSite.noneRequiresSecure). It was
  //   stored as no attribute at all.
  // - A session cookie (expires -1 or none) gets an expiry that does not
  //   restrict it: Firefox honours the expiry of a session cookie too, and -1
  //   seconds was 1969, so the cookie was gone before anyone read it.
  // - A refused cookie is not stored and Services.cookies.add still answers
  //   NS_OK, with the reason in the validation it returns: it was dropped.
  setCookies(cookies) {
    const protocolToSameSite = {
      [undefined]: Ci.nsICookie.SAMESITE_UNSET,
      'None': Ci.nsICookie.SAMESITE_NONE,
      'Lax': Ci.nsICookie.SAMESITE_LAX,
      'Strict': Ci.nsICookie.SAMESITE_STRICT,
    };
    const refused = [];
    for (const cookie of cookies) {
      const uri = cookie.url ? NetUtil.newURI(cookie.url) : null;
      let domain = cookie.domain;
      if (!domain) {
        if (!uri)
          throw new Error('At least one of the url and domain needs to be specified');
        domain = uri.host;
      }
      let path = cookie.path;
      if (!path)
        path = uri ? dirPath(uri.filePath) : '/';
      let secure = false;
      if (cookie.secure !== undefined)
        secure = cookie.secure;
      else if (uri && uri.scheme === 'https')
        secure = true;
      const isSession = cookie.expires === undefined || cookie.expires === -1;
      const validation = Services.cookies.add(
        domain,
        path,
        cookie.name,
        cookie.value,
        secure,
        cookie.httpOnly || false,
        isSession,
        isSession ? Date.now() + HUNDRED_YEARS : cookie.expires * 1000,
        { userContextId: this.userContextId || undefined } /* originAttributes */,
        protocolToSameSite[cookie.sameSite],
        Ci.nsICookie.SCHEME_UNSET
      );
      if (validation && validation.result !== Ci.nsICookieValidation.eOK)
        refused.push(`"${cookie.name}" (${validationName(validation.result)})`);
    }
    if (refused.length)
      throw new Error(`Firefox refused the cookie${refused.length > 1 ? 's' : ''} ${refused.join(', ')}; the others are set`);
  }

  clearCookies() {
    Services.cookies.removeCookiesWithOriginAttributes(JSON.stringify({ userContextId: this.userContextId || undefined }));
  }

  getCookies() {
    const result = [];
    const sameSiteToProtocol = {
      [Ci.nsICookie.SAMESITE_UNSET]: 'None',
      [Ci.nsICookie.SAMESITE_NONE]: 'None',
      [Ci.nsICookie.SAMESITE_LAX]: 'Lax',
      [Ci.nsICookie.SAMESITE_STRICT]: 'Strict',
    };
    for (let cookie of Services.cookies.cookies) {
      if (cookie.originAttributes.userContextId !== this.userContextId)
        continue;
      if (cookie.host === 'addons.mozilla.org')
        continue;
      result.push({
        name: cookie.name,
        value: cookie.value,
        domain: cookie.host,
        path: cookie.path,
        expires: cookie.isSession ? -1 : cookie.expiry / 1000,
        size: cookie.name.length + cookie.value.length,
        httpOnly: cookie.isHttpOnly,
        secure: cookie.isSecure,
        session: cookie.isSession,
        sameSite: sameSiteToProtocol[cookie.sameSite],
      });
    }
    return result;
  }
}

class Dialog {
  static createIfSupported(prompt) {
    const type = prompt.args.promptType;
    switch (type) {
      case 'alert':
      case 'alertCheck':
        return new Dialog(prompt, 'alert');
      case 'prompt':
        return new Dialog(prompt, 'prompt');
      case 'confirm':
      case 'confirmCheck':
        return new Dialog(prompt, 'confirm');
      case 'confirmEx':
        return new Dialog(prompt, 'beforeunload');
      default:
        return null;
    };
  }

  constructor(prompt, type) {
    this._id = helper.generateId();
    this._type = type;
    this._prompt = prompt;
  }

  id() {
    return this._id;
  }

  message() {
    return this._prompt.ui.infoBody.textContent;
  }

  type() {
    return this._type;
  }

  prompt() {
    return this._prompt;
  }

  dismiss() {
    if (this._prompt.ui.button1)
      this._prompt.ui.button1.click();
    else
      this._prompt.ui.button0.click();
  }

  defaultValue() {
    return this._prompt.ui.loginTextbox.value;
  }

  accept(promptValue) {
    if (typeof promptValue === 'string' && this._type === 'prompt')
      this._prompt.ui.loginTextbox.value = promptValue;
    this._prompt.ui.button0.click();
  }
}


function dirPath(path) {
  return path.substring(0, path.lastIndexOf('/') + 1);
}

async function waitForWindowReady(window) {
  if (window.document.readyState === 'loading' || window.document.readyState === 'uninitialized') {
    await helper.awaitEvent(window, 'DOMContentLoaded');
  }
  if (window.delayedStartupPromise) {
    await window.delayedStartupPromise;
  } else {
    await new Promise((resolve => {
      Services.obs.addObserver(function observer(aSubject, aTopic) {
        if (window == aSubject) {
          Services.obs.removeObserver(observer, aTopic);
          resolve();
        }
      }, "browser-delayed-startup-finished");
    }));
  }
  if (window.document.readyState !== 'complete') {
    await helper.awaitEvent(window, 'load');
  }
}

TargetRegistry.Events = {
  TargetCreated: Symbol('TargetRegistry.Events.TargetCreated'),
  TargetDestroyed: Symbol('TargetRegistry.Events.TargetDestroyed'),
  DownloadCreated: Symbol('TargetRegistry.Events.DownloadCreated'),
  DownloadFinished: Symbol('TargetRegistry.Events.DownloadFinished'),
};
