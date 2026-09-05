/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

const {t} = ChromeUtils.importESModule('chrome://juggler/content/protocol/PrimitiveTypes.js');

// Protocol-specific types.
const browserTypes = {};

browserTypes.TargetInfo = {
  type: t.Enum(['page']),
  targetId: t.String,
  browserContextId: t.Optional(t.String),
  // PageId of parent tab, if any.
  openerId: t.Optional(t.String),
};

browserTypes.UserPreference = {
  name: t.String,
  value: t.Any,
};

browserTypes.CookieOptions = {
  name: t.String,
  value: t.String,
  url: t.Optional(t.String),
  domain: t.Optional(t.String),
  path: t.Optional(t.String),
  secure: t.Optional(t.Boolean),
  httpOnly: t.Optional(t.Boolean),
  sameSite: t.Optional(t.Enum(['Strict', 'Lax', 'None'])),
  expires: t.Optional(t.Number),
};

browserTypes.Cookie = {
  name: t.String,
  domain: t.String,
  path: t.String,
  value: t.String,
  expires: t.Number,
  size: t.Number,
  httpOnly: t.Boolean,
  secure: t.Boolean,
  session: t.Boolean,
  sameSite: t.Enum(['Strict', 'Lax', 'None']),
};

browserTypes.Geolocation = {
  latitude: t.Number,
  longitude: t.Number,
  accuracy: t.Optional(t.Number),
};

browserTypes.DownloadOptions = {
  behavior: t.Optional(t.Enum(['saveToDisk', 'cancel'])),
  downloadsDir: t.Optional(t.String),
};

const pageTypes = {};
pageTypes.DOMPoint = {
  x: t.Number,
  y: t.Number,
};

pageTypes.Rect = {
  x: t.Number,
  y: t.Number,
  width: t.Number,
  height: t.Number,
};

pageTypes.Size = {
  width: t.Number,
  height: t.Number,
};

pageTypes.Viewport = {
  viewportSize: pageTypes.Size,
  deviceScaleFactor: t.Optional(t.Number),
  // STEALTHFOX: Playwright 1.61+ sends isMobile and screenSize on
  // Browser.setDefaultViewport (Firefox 151 Juggler). The strict checkScheme
  // validator rejects any undeclared field, so declare both to stay
  // forward-compatible. Both are accepted and ignored: setDefaultViewport reads
  // only viewportSize + deviceScaleFactor, we are always desktop, and the screen
  // dimensions come from the fingerprint prefs - letting the client override them
  // would break the seed/screen consistency the profile guarantees.
  isMobile: t.Optional(t.Boolean),
  screenSize: t.Optional(t.Nullable(pageTypes.Size)),
};

pageTypes.DOMQuad = {
  p1: pageTypes.DOMPoint,
  p2: pageTypes.DOMPoint,
  p3: pageTypes.DOMPoint,
  p4: pageTypes.DOMPoint,
};

pageTypes.TouchPoint = {
  x: t.Number,
  y: t.Number,
  radiusX: t.Optional(t.Number),
  radiusY: t.Optional(t.Number),
  rotationAngle: t.Optional(t.Number),
  force: t.Optional(t.Number),
};

pageTypes.Clip = {
  x: t.Number,
  y: t.Number,
  width: t.Number,
  height: t.Number,
};

pageTypes.InitScript = {
  script: t.String,
  worldName: t.Optional(t.String),
};

const runtimeTypes = {};
runtimeTypes.RemoteObject = {
  type: t.Optional(t.Enum(['object', 'function', 'undefined', 'string', 'number', 'boolean', 'symbol', 'bigint'])),
  subtype: t.Optional(t.Enum(['array', 'null', 'node', 'regexp', 'date', 'map', 'set', 'weakmap', 'weakset', 'error', 'proxy', 'promise', 'typedarray'])),
  objectId: t.Optional(t.String),
  unserializableValue: t.Optional(t.Enum(['Infinity', '-Infinity', '-0', 'NaN'])),
  value: t.Any
};

runtimeTypes.ObjectProperty = {
  name: t.String,
  value: runtimeTypes.RemoteObject,
};

runtimeTypes.ScriptLocation = {
  columnNumber: t.Number,
  lineNumber: t.Number,
  url: t.String,
};

runtimeTypes.ExceptionDetails = {
  text: t.Optional(t.String),
  stack: t.Optional(t.String),
  value: t.Optional(t.Any),
};

runtimeTypes.CallFunctionArgument = {
  objectId: t.Optional(t.String),
  unserializableValue: t.Optional(t.Enum(['Infinity', '-Infinity', '-0', 'NaN'])),
  value: t.Any,
};

runtimeTypes.AuxData = {
  frameId: t.Optional(t.String),
  name: t.Optional(t.String),
};

const networkTypes = {};

networkTypes.HTTPHeader = {
  name: t.String,
  value: t.String,
};

networkTypes.HTTPCredentials = {
  username: t.String,
  password: t.String,
  origin: t.Optional(t.String),
};

networkTypes.SecurityDetails = {
  protocol: t.String,
  subjectName: t.String,
  issuer: t.String,
  validFrom: t.Number,
  validTo: t.Number,
};

networkTypes.ResourceTiming = {
  startTime: t.Number,
  domainLookupStart: t.Number,
  domainLookupEnd: t.Number,
  connectStart: t.Number,
  secureConnectionStart: t.Number,
  connectEnd: t.Number,
  requestStart: t.Number,
  responseStart: t.Number,
};

const Browser = {
  targets: ['browser'],

  types: browserTypes,

  events: {
    'attachedToTarget': {
      sessionId: t.String,
      targetInfo: browserTypes.TargetInfo,
    },
    'detachedFromTarget': {
      sessionId: t.String,
      targetId: t.String,
    },
    'downloadCreated': {
      uuid: t.String,
      browserContextId: t.Optional(t.String),
      pageTargetId: t.String,
      frameId: t.String,
      url: t.String,
      suggestedFileName: t.String,
    },
    'downloadFinished': {
      uuid: t.String,
      canceled: t.Optional(t.Boolean),
      error: t.Optional(t.String),
    },
  },

  methods: {
    'enable': {
      params: {
        attachToDefaultContext: t.Boolean,
        userPrefs: t.Optional(t.Array(browserTypes.UserPreference)),
      },
    },
    'createBrowserContext': {
      params: {
        removeOnDetach: t.Optional(t.Boolean),
      },
      returns: {
        browserContextId: t.String,
      },
    },
    'removeBrowserContext': {
      params: {
        browserContextId: t.String,
      },
    },
    'newPage': {
      params: {
        browserContextId: t.Optional(t.String),
      },
      returns: {
        targetId: t.String,
      }
    },
    'close': {},
    'getInfo': {
      returns: {
        userAgent: t.String,
        version: t.String,
      },
    },
    'setExtraHTTPHeaders': {
      params: {
        browserContextId: t.Optional(t.String),
        headers: t.Array(networkTypes.HTTPHeader),
      },
    },
    'clearCache': {},
    'setBrowserProxy': {
      params: {
        type: t.Enum(['http', 'https', 'socks', 'socks4']),
        bypass: t.Array(t.String),
        host: t.String,
        port: t.Number,
        username: t.Optional(t.String),
        password: t.Optional(t.String),
      },
    },
    'setContextProxy': {
      params: {
        browserContextId: t.Optional(t.String),
        type: t.Enum(['http', 'https', 'socks', 'socks4']),
        bypass: t.Array(t.String),
        host: t.String,
        port: t.Number,
        username: t.Optional(t.String),
        password: t.Optional(t.String),
      },
    },
    'setHTTPCredentials': {
      params: {
        browserContextId: t.Optional(t.String),
        credentials: t.Nullable(networkTypes.HTTPCredentials),
      },
    },
    'setCacheDisabled': {
      params: {
        browserContextId: t.Optional(t.String),
        cacheDisabled: t.Boolean,
      },
    },
    'setRequestInterception': {
      params: {
        browserContextId: t.Optional(t.String),
        enabled: t.Boolean,
      },
    },
    'setGeolocationOverride': {
      params: {
        browserContextId: t.Optional(t.String),
        geolocation: t.Nullable(browserTypes.Geolocation),
      }
    },
    'setUserAgentOverride': {
      params: {
        browserContextId: t.Optional(t.String),
        userAgent: t.Nullable(t.String),
      }
    },
    'setBypassCSP': {
      params: {
        browserContextId: t.Optional(t.String),
        bypassCSP: t.Nullable(t.Boolean),
      }
    },
    'setIgnoreHTTPSErrors': {
      params: {
        browserContextId: t.Optional(t.String),
        ignoreHTTPSErrors: t.Nullable(t.Boolean),
      }
    },
    'setJavaScriptDisabled': {
      params: {
        browserContextId: t.Optional(t.String),
        javaScriptDisabled: t.Boolean,
      }
    },
    'setLocaleOverride': {
      params: {
        browserContextId: t.Optional(t.String),
        locale: t.Nullable(t.String),
      }
    },
    'setTimezoneOverride': {
      params: {
        browserContextId: t.Optional(t.String),
        timezoneId: t.Nullable(t.String),
      }
    },
    'setDownloadOptions': {
      params: {
        browserContextId: t.Optional(t.String),
        downloadOptions: t.Nullable(browserTypes.DownloadOptions),
      }
    },
    'setTouchOverride': {
      params: {
        browserContextId: t.Optional(t.String),
        hasTouch: t.Nullable(t.Boolean),
      }
    },
    'setDefaultViewport': {
      params: {
        browserContextId: t.Optional(t.String),
        viewport: t.Nullable(pageTypes.Viewport),
      }
    },
    'setInitScripts': {
      params: {
        browserContextId: t.Optional(t.String),
        scripts: t.Array(pageTypes.InitScript),
      }
    },
    'addBinding': {
      params: {
        browserContextId: t.Optional(t.String),
        worldName: t.Optional(t.String),
        name: t.String,
        script: t.String,
      },
    },
    'grantPermissions': {
      params: {
        origin: t.String,
        browserContextId: t.Optional(t.String),
        permissions: t.Array(t.String),
      },
    },
    'resetPermissions': {
      params: {
        browserContextId: t.Optional(t.String),
      }
    },
    'setCookies': {
      params: {
        browserContextId: t.Optional(t.String),
        cookies: t.Array(browserTypes.CookieOptions),
      }
    },
    'clearCookies': {
      params: {
        browserContextId: t.Optional(t.String),
      }
    },
    'getCookies': {
      params: {
        browserContextId: t.Optional(t.String)
      },
      returns: {
        cookies: t.Array(browserTypes.Cookie),
      },
    },
    'setOnlineOverride': {
      params: {
        browserContextId: t.Optional(t.String),
        override: t.Nullable(t.Enum(['online', 'offline'])),
      }
    },
    'setColorScheme': {
      params: {
        browserContextId: t.Optional(t.String),
        colorScheme: t.Nullable(t.Enum(['dark', 'light', 'no-preference'])),
      },
    },
    // I tre qui sotto erano spariti insieme alla potatura di TargetRegistry.js
    // (28.4 di 20-our-patches.md documenta la conseguenza su
    // Page.setEmulatedMedia, non questa). Playwright upstream li manda a OGNI
    // creazione di contesto salvo un "no-override" esplicito, quindi senza la
    // dichiarazione un `new_page()` qualsiasi muore con "method ... is not
    // supported". Restano dichiarati; l'handler accetta solo il valore neutro.
    'setReducedMotion': {
      params: {
        browserContextId: t.Optional(t.String),
        reducedMotion: t.Nullable(t.Enum(['reduce', 'no-preference'])),
      },
    },
    'setForcedColors': {
      params: {
        browserContextId: t.Optional(t.String),
        forcedColors: t.Nullable(t.Enum(['active', 'none'])),
      },
    },
    'setContrast': {
      params: {
        browserContextId: t.Optional(t.String),
        contrast: t.Nullable(t.Enum(['less', 'more', 'custom', 'no-preference'])),
      },
    },
    'cancelDownload': {
      params: {
        uuid: t.Optional(t.String),
      }
    }
  },
};

const Heap = {
  targets: ['page'],
  types: {},
  events: {},
  methods: {
    'collectGarbage': {
      params: {},
    },
  },
};

const Network = {
  targets: ['page'],
  types: networkTypes,
  events: {
    'requestWillBeSent': {
      // frameId may be absent for redirected requests.
      frameId: t.Optional(t.String),
      requestId: t.String,
      // RequestID of redirected request.
      redirectedFrom: t.Optional(t.String),
      postData: t.Optional(t.String),
      headers: t.Array(networkTypes.HTTPHeader),
      isIntercepted: t.Boolean,
      url: t.String,
      method: t.String,
      navigationId: t.Optional(t.String),
      cause: t.String,
      internalCause: t.String,
    },
    'responseReceived': {
      securityDetails: t.Nullable(networkTypes.SecurityDetails),
      requestId: t.String,
      fromCache: t.Boolean,
      remoteIPAddress: t.Optional(t.String),
      remotePort: t.Optional(t.Number),
      status: t.Number,
      statusText: t.String,
      headers: t.Array(networkTypes.HTTPHeader),
      timing: networkTypes.ResourceTiming,
      fromServiceWorker: t.Boolean,
    },
    'requestFinished': {
      requestId: t.String,
      responseEndTime: t.Number,
      transferSize: t.Number,
      encodedBodySize: t.Number,
      protocolVersion: t.Optional(t.String),
    },
    'requestFailed': {
      requestId: t.String,
      errorCode: t.String,
    },
  },
  methods: {
    'setRequestInterception': {
      params: {
        enabled: t.Boolean,
      },
    },
    'setExtraHTTPHeaders': {
      params: {
        headers: t.Array(networkTypes.HTTPHeader),
      },
    },
    'abortInterceptedRequest': {
      params: {
        requestId: t.String,
        errorCode: t.String,
      },
    },
    'resumeInterceptedRequest': {
      params: {
        requestId: t.String,
        url: t.Optional(t.String),
        method: t.Optional(t.String),
        headers: t.Optional(t.Array(networkTypes.HTTPHeader)),
        postData: t.Optional(t.String),
      },
    },
    'fulfillInterceptedRequest': {
      params: {
        requestId: t.String,
        status: t.Number,
        statusText: t.String,
        headers: t.Array(networkTypes.HTTPHeader),
        base64body: t.Optional(t.String),  // base64-encoded
      },
    },
    // Stealthfox 2026-08-30: RIMESSO. Era stato tolto il 2026-08-24 portando
    // NetworkObserver all'osso, e la conseguenza l'ha segnalata un utente:
    // response.text() e response.body() sono una delle tre fonti - URL, header,
    // corpo - con cui si riconosce chi protegge un sito, e senza il corpo ne
    // restano due. Toglieva anche i corpi dai trace e dagli HAR, in silenzio.
    'getResponseBody': {
      params: {
        requestId: t.String,
      },
      returns: {
        base64body: t.String,
        evicted: t.Optional(t.Boolean),
      },
    },
  },
};

const Runtime = {
  targets: ['page'],
  types: runtimeTypes,
  events: {
    'executionContextCreated': {
      executionContextId: t.String,
      auxData: runtimeTypes.AuxData,
    },
    'executionContextDestroyed': {
      executionContextId: t.String,
    },
    'executionContextsCleared': {
    },
    'console': {
      executionContextId: t.String,
      args: t.Array(runtimeTypes.RemoteObject),
      type: t.String,
      location: runtimeTypes.ScriptLocation,
    },
  },
  methods: {
    'evaluate': {
      params: {
        // Pass frameId here.
        executionContextId: t.String,
        expression: t.String,
        returnByValue: t.Optional(t.Boolean),
      },

      returns: {
        result: t.Optional(runtimeTypes.RemoteObject),
        exceptionDetails: t.Optional(runtimeTypes.ExceptionDetails),
      }
    },
    'callFunction': {
      params: {
        // Pass frameId here.
        executionContextId: t.String,
        functionDeclaration: t.String,
        returnByValue: t.Optional(t.Boolean),
        args: t.Array(runtimeTypes.CallFunctionArgument),
      },

      returns: {
        result: t.Optional(runtimeTypes.RemoteObject),
        exceptionDetails: t.Optional(runtimeTypes.ExceptionDetails),
      }
    },
    'disposeObject': {
      params: {
        executionContextId: t.String,
        objectId: t.String,
      },
    },

    'getObjectProperties': {
      params: {
        executionContextId: t.String,
        objectId: t.String,
      },

      returns: {
        properties: t.Array(runtimeTypes.ObjectProperty),
      }
    },
  },
};

const Page = {
  targets: ['page'],

  types: pageTypes,
  events: {
    'ready': {
    },
    'crashed': {
    },
    'eventFired': {
      frameId: t.String,
      name: t.Enum(['load', 'DOMContentLoaded']),
    },
    'uncaughtError': {
      frameId: t.String,
      message: t.String,
      stack: t.String,
      location: runtimeTypes.ScriptLocation,
    },
    'frameAttached': {
      frameId: t.String,
      parentFrameId: t.Optional(t.String),
    },
    'frameDetached': {
      frameId: t.String,
    },
    'navigationStarted': {
      frameId: t.String,
      navigationId: t.String,
    },
    'navigationCommitted': {
      frameId: t.String,
      // |navigationId| can only be null in response to enable.
      navigationId: t.Optional(t.String),
      url: t.String,
      // frame.id or frame.name
      name: t.String,
    },
    'navigationAborted': {
      frameId: t.String,
      navigationId: t.String,
      errorText: t.String,
    },
    'sameDocumentNavigation': {
      frameId: t.String,
      url: t.String,
    },
    'dialogOpened': {
      dialogId: t.String,
      type: t.Enum(['prompt', 'alert', 'confirm', 'beforeunload']),
      message: t.String,
      defaultValue: t.Optional(t.String),
    },
    'bindingCalled': {
      executionContextId: t.String,
      name: t.String,
      payload: t.Any,
    },
    'linkClicked': {
      phase: t.Enum(['before', 'after']),
    },
    'fileChooserOpened': {
      executionContextId: t.String,
      element: runtimeTypes.RemoteObject
    },
    'workerCreated': {
      workerId: t.String,
      frameId: t.String,
      url: t.String,
    },
    'workerDestroyed': {
      workerId: t.String,
    },
    'dispatchMessageFromWorker': {
      workerId: t.String,
      message: t.String,
    },
    'webSocketCreated': {
      frameId: t.String,
      wsid: t.String,
      requestURL: t.String,
    },
    'webSocketOpened': {
      frameId: t.String,
      requestId: t.String,
      wsid: t.String,
      effectiveURL: t.String,
    },
    'webSocketClosed': {
      frameId: t.String,
      wsid: t.String,
      error: t.String,
    },
    'webSocketFrameSent': {
      frameId: t.String,
      wsid: t.String,
      opcode: t.Number,
      data: t.String,
    },
    'webSocketFrameReceived': {
      frameId: t.String,
      wsid: t.String,
      opcode: t.Number,
      data: t.String,
    },
    // One JPEG frame of the browser window, base64 encoded. deviceWidth and
    // deviceHeight are the captured area BEFORE the scale-to-fit, so a viewer
    // can map a point on the image back onto the window.
    //
    // `timestamp` is declared because the Playwright client READS it and it
    // was never declared: the frame number computed from it came out NaN.
    // Seconds, like every other timestamp on this protocol.
    'screencastFrame': {
      data: t.String,
      deviceWidth: t.Number,
      deviceHeight: t.Number,
      timestamp: t.Number,
    },
  },

  methods: {
    'close': {
      params: {
        runBeforeUnload: t.Optional(t.Boolean),
      },
    },
    'setFileInputFiles': {
      params: {
        frameId: t.String,
        objectId: t.String,
        files: t.Array(t.String),
      },
    },
    'dispatchTrustedInputEvents': {
      params: {
        frameId: t.String,
        objectId: t.String,
        types: t.Array(t.String),
      },
    },
    'setViewportSize': {
      params: {
        viewportSize: t.Nullable(pageTypes.Size),
        // STEALTHFOX: same 1.61 drift as pageTypes.Viewport, on the per-page path
        // (page.set_viewport_size). Declared and ignored - the handler reads only
        // viewportSize, and screen dimensions stay owned by the fingerprint prefs.
        screenSize: t.Optional(t.Nullable(pageTypes.Size)),
        isMobile: t.Optional(t.Boolean),
      },
    },
    'bringToFront': {
      params: {
      },
    },
    'setEmulatedMedia': {
      params: {
        type: t.Optional(t.Enum(['screen', 'print', ''])),
        colorScheme: t.Optional(t.Enum(['dark', 'light', 'no-preference'])),
        reducedMotion: t.Optional(t.Enum(['reduce', 'no-preference'])),
        forcedColors: t.Optional(t.Enum(['active', 'none'])),
        contrast: t.Optional(t.Enum(['less', 'more', 'custom', 'no-preference'])),
      },
    },
    'setCacheDisabled': {
      params: {
        cacheDisabled: t.Boolean,
      },
    },
    'describeNode': {
      params: {
        frameId: t.String,
        objectId: t.String,
      },
      returns: {
        contentFrameId: t.Optional(t.String),
        ownerFrameId: t.Optional(t.String),
      },
    },
    'scrollIntoViewIfNeeded': {
      params: {
        frameId: t.String,
        objectId: t.String,
        rect: t.Optional(pageTypes.Rect),
      },
    },
    'setInitScripts': {
      params: {
        scripts: t.Array(pageTypes.InitScript)
      }
    },
    'navigate': {
      params: {
        frameId: t.String,
        url: t.String,
        referer: t.Optional(t.String),
      },
      returns: {
        navigationId: t.Nullable(t.String),
      }
    },
    'goBack': {
      params: {
        frameId: t.String,
      },
      returns: {
        success: t.Boolean,
      },
    },
    'goForward': {
      params: {
        frameId: t.String,
      },
      returns: {
        success: t.Boolean,
      },
    },
    'reload': {
      params: { },
    },
    'adoptNode': {
      params: {
        frameId: t.String,
        // Missing objectId adopts frame owner.
        objectId: t.Optional(t.String),
        executionContextId: t.String,
      },
      returns: {
        remoteObject: t.Nullable(runtimeTypes.RemoteObject),
      },
    },
    'screenshot': {
      params: {
        mimeType: t.Enum(['image/png', 'image/jpeg']),
        clip: pageTypes.Clip,
        quality: t.Optional(t.Number),
        omitDeviceScaleFactor: t.Optional(t.Boolean),
      },
      returns: {
        data: t.String,
      }
    },
    'getContentQuads': {
      params: {
        frameId: t.String,
        objectId: t.String,
      },
      returns: {
        quads: t.Array(pageTypes.DOMQuad),
      },
    },
    'dispatchKeyEvent': {
      params: {
        type: t.String,
        key: t.String,
        keyCode: t.Number,
        location: t.Number,
        code: t.String,
        repeat: t.Boolean,
        text: t.Optional(t.String),
      }
    },
    'dispatchTapEvent': {
      params: {
        x: t.Number,
        y: t.Number,
        modifiers: t.Number,
      }
    },
    'dispatchMouseEvent': {
      params: {
        type: t.Enum(['mousedown', 'mousemove', 'mouseup']),
        button: t.Number,
        x: t.Number,
        y: t.Number,
        modifiers: t.Number,
        clickCount: t.Optional(t.Number),
        buttons: t.Number,
      }
    },
    'dispatchWheelEvent': {
      params: {
        x: t.Number,
        y: t.Number,
        deltaX: t.Number,
        deltaY: t.Number,
        deltaZ: t.Number,
        modifiers: t.Number,
      }
    },
    'insertText': {
      params: {
        text: t.String,
      }
    },
    'handleDialog': {
      params: {
        dialogId: t.String,
        accept: t.Boolean,
        promptText: t.Optional(t.String),
      },
    },
    'setInterceptFileChooserDialog': {
      params: {
        enabled: t.Boolean,
      },
    },
    'sendMessageToWorker': {
      params: {
        frameId: t.String,
        workerId: t.String,
        message: t.String,
      },
    },
    // Live JPEG frames of the browser window. width and height BOUND the
    // frame: it is scaled down to fit and never up.
    //
    // ⛔ `fullWindow` is ours and it is the reason this exists. Upstream
    // captures the window and then crops the chrome away, because it wanted
    // video of the page; with this flag the chrome crop is skipped and the
    // frame keeps the tab strip, the address bar and the chrome-side pointer
    // overlay - which lives in the chrome document and can therefore never
    // appear in a page screenshot. The window's OS frame is cropped in both
    // modes: on Windows 11 that border is transparent on screen and the
    // capturer would otherwise hand over the pixels of whatever lies under
    // it. Default false, which is upstream's behaviour to the pixel.
    'startScreencast': {
      params: {
        width: t.Number,
        height: t.Number,
        quality: t.Optional(t.Number),
        fullWindow: t.Optional(t.Boolean),
        fps: t.Optional(t.Number),
      },
      returns: {
        screencastId: t.String,
      },
    },
    // Flow control: one frame is in flight until this is called, so a slow
    // consumer sees fewer frames rather than a growing queue.
    'screencastFrameAck': {
      params: {
        screencastId: t.String,
      },
    },
    'stopScreencast': {
    },
  },
};

export const protocol = {
  domains: {Browser, Heap, Page, Runtime, Network},
};
