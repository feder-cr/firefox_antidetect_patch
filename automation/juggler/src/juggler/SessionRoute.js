/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * The session's road out, as one module that two PROCESSES load.
 *
 * The browser loads it for its channel filter (NetworkObserver.js), which sends
 * every request of the session through the proxy that `Browser.setBrowserProxy`
 * or `BrowserContext.setProxy` declared. The pingsender background task loads
 * it too: at exit the browser starts `firefox --backgroundtask pingsender` to
 * deliver the shutdown pings, and that process has to leave by the same road
 * (B266). It used to be `pingsender[.exe]`, a separate executable that took its
 * road from the HOST - WinINet's system settings on Windows, libcurl's
 * environment on Linux - and so sent the ping from the machine's own address
 * while the session was behind a proxy. WinINet cannot even express a SOCKS5
 * proxy or SOCKS credentials, so no amount of arguments could have fixed it
 * there.
 *
 * So what turns a proxy record into a road lives here, once, and both
 * processes read the same lines: there is no second translation in the
 * background task that could drift from the browser's.
 */

/**
 * The proxy types whose credentials belong INSIDE the nsIProxyInfo.
 *
 * ⛔ THIS IS NOT A STYLE CHOICE, IT IS WHAT GECKO IMPLEMENTS, and reading it as
 * one is what took HTTP proxies out for seven releases.
 * `nsProtocolProxyService::NewProxyInfoWithAuth`
 * (netwerk/base/nsProtocolProxyService.cpp) refuses outright:
 *
 *     // We have only implemented username/password for SOCKS proxies.
 *     if ((!aUsername.IsEmpty() || !aPassword.IsEmpty()) &&
 *         !aType.LowerCaseEqualsASCII(kProxyType_SOCKS) &&
 *         !aType.LowerCaseEqualsASCII(kProxyType_SOCKS4)) {
 *       return NS_ERROR_NOT_IMPLEMENTED;
 *     }
 *
 * The two families are not two spellings of one idea. SOCKS authenticates
 * inside its own handshake (RFC 1929), so the credentials have to travel with
 * the endpoint and there is nowhere else to put them. HTTP authenticates with a
 * 407 challenge (RFC 7235), which arrives on the channel and is answered by
 * `promptAuth` in NetworkObserver.js, where the AUTH_PROXY branch reads these
 * same credentials back out of the registry
 * (TargetRegistry.js). So handing HTTP credentials to
 * the endpoint is not merely unsupported: it is the wrong mechanism, and the
 * right one was already wired and already working.
 */
const PROXY_TYPES_THAT_CARRY_THEIR_OWN_CREDENTIALS = new Set(['socks', 'socks4']);

const UINT32_MAX = Math.pow(2, 32) - 1;

/**
 * ONE expression of how a stored proxy record becomes an nsIProxyInfo.
 *
 * It is a function rather than a line inside the channel filter because two
 * callers need the same answer and only one of them used to ask. The filter
 * built the proxy info per channel; nothing built it when the proxy was SET. A
 * proxy this engine cannot express therefore reached the filter, threw there,
 * and the filter fell on to hand necko the DEFAULT proxy info, which is DIRECT.
 * The page then left over the host's own address, with no error raised
 * anywhere, which is the precise opposite of what asking for a proxy means.
 *
 * Throwing is the point of it. `setBrowserProxy` and `BrowserContext.setProxy`
 * call this while the command is still on the wire, so a proxy that cannot be
 * expressed refuses the command that asked for it instead of turning into a
 * silent leak one navigation later.
 */
export function newProxyInfoFor(proxy) {
  const carriesOwnCredentials = PROXY_TYPES_THAT_CARRY_THEIR_OWN_CREDENTIALS.has(proxy.type);
  const protocolProxyService = Cc['@mozilla.org/network/protocol-proxy-service;1'].getService();
  return protocolProxyService.newProxyInfoWithAuth(
      proxy.type,
      proxy.host,
      proxy.port,
      carriesOwnCredentials ? (proxy.username || '') : '',
      carriesOwnCredentials ? (proxy.password || '') : '',
      '', /* aProxyAuthorizationHeader */
      '', /* aConnectionIsolationKey */
      Ci.nsIProxyInfo.TRANSPARENT_PROXY_RESOLVES_HOST, /* aFlags */
      UINT32_MAX, /* aFailoverTimeout */
      null, /* failover proxy */
  );
}

/**
 * The record that applies to a request for `host`: `record` itself, or null
 * when one of its `bypass` suffixes matches and the request takes the default
 * road. The one place the bypass rule is written; the browser's filter and the
 * pingsender background task both ask it.
 */
export function proxyForHost(record, host) {
  if (!record || record.bypass.some(domainSuffix => host.endsWith(domainSuffix)))
    return null;
  return record;
}

/**
 * The callbacks that answer an HTTP proxy's 407 for a request that has no
 * page: the pingsender background task.
 *
 * In the browser the same answer comes from `promptAuth` in NetworkObserver.js,
 * which needs the page's network to exist. A ping has no page, so it gets this:
 * the record's credentials for a PROXY challenge, and nothing for anything
 * else. A SOCKS proxy never asks, because its credentials travel inside the
 * nsIProxyInfo (see above). A challenge that already failed once is refused
 * rather than retried with the same words.
 */
export function proxyAuthCallbacks(proxy) {
  const prompt = {
    QueryInterface: ChromeUtils.generateQI([Ci.nsIInterfaceRequestor, Ci.nsIAuthPrompt2]),
    getInterface(iid) {
      if (iid.equals(Ci.nsIAuthPrompt2))
        return prompt;
      throw Components.Exception('', Cr.NS_ERROR_NO_INTERFACE);
    },
    promptAuth(channel, level, authInfo) {
      if (!(authInfo.flags & Ci.nsIAuthInformation.AUTH_PROXY))
        return false;
      if (authInfo.flags & Ci.nsIAuthInformation.PREVIOUS_FAILED)
        return false;
      if (!proxy.username)
        return false;
      authInfo.username = proxy.username;
      authInfo.password = proxy.password || '';
      return true;
    },
    asyncPromptAuth(channel, callback, context, level, authInfo) {
      let cancelled = false;
      Promise.resolve().then(() => {
        if (cancelled)
          return;
        if (prompt.promptAuth(channel, level, authInfo))
          callback.onAuthAvailable(context, authInfo);
        else
          callback.onAuthCancelled(context, true);
      });
      return {
        QueryInterface: ChromeUtils.generateQI([Ci.nsICancelable]),
        cancel: () => {
          cancelled = true;
          callback.onAuthCancelled(context, false);
        },
      };
    },
  };
  return prompt;
}

/**
 * The proxy the session's UDP leaves by, handed to the engine.
 *
 * Whether UDP goes through the proxy is decided in one place in the engine
 * (netwerk/socket/nsSOCKSUDPIOLayer.cpp, StealthSessionUdpProxy) from two
 * inputs: the core's verdict, `network.proxy.socks_remote_udp`, and the proxy
 * this session is using. The proxy used to be read from `network.proxy.*`,
 * which a Juggler session never writes (the type stays 5, SessionRoute is the
 * road), so the SOCKS UDP layer never armed. The session's proxy is HERE, so
 * here it is handed over.
 *
 * It goes through `nsIStealthSessionUdpProxy` (juggler/udp), which keeps the
 * host, the port and the credentials in the parent and sends them to the socket
 * process, where ICE's UDP sockets are, by IPC. Never through a pref: libpref
 * would put the credentials in every content process, and the credentials are
 * in no pref, no prefs.js and no log. Nothing below prints a value.
 *
 * Only when the verdict is true, and only for ONE SOCKS5 proxy that every
 * request of the session shares. A UDP socket has no URL and no user context to
 * ask a per-context road, so two different proxies in one session, or a context
 * that goes direct next to one that does not, cannot be told apart at the
 * socket: nothing is handed over then, and the engine's direct-UDP filter stays
 * in force. A bypass list is not applied to UDP either: it is a list of host
 * name suffixes, and an ICE server is reached by address.
 */
function sameRoad(a, b) {
  return a.type === b.type && a.host === b.host && a.port === b.port &&
      (a.username || '') === (b.username || '') && (a.password || '') === (b.password || '');
}

/**
 * The SOCKS5 record that carries ALL of the session's UDP, or null.
 *
 * `roads` is the record every request of the session leaves by: one entry per
 * browser context (its own proxy, else the browser's) and the browser's own
 * for what has no context, null for a road that goes direct. SOCKS4 has no UDP.
 */
export function sessionUdpProxy(roads) {
  if (!roads.length || roads.some(road => !road))
    return null;
  const first = roads[0];
  if (first.type !== 'socks' || !roads.every(road => sameRoad(first, road)))
    return null;
  if (!first.host || !(first.port > 0 && first.port <= 65535))
    return null;
  return first;
}

/**
 * Hand the session's UDP proxy to `service` (nsIStealthSessionUdpProxy), or
 * clear it. `enabled` is the core's verdict. Returns whether a proxy is held.
 */
export function publishSessionUdpProxy(service, enabled, browserProxy, contextProxies) {
  const roads = contextProxies.map(proxy => proxy || browserProxy);
  roads.push(browserProxy);
  const proxy = enabled ? sessionUdpProxy(roads) : null;
  if (!proxy) {
    service.clear();
    return false;
  }
  service.set(proxy.host, proxy.port, proxy.username || '', proxy.password || '');
  return true;
}
