/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {Helper} = ChromeUtils.importESModule('chrome://juggler/content/Helper.js');
const { ChannelEventSinkFactory } = ChromeUtils.importESModule("chrome://juggler/content/ChannelEventSink.sys.mjs");


const Cc = Components.classes;
const Ci = Components.interfaces;
const Cu = Components.utils;
const Cr = Components.results;
const Cm = Components.manager;
const CC = Components.Constructor;
const helper = new Helper();

const UINT32_MAX = Math.pow(2, 32)-1;

const pageNetworkSymbol = Symbol('PageNetwork');

export class PageNetwork {
  static forPageTarget(target) {
    if (!target)
      return undefined;
    let result = target[pageNetworkSymbol];
    if (!result) {
      result = new PageNetwork(target);
      target[pageNetworkSymbol] = result;
    }
    return result;
  }

  constructor(target) {
    helper.decorateAsEventEmitter(this);
    this._target = target;
    this._extraHTTPHeaders = null;
    this._requestInterceptionEnabled = false;
    // This is requestId => NetworkRequest map, only contains requests that are
    // awaiting interception action (abort, resume, fulfill) over the protocol.
    this._interceptedRequests = new Map();
  }

  setExtraHTTPHeaders(headers) {
    this._extraHTTPHeaders = headers;
  }

  combinedExtraHTTPHeaders() {
    return [
      ...(this._target.browserContext().extraHTTPHeaders || []),
      ...(this._extraHTTPHeaders || []),
    ];
  }

  enableRequestInterception() {
    this._requestInterceptionEnabled = true;
  }

  disableRequestInterception() {
    this._requestInterceptionEnabled = false;
    for (const intercepted of this._interceptedRequests.values())
      intercepted.resume();
    this._interceptedRequests.clear();
  }

  resumeInterceptedRequest(requestId, url, method, headers, postData) {
    this._takeIntercepted(requestId).resume(url, method, headers, postData);
  }

  fulfillInterceptedRequest(requestId, status, statusText, headers, base64body) {
    this._takeIntercepted(requestId).fulfill(status, statusText, headers, base64body);
  }

  abortInterceptedRequest(requestId, errorCode) {
    this._takeIntercepted(requestId).abort(errorCode);
  }

  _takeIntercepted(requestId) {
    const intercepted = this._interceptedRequests.get(requestId);
    if (!intercepted)
      throw new Error(`Cannot find request "${requestId}"`);
    this._interceptedRequests.delete(requestId);
    return intercepted;
  }
}

class NetworkRequest {
  constructor(networkObserver, httpChannel, redirectedFrom) {
    this._networkObserver = networkObserver;
    this.httpChannel = httpChannel;

    const loadInfo = this.httpChannel.loadInfo;
    const browsingContext = loadInfo?.frameBrowsingContext || loadInfo?.workerAssociatedBrowsingContext || loadInfo?.browsingContext;

    this._frameId = helper.browsingContextToFrameId(browsingContext);

    this.requestId = httpChannel.channelId + '';
    this.navigationId = httpChannel.isMainDocumentChannel && loadInfo ? helper.toProtocolNavigationId(loadInfo.jugglerLoadIdentifier) : undefined;

    this._redirectedIndex = 0;
    if (redirectedFrom) {
      this.redirectedFromId = redirectedFrom.requestId;
      this._redirectedIndex = redirectedFrom._redirectedIndex + 1;
      this.requestId = this.requestId + '-redirect' + this._redirectedIndex;
      this.navigationId = redirectedFrom.navigationId;
      // Finish previous request now. Since we inherit the listener, we could in theory
      // use onStopRequest, but that will only happen after the last redirect has finished.
      redirectedFrom._sendOnRequestFinished();
    }
    // In case of proxy auth, we get two requests with the same channel:
    // - one is pre-auth
    // - second is with auth header.
    //
    // In this case, we create this NetworkRequest object with a `redirectedFrom`
    // object, and they both share the same httpChannel.
    //
    // Since we want to maintain _channelToRequest map without clashes,
    // we must call `_sendOnRequestFinished` **before** we update it with a new object
    // here.
    if (this._networkObserver._channelToRequest.has(this.httpChannel))
      throw new Error(`Internal Error: invariant is broken for _channelToRequest map`);
    this._networkObserver._channelToRequest.set(this.httpChannel, this);

    if (redirectedFrom) {
      this._pageNetwork = redirectedFrom._pageNetwork;
    } else if (browsingContext) {
      const target = this._networkObserver._targetRegistry.targetForBrowserId(browsingContext.browserId);
      this._pageNetwork = PageNetwork.forPageTarget(target);
    }
    this._shouldYieldInterceptionToServiceWorker = false;
    this._expectingResumedRequest = undefined;  // { method, headers, postData }
    this._overriddenHeadersForRedirect = redirectedFrom?._overriddenHeadersForRedirect;
    this._sentOnRequest = false;
    this._sentOnResponse = false;
    this._fulfilled = false;

    if (this._overriddenHeadersForRedirect)
      overrideRequestHeaders(httpChannel, this._overriddenHeadersForRedirect);
    else if (this._pageNetwork)
      appendExtraHTTPHeaders(httpChannel, this._pageNetwork.combinedExtraHTTPHeaders());

    httpChannel.QueryInterface(Ci.nsITraceableChannel);
    this._originalListener = httpChannel.setNewListener(this);
    if (redirectedFrom) {
      // Listener is inherited for regular redirects, so we'd like to avoid
      // calling into previous NetworkRequest.
      this._originalListener = redirectedFrom._originalListener;
    }

    this._previousCallbacks = httpChannel.notificationCallbacks;
    httpChannel.notificationCallbacks = this;

    this.QueryInterface = ChromeUtils.generateQI([
      Ci.nsIAuthPrompt2,
      Ci.nsIAuthPromptProvider,
      Ci.nsIInterfaceRequestor,
      Ci.nsINetworkInterceptController,
      Ci.nsIStreamListener,
    ]);

    if (this.redirectedFromId) {
      // Redirects are not interceptable.
      this._sendOnRequest(false);
    }
  }

  // Public interception API.
  resume(url, method, headers, postData) {
    this._expectingResumedRequest = { method, headers, postData };
    const newUri = url ? Services.io.newURI(url) : null;
    this._interceptedChannel.resetInterceptionWithURI(newUri);
    this._interceptedChannel = undefined;
  }

  // Public interception API.
  abort(errorCode) {
    const error = errorMap[errorCode] || Cr.NS_ERROR_FAILURE;
    this._interceptedChannel.cancelInterception(error);
    this._interceptedChannel = undefined;
  }

  // Public interception API.
  fulfill(status, statusText, headers, base64body) {
    this._fulfilled = true;
    this._interceptedChannel.synthesizeStatus(status, statusText);
    for (const header of headers) {
      this._interceptedChannel.synthesizeHeader(header.name, header.value);
      if (header.name.toLowerCase() === 'set-cookie') {
        Services.cookies.QueryInterface(Ci.nsICookieService);
        for (const cookieString of header.value.split('\n'))
          Services.cookies.setCookieStringFromHttp(this.httpChannel.URI, cookieString, this.httpChannel);
      }
    }
    const synthesized = Cc["@mozilla.org/io/string-input-stream;1"].createInstance(Ci.nsIStringInputStream);
    if (base64body)
      synthesized.setByteStringData(atob(base64body));
    this._interceptedChannel.startSynthesizedResponse(synthesized, null, null, '', false);
    this._interceptedChannel.finishSynthesizedResponse();
    this._interceptedChannel = undefined;
  }

  // Instrumentation called by NetworkObserver.
  _onInternalRedirect(newChannel) {
    // Intercepted requests produce "internal redirects" - this is both for our own
    // interception and service workers.
    // An internal redirect does not necessarily have the same channelId,
    // but inherits notificationCallbacks and the listener,
    // and should be used instead of an old channel.
    this._networkObserver._channelToRequest.delete(this.httpChannel);
    this.httpChannel = newChannel;
    this._networkObserver._channelToRequest.set(this.httpChannel, this);
  }

  // Instrumentation called by NetworkObserver.
  _onInternalRedirectReady() {
    // Resumed request is first internally redirected to a new request,
    // and then the new request is ready to be updated.
    if (!this._expectingResumedRequest)
      return;
    const { method, headers, postData } = this._expectingResumedRequest;
    this._overriddenHeadersForRedirect = headers;
    this._expectingResumedRequest = undefined;

    if (headers)
      overrideRequestHeaders(this.httpChannel, headers);
    else if (this._pageNetwork)
      appendExtraHTTPHeaders(this.httpChannel, this._pageNetwork.combinedExtraHTTPHeaders());
    if (method)
      this.httpChannel.requestMethod = method;
    if (postData !== undefined)
      setPostData(this.httpChannel, postData, headers);
  }

  // nsIInterfaceRequestor
  getInterface(iid) {
    if (iid.equals(Ci.nsIAuthPrompt2) || iid.equals(Ci.nsIAuthPromptProvider) || iid.equals(Ci.nsINetworkInterceptController))
      return this;
    if (iid.equals(Ci.nsIAuthPrompt))  // Block nsIAuthPrompt - we want nsIAuthPrompt2 to be used instead.
      throw Cr.NS_ERROR_NO_INTERFACE;
    if (this._previousCallbacks)
      return this._previousCallbacks.getInterface(iid);
    throw Cr.NS_ERROR_NO_INTERFACE;
  }

  // nsIAuthPromptProvider
  getAuthPrompt(aPromptReason, iid) {
    return this;
  }

  // nsIAuthPrompt2
  asyncPromptAuth(aChannel, aCallback, aContext, level, authInfo) {
    let canceled = false;
    Promise.resolve().then(() => {
      if (canceled)
        return;
      const hasAuth = this.promptAuth(aChannel, level, authInfo);
      if (hasAuth)
        aCallback.onAuthAvailable(aContext, authInfo);
      else
        aCallback.onAuthCancelled(aContext, true);
    });
    return {
      QueryInterface: ChromeUtils.generateQI([Ci.nsICancelable]),
      cancel: () => {
        aCallback.onAuthCancelled(aContext, false);
        canceled = true;
      }
    };
  }

  // nsIAuthPrompt2
  promptAuth(aChannel, level, authInfo) {
    if (authInfo.flags & Ci.nsIAuthInformation.PREVIOUS_FAILED)
      return false;
    const pageNetwork = this._pageNetwork;
    if (!pageNetwork)
      return false;
    let credentials = null;
    if (authInfo.flags & Ci.nsIAuthInformation.AUTH_PROXY) {
      const proxy = this._networkObserver._targetRegistry.getProxyInfo(aChannel);
      credentials = proxy ? {username: proxy.username, password: proxy.password} : null;
    } else {
      credentials = pageNetwork._target.browserContext().httpCredentials;
    }
    if (!credentials)
      return false;
    const origin = aChannel.URI.scheme + '://' + aChannel.URI.hostPort;
    if (credentials.origin && origin.toLowerCase() !== credentials.origin.toLowerCase())
      return false;
    authInfo.username = credentials.username;
    authInfo.password = credentials.password;
    // This will produce a new request with respective auth header set.
    // It will have the same id as ours. We expect it to arrive as new request and
    // will treat it as our own redirect.
    this._networkObserver._expectRedirect(this.httpChannel.channelId + '', this);
    return true;
  }

  // nsINetworkInterceptController
  shouldPrepareForIntercept(aURI, channel) {
    const interceptController = this._fallThroughInterceptController();
    if (interceptController && interceptController.shouldPrepareForIntercept(aURI, channel)) {
      // We assume that interceptController is a service worker if there is one,
      // and yield interception to it.
      this._shouldYieldInterceptionToServiceWorker = true;
      return true;
    }

    if (channel !== this.httpChannel) {
      // Not our channel? Just in case this happens, don't do anything.
      return false;
    }

    const shouldIntercept = this._shouldIntercept();
    if (!shouldIntercept) {
      // We are not intercepting - ready to issue onRequest.
      this._sendOnRequest(false);
      return false;
    }

    return true;
  }

  // nsINetworkInterceptController
  channelIntercepted(intercepted) {
    // Yield to a service worker if determined so in shouldPrepareForIntercept().
    const serviceWorker = this._shouldYieldInterceptionToServiceWorker ? this._fallThroughInterceptController() : undefined;
    // Clear the flag to avoid an infinite loop. After service worker, we should intercept ourselves.
    this._shouldYieldInterceptionToServiceWorker = false;

    if (serviceWorker) {
      const interceptedChannel = intercepted.QueryInterface(Ci.nsIInterceptedChannel);
      // If service worker will not actually intercept the request, we want to be called again.
      interceptedChannel.interceptAfterServiceWorkerResets();
      serviceWorker.channelIntercepted(intercepted);
      return;
    }

    this._interceptedChannel = intercepted.QueryInterface(Ci.nsIInterceptedChannel);

    const pageNetwork = this._pageNetwork;
    if (!pageNetwork) {
      // Just in case we disabled instrumentation while intercepting, resume and forget.
      this.resume();
      return;
    }

    // Ok, so now we have intercepted the request, let's issue onRequest.
    // If interception has been disabled while we were intercepting, resume and forget.
    const interceptionEnabled = this._shouldIntercept();
    this._sendOnRequest(!!interceptionEnabled);
    if (interceptionEnabled)
      pageNetwork._interceptedRequests.set(this.requestId, this);
    else
      this.resume();
  }

  // nsIStreamListener
  onDataAvailable(aRequest, aInputStream, aOffset, aCount) {
    // Turns out webcompat shims might redirect to
    // SimpleChannel, so we get requests from a different channel.
    // See https://github.com/microsoft/playwright/issues/9418#issuecomment-944836244
    if (aRequest !== this.httpChannel)
      return;
    // For requests with internal redirect (e.g. intercepted by Service Worker),
    // we do not get onResponse normally, but we do get nsIStreamListener notifications.
    this._sendOnResponse(false);

    // Stealthfox 2026-08-24: response.body() e' rifiutato (Network.getResponseBody
    // non esiste piu'), quindi il corpo non lo legge mai nessuno - pass-through
    // diretto, senza copiarlo.
    try {
      this._originalListener.onDataAvailable(aRequest, aInputStream, aOffset, aCount);
    } catch (e) {
      // Be ready to original listener exceptions.
    }
  }

  // nsIStreamListener
  onStartRequest(aRequest) {
    // Turns out webcompat shims might redirect to
    // SimpleChannel, so we get requests from a different channel.
    // See https://github.com/microsoft/playwright/issues/9418#issuecomment-944836244
    if (aRequest !== this.httpChannel)
      return;
    this._sendOnRequest(false);
    try {
      this._originalListener.onStartRequest(aRequest);
    } catch (e) {
      // Be ready to original listener exceptions.
    }
  }

  // nsIStreamListener
  onStopRequest(aRequest, aStatusCode) {
    // Turns out webcompat shims might redirect to
    // SimpleChannel, so we get requests from a different channel.
    // See https://github.com/microsoft/playwright/issues/9418#issuecomment-944836244
    if (aRequest !== this.httpChannel)
      return;
    try {
      this._originalListener.onStopRequest(aRequest, aStatusCode);
    } catch (e) {
      // Be ready to original listener exceptions.
    }

    if (aStatusCode === 0) {
      // For requests with internal redirect (e.g. intercepted by Service Worker),
      // we do not get onResponse normally, but we do get nsIRequestObserver notifications.
      this._sendOnResponse(false);
      this._sendOnRequestFinished();
    } else {
      this._sendOnRequestFailed(aStatusCode);
    }
  }

  _shouldIntercept() {
    // We do not want to intercept any redirects, because we are not able
    // to intercept subresource redirects, and it's unreliable for main requests.
    if (this.redirectedFromId)
      return false;
    const pageNetwork = this._pageNetwork;
    if (!pageNetwork)
      return false;
    if (pageNetwork._requestInterceptionEnabled)
      return true;
    const browserContext = pageNetwork._target.browserContext();
    if (browserContext.requestInterceptionEnabled)
      return true;
    return false;
  }

  _fallThroughInterceptController() {
    try {
      return this._previousCallbacks?.getInterface(Ci.nsINetworkInterceptController);
    } catch (e) {
      return undefined;
    }
  }

  _sendOnRequest(isIntercepted) {
    if (this._sentOnRequest) {
      // We can come here twice because:
      // - Redirects call _sendOnRequest in the constructor and from inside interception.
      // - All other requests might call _sendOnRequest from onStartRequest and from inside interception.
      // - All requests call _sendOnRequest from _sendOnResponse to avoid responses without requests.
      return;
    }
    this._sentOnRequest = true;

    const pageNetwork = this._pageNetwork;
    if (!pageNetwork)
      return;
    const loadInfo = this.httpChannel.loadInfo;
    const causeType = loadInfo?.externalContentPolicyType || Ci.nsIContentPolicy.TYPE_OTHER;
    const internalCauseType = loadInfo?.internalContentPolicyType || Ci.nsIContentPolicy.TYPE_OTHER;
    pageNetwork.emit(PageNetwork.Events.Request, {
      url: this.httpChannel.URI.spec,
      frameId: this._frameId,
      isIntercepted,
      requestId: this.requestId,
      redirectedFrom: this.redirectedFromId,
      // Stealthfox 2026-08-24: mai letto - clonava lo stream e lo
      // base64-codificava per ogni POST, e nessuno lo chiede senza
      // page.route()/request.postData().
      postData: undefined,
      headers: requestHeaders(this.httpChannel),
      method: this.httpChannel.requestMethod,
      navigationId: this.navigationId,
      cause: causeTypeToString(causeType),
      internalCause: causeTypeToString(internalCauseType),
    }, this._frameId);
  }

  _sendOnResponse(fromCache, opt_statusCode, opt_statusText) {
    // For internal redirects, and perhaps something else that we lack test coverage for,
    // we can arrive here before onStartRequest has fired. Make sure we
    // notify about the request first.
    this._sendOnRequest(false);

    if (this._sentOnResponse) {
      // We can come here twice because of an internal redirect, for example:
      // - request was intercepted by a service worker;
      // - HSTS redirect;
      // - CORS preflight;
      // - who knows what else?
      return;
    }
    this._sentOnResponse = true;

    const pageNetwork = this._pageNetwork;
    if (!pageNetwork)
      return;

    this.httpChannel.QueryInterface(Ci.nsIHttpChannelInternal);
    this.httpChannel.QueryInterface(Ci.nsITimedChannel);
    const timing = {
      startTime: this.httpChannel.channelCreationTime,
      domainLookupStart: this.httpChannel.domainLookupStartTime,
      domainLookupEnd: this.httpChannel.domainLookupEndTime,
      connectStart: this.httpChannel.connectStartTime,
      secureConnectionStart: this.httpChannel.secureConnectionStartTime,
      connectEnd: this.httpChannel.connectEndTime,
      requestStart: this.httpChannel.requestStartTime,
      responseStart: this.httpChannel.responseStartTime,
    };

    const { status, statusText, headers } = responseHead(this.httpChannel, opt_statusCode, opt_statusText);
    if (redirectStatus.includes(status) && this._overriddenHeadersForRedirect)
      this._overriddenHeadersForRedirect = filterHeadersForRedirect(this._overriddenHeadersForRedirect, this.httpChannel.requestMethod, status);
    let remoteIPAddress = undefined;
    let remotePort = undefined;
    try {
      remoteIPAddress = this.httpChannel.remoteAddress;
      remotePort = this.httpChannel.remotePort;
    } catch (e) {
      // remoteAddress is not defined for cached requests.
    }

    pageNetwork.emit(PageNetwork.Events.Response, {
      requestId: this.requestId,
      // Stealthfox 2026-08-24: niente parsing del certificato per ogni
      // risposta - nessuno lo chiede senza response.securityDetails().
      securityDetails: null,
      fromCache,
      headers,
      remoteIPAddress,
      remotePort,
      status,
      statusText,
      timing,
      // Stealthfox 2026-08-24: sempre false, il rilevamento (l'observer
      // service-worker-synthesized-response) e' sparito.
      fromServiceWorker: false,
    }, this._frameId);
  }

  _sendOnRequestFailed(error) {
    const pageNetwork = this._pageNetwork;
    if (pageNetwork) {
      pageNetwork.emit(PageNetwork.Events.RequestFailed, {
        requestId: this.requestId,
        errorCode: helper.getNetworkErrorStatusText(error),
      }, this._frameId);
    }
    this._networkObserver._channelToRequest.delete(this.httpChannel);
  }

  _sendOnRequestFinished() {
    const pageNetwork = this._pageNetwork;
    // Undefined |responseEndTime| means there has been no response yet.
    // This happens when request interception API is used to redirect
    // the request to a different URL.
    // In this case, we should not emit "requestFinished" event.
    if (pageNetwork && this.httpChannel.responseEndTime !== undefined) {
      let protocolVersion = undefined;
      try {
        protocolVersion = this.httpChannel.protocolVersion;
      } catch (e) {
        // protocolVersion is unavailable in certain cases.
      };
      pageNetwork.emit(PageNetwork.Events.RequestFinished, {
        requestId: this.requestId,
        responseEndTime: this.httpChannel.responseEndTime,
        transferSize: this.httpChannel.transferSize,
        encodedBodySize: this.httpChannel.encodedBodySize,
        protocolVersion,
      }, this._frameId);
    }
    this._networkObserver._channelToRequest.delete(this.httpChannel);
  }
}

export class NetworkObserver {
  constructor(targetRegistry) {
    helper.decorateAsEventEmitter(this);

    this._targetRegistry = targetRegistry;

    this._channelToRequest = new Map();  // http channel -> network request
    this._expectedRedirect = new Map();  // expected redirect channel id (string) -> network request

    const protocolProxyService = Cc['@mozilla.org/network/protocol-proxy-service;1'].getService();
    this._channelProxyFilter = {
      QueryInterface: ChromeUtils.generateQI([Ci.nsIProtocolProxyChannelFilter]),
      applyFilter: (channel, defaultProxyInfo, proxyFilter) => {
        const proxy = this._targetRegistry.getProxyInfo(channel);
        if (!proxy) {
          proxyFilter.onProxyFilterResult(defaultProxyInfo);
          return;
        }
        if (this._targetRegistry.shouldBustHTTPAuthCacheForProxy(proxy))
          Services.obs.notifyObservers(null, "net:clear-active-logins");
        proxyFilter.onProxyFilterResult(protocolProxyService.newProxyInfo(
            proxy.type,
            proxy.host,
            proxy.port,
            '', /* aProxyAuthorizationHeader */
            '', /* aConnectionIsolationKey */
            Ci.nsIProxyInfo.TRANSPARENT_PROXY_RESOLVES_HOST, /* aFlags */
            UINT32_MAX, /* aFailoverTimeout */
            null, /* failover proxy */
        ));
      },
    };
    protocolProxyService.registerChannelFilter(this._channelProxyFilter, 0 /* position */);

    // Register self as ChannelEventSink to track redirects.
    ChannelEventSinkFactory.getService().registerCollector({
      _onChannelRedirect: this._onRedirect.bind(this),
    });

    this._eventListeners = [
      helper.addObserver(this._onRequest.bind(this), 'http-on-modify-request'),
      helper.addObserver(this._onResponse.bind(this, false /* fromCache */), 'http-on-examine-response'),
      helper.addObserver(this._onResponse.bind(this, true /* fromCache */), 'http-on-examine-cached-response'),
      helper.addObserver(this._onResponse.bind(this, true /* fromCache */), 'http-on-examine-merged-response'),
    ];
  }

  _expectRedirect(channelId, previous) {
    this._expectedRedirect.set(channelId, previous);
  }

  _onRedirect(oldChannel, newChannel, flags) {
    if (!(oldChannel instanceof Ci.nsIHttpChannel) || !(newChannel instanceof Ci.nsIHttpChannel))
      return;
    const oldHttpChannel = oldChannel.QueryInterface(Ci.nsIHttpChannel);
    const newHttpChannel = newChannel.QueryInterface(Ci.nsIHttpChannel);
    const request = this._channelToRequest.get(oldHttpChannel);
    if (flags & Ci.nsIChannelEventSink.REDIRECT_INTERNAL) {
      if (request)
        request._onInternalRedirect(newHttpChannel);
    } else if (flags & Ci.nsIChannelEventSink.REDIRECT_STS_UPGRADE) {
      if (request) {
        // This is an internal HSTS upgrade. The original http request is canceled, and a new
        // equivalent https request is sent. We forge 307 redirect to follow Chromium here:
        // https://source.chromium.org/chromium/chromium/src/+/main:net/url_request/url_request_http_job.cc;l=211
        request._sendOnResponse(false, 307, 'Temporary Redirect');
        this._expectRedirect(newHttpChannel.channelId + '', request);
      }
    } else {
      if (request)
        this._expectRedirect(newHttpChannel.channelId + '', request);
    }
  }

  _onRequest(channel, topic) {
    if (!(channel instanceof Ci.nsIHttpChannel))
      return;
    const httpChannel = channel.QueryInterface(Ci.nsIHttpChannel);
    const channelId = httpChannel.channelId + '';
    const redirectedFrom = this._expectedRedirect.get(channelId);
    if (redirectedFrom) {
      this._expectedRedirect.delete(channelId);
      new NetworkRequest(this, httpChannel, redirectedFrom);
    } else {
      const redirectedRequest = this._channelToRequest.get(httpChannel);
      if (redirectedRequest)
        redirectedRequest._onInternalRedirectReady();
      else
        new NetworkRequest(this, httpChannel);
    }
  }

  _onResponse(fromCache, httpChannel, topic) {
    const request = this._channelToRequest.get(httpChannel);
    if (request)
      request._sendOnResponse(fromCache);
  }

}

function requestHeaders(httpChannel) {
  const headers = [];
  httpChannel.visitRequestHeaders({
    visitHeader: (name, value) => headers.push({name, value}),
  });
  return headers;
}

function clearRequestHeaders(httpChannel) {
  for (const header of requestHeaders(httpChannel)) {
    // We cannot remove the "host" header.
    if (header.name.toLowerCase() === 'host')
      continue;
    // Keep the "cookie" header. If there is an override, it will be set anyway.
    // Otherwise, we may delete a cookie that was set for a redirect.
    if (header.name.toLowerCase() === 'cookie')
      continue;
    httpChannel.setRequestHeader(header.name, '', false /* merge */);
  }
}

function overrideRequestHeaders(httpChannel, headers) {
  clearRequestHeaders(httpChannel);
  appendExtraHTTPHeaders(httpChannel, headers);
}

const redirectStatus = [301, 302, 303, 307, 308];

function filterHeadersForRedirect(headers, requestMethod, status) {
    // HTTP-redirect fetch step 13 (https://fetch.spec.whatwg.org/#http-redirect-fetch)
  if ((status === 301 || status === 302) && requestMethod === 'POST' ||
      status === 303 && !['GET', 'HEAD'].includes(requestMethod)) {
    const requestBodyHeaders = ['content-encoding', 'content-language', 'content-length', 'content-location', 'content-type'];
    return headers.filter(header => !requestBodyHeaders.includes(header.name.toLowerCase()));
  }
  return headers;
}

function causeTypeToString(causeType) {
  for (let key in Ci.nsIContentPolicy) {
    if (Ci.nsIContentPolicy[key] === causeType)
      return key;
  }
  return 'TYPE_OTHER';
}

function appendExtraHTTPHeaders(httpChannel, headers) {
  if (!headers)
    return;
  for (const header of headers) {
    let value = header.value;
    // STEALTHFOX: Playwright sets the Accept-Language from the `locale` option as a single
    // primary tag ("en-US"), which diverges from navigator.languages (the desktop-default
    // 2-tag list ["en-US","en"]) - a fingerprint tell (browserscan 'language mismatch').
    //
    // This used to expand the single tag here, in JavaScript, appending a
    // hardcoded ";q=0.5" and calling it "the Firefox-native q-valued form". It
    // is not: stock Firefox 151 sends "en-US,en;q=0.9" (measured 2026-08-09),
    // and the 0.5 went out on every request of every session. The number was
    // copied from the stale doc block above PrepareAcceptLanguages in
    // nsHttpHandler.cpp, while the code below it forwards to
    // rust_prepare_accept_languages, which does 1.0/0.9/0.8.
    //
    // So the header is DECLARED now, by invisible_core, and read here verbatim
    // (engine rule 7: one source, no compiled copy, no arithmetic on this side).
    if (header.name.toLowerCase() === 'accept-language' && value) {
      const declared = Services.prefs.getStringPref('zoom.stealth.http.accept_language', '');
      if (declared)
        value = declared;
    }
    httpChannel.setRequestHeader(header.name, value, false /* merge */);
  }
}

function responseHead(httpChannel, opt_statusCode, opt_statusText) {
  const headers = [];
  let status = opt_statusCode || 0;
  let statusText = opt_statusText || '';
  try {
    status = httpChannel.responseStatus;
    statusText = httpChannel.responseStatusText;
    httpChannel.visitResponseHeaders({
      visitHeader: (name, value) => headers.push({name, value}),
    });
  } catch (e) {
    // Response headers, status and/or statusText are not available
    // when redirect did not actually hit the network.
  }
  return { status, statusText, headers };
}

function setPostData(httpChannel, postData, headers) {
  if (!(httpChannel instanceof Ci.nsIUploadChannel2))
    return;
  const synthesized = Cc["@mozilla.org/io/string-input-stream;1"].createInstance(Ci.nsIStringInputStream);
  const body = atob(postData);
  synthesized.setByteStringData(body);

  const overriddenHeader = (lowerCaseName) => {
    if (headers) {
      for (const header of headers) {
        if (header.name.toLowerCase() === lowerCaseName) {
          return header.value;
        }
      }
    }
    return undefined;
  }
  // Clear content-length, so that upload stream resets it.
  httpChannel.setRequestHeader('content-length', '', false /* merge */);
  let contentType = overriddenHeader('content-type');
  if (contentType === undefined) {
    try {
      contentType = httpChannel.getRequestHeader('content-type');
    } catch (e) {
      if (e.result == Cr.NS_ERROR_NOT_AVAILABLE)
        contentType =  'application/octet-stream';
      else
        throw e;
    }
  }
  httpChannel.explicitSetUploadStream(synthesized, contentType, -1, httpChannel.requestMethod, false);
}

const errorMap = {
  'aborted': Cr.NS_ERROR_ABORT,
  'accessdenied': Cr.NS_ERROR_PORT_ACCESS_NOT_ALLOWED,
  'addressunreachable': Cr.NS_ERROR_UNKNOWN_HOST,
  'blockedbyclient': Cr.NS_ERROR_FAILURE,
  'blockedbyresponse': Cr.NS_ERROR_FAILURE,
  'connectionaborted': Cr.NS_ERROR_NET_INTERRUPT,
  'connectionclosed': Cr.NS_ERROR_FAILURE,
  'connectionfailed': Cr.NS_ERROR_FAILURE,
  'connectionrefused': Cr.NS_ERROR_CONNECTION_REFUSED,
  'connectionreset': Cr.NS_ERROR_NET_RESET,
  'internetdisconnected': Cr.NS_ERROR_OFFLINE,
  'namenotresolved': Cr.NS_ERROR_UNKNOWN_HOST,
  'timedout': Cr.NS_ERROR_NET_TIMEOUT,
  'failed': Cr.NS_ERROR_FAILURE,
};

PageNetwork.Events = {
  Request: Symbol('PageNetwork.Events.Request'),
  Response: Symbol('PageNetwork.Events.Response'),
  RequestFinished: Symbol('PageNetwork.Events.RequestFinished'),
  RequestFailed: Symbol('PageNetwork.Events.RequestFailed'),
};

