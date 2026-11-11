// The DoH state of the session follows the proxies the engine received.
//
// Names are resolved by the browser itself (TRR, mode 3, one resolver over the
// session proxy), never by the host's resolver. The core declares the provider
// (network.trr.uri and network.trr.bootstrapAddr, user prefs); the engine
// derives only on/off and the excluded names from the proxies it receives
// (TrrPrefs.js), at each event that changes the set: the browser proxy, a
// context proxy, a context that ends. A session whose proxy comes per context
// (Browser.setContextProxy, launch without proxy) gets DoH too.
//
// The real TargetRegistry, BrowserContext and TrrPrefs run here on stubs.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = rel => fs.readFileSync(path.join(ROOT, rel), 'utf8');

function load(file, scope, names) {
  const source = read(file).replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

const Cr = { NS_BINDING_ABORTED: 0x804b0002, NS_ERROR_FAILURE: 0x80004005 };
const Ci = { nsIProtocolProxyChannelFilter: {} };

class EventEmitter {
  on() {}
  off() {}
  once() {}
  emit() {}
}

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Services: { obs: { addObserver() {}, removeObserver() {} } },
  Ci, Cr,
  ChromeUtils: { importESModule: () => ({ EventEmitter }) },
}, ['Helper']);

// The user values of a pref branch, and the order of the writes.
function fakePrefs() {
  const user = new Map();
  const log = [];
  const need = name => {
    if (!user.has(name))
      throw new Error('NS_ERROR_UNEXPECTED ' + name);
    return user.get(name);
  };
  return {
    user, log,
    prefHasUserValue: name => user.has(name),
    getIntPref: need, getBoolPref: need,
    // Like Services.prefs: the fallback answers when nothing has a value.
    getStringPref: (name, fallback) => user.has(name) || fallback === undefined ? need(name) : fallback,
    setIntPref: (name, v) => { log.push(['set', name, v]); user.set(name, v); },
    setStringPref: (name, v) => { log.push(['set', name, v]); user.set(name, v); },
    setBoolPref: (name, v) => { log.push(['set', name, v]); user.set(name, v); },
    clearUserPref: name => { log.push(['clear', name]); user.delete(name); },
  };
}

const URI = 'https://mozilla.cloudflare-dns.com/dns-query';
const BOOTSTRAP = '162.159.61.4';

// The provider is the core's; the other three prefs are the engine's.
const DECLARED = ['network.trr.uri', 'network.trr.bootstrapAddr'];

function trr(prefs) {
  return Object.fromEntries([...prefs.user]
      .filter(([k]) => k.startsWith('network.trr.') && !DECLARED.includes(k)));
}

function loadTrr(prefs) {
  return load('TrrPrefs.js', { Services: { prefs } }, ['syncTrrPrefs']);
}

const proxy = (host, extra = {}) => ({ type: 'socks', host, port: 1080, bypass: [], ...extra });

// A registry and its contexts, as the constructor leaves them.
// `declared` emulates the core's declaration of the provider.
function session({ declared = true } = {}) {
  const prefs = fakePrefs();
  if (declared) {
    prefs.user.set('network.trr.uri', URI);
    prefs.user.set('network.trr.bootstrapAddr', BOOTSTRAP);
  }
  const { syncTrrPrefs } = loadTrr(prefs);
  let nextUserContextId = 10;
  const modules = {
    Helper, syncTrrPrefs,
    ContextualIdentityService: {
      create: () => ({ userContextId: nextUserContextId++ }),
      remove() {},
      getPublicIdentities: () => [],
    },
    newProxyInfoFor: p => {
      if (p.type === 'refused')
        throw new Error('NS_ERROR_NOT_IMPLEMENTED');
      return {};
    },
    proxyForHost: (record, host) => !record || record.bypass.some(s => host.endsWith(s)) ? null : record,
  };
  const { TargetRegistry, BrowserContext } = load('TargetRegistry.js', {
    Ci, Cc: {}, Components: { results: Cr },
    ChromeUtils: { importESModule: () => modules, getAllDOMProcesses: () => [] },
    Services: {
      prefs,
      obs: { notifyObservers() {} },
      ppmm: { sharedData: { has: () => false, set() {}, delete() {}, flush() {} } },
    },
    NetUtil: {},
    dump: () => {},
    setTimeout,
    Date,
  }, ['TargetRegistry', 'BrowserContext']);
  const registry = Object.create(TargetRegistry.prototype);
  registry._browserProxy = null;
  registry._browserProxyDecided = false;
  registry._browserProxyListeners = [];
  registry._proxiesWithClashingAuthCacheKeys = new Set();
  registry._browserContextIdToBrowserContext = new Map();
  registry._userContextIdToBrowserContext = new Map();
  const newContext = id => new BrowserContext(registry, id, false);
  return { registry, prefs, newContext };
}

test('launch proxy only: the three prefs are user values and the proxy name is excluded', () => {
  const { registry, prefs } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  assert.deepEqual(trr(prefs), {
    'network.trr.mode': 3,
    'network.trr.exclude-etc-hosts': false,
    'network.trr.excluded-domains': 'gw.example',
  });
});

test('context proxy only (launch without proxy): the session resolves by DoH too', () => {
  const { registry, prefs, newContext } = session();
  registry.setNoBrowserProxy();
  assert.deepEqual(trr(prefs), {}, 'no proxy yet: retail default, nothing written');
  const context = newContext('ctx-1');
  context.setProxy(proxy('ctx.example'));
  assert.equal(prefs.user.get('network.trr.mode'), 3);
  assert.equal(prefs.user.get('network.trr.exclude-etc-hosts'), false);
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'ctx.example');
});

test('both: browser proxy and context proxy, names of both excluded, sorted', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('zeta.example'));
  newContext('ctx-1').setProxy(proxy('alpha.example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'alpha.example,zeta.example');
  assert.equal(prefs.user.get('network.trr.mode'), 3);
});

test('names of two contexts are both excluded, and one name once', () => {
  const { registry, prefs, newContext } = session();
  registry.setNoBrowserProxy();
  newContext('ctx-1').setProxy(proxy('b.example'));
  newContext('ctx-2').setProxy(proxy('a.example'));
  newContext('ctx-3').setProxy(proxy('b.example', { port: 1081 }));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'a.example,b.example');
});

test('a proxy given by IP (v4, v6, bracketed) is not excluded; one given by name is', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('203.0.113.7'));
  assert.equal(prefs.user.get('network.trr.mode'), 3, 'an IP proxy still turns DoH on');
  assert.equal(prefs.user.get('network.trr.excluded-domains'), '');
  newContext('ctx-1').setProxy(proxy('2001:db8::1'));
  newContext('ctx-2').setProxy(proxy('[2001:db8::2]'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), '');
  newContext('ctx-3').setProxy(proxy('Gw.Example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'gw.example');
});

test('a context that ends is recomputed out of the set', async () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  const one = newContext('ctx-1');
  one.setProxy(proxy('one.example'));
  newContext('ctx-2').setProxy(proxy('two.example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'gw.example,one.example,two.example');
  await one.destroy();
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'gw.example,two.example');
  assert.equal(prefs.user.get('network.trr.mode'), 3);
});

test('the last proxy removed clears the three prefs (retail without proxy is the default)', async () => {
  const { registry, prefs, newContext } = session();
  registry.setNoBrowserProxy();
  const context = newContext('ctx-1');
  context.setProxy(proxy('ctx.example'));
  assert.equal(prefs.user.get('network.trr.mode'), 3);
  await context.destroy();
  assert.deepEqual(trr(prefs), {});
  // And through the browser proxy.
  registry.setBrowserProxy(proxy('gw.example'));
  assert.equal(prefs.user.get('network.trr.mode'), 3);
  registry.setNoBrowserProxy();
  assert.deepEqual(trr(prefs), {});
});

test('setNoBrowserProxy with a context proxy still present leaves DoH on', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  newContext('ctx-1').setProxy(proxy('ctx.example'));
  registry.setNoBrowserProxy();
  assert.equal(prefs.user.get('network.trr.mode'), 3);
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'ctx.example');
});

test('setNoBrowserProxy with no context proxy: mode stays the default', () => {
  const { registry, prefs, newContext } = session();
  newContext('ctx-1');
  registry.setNoBrowserProxy();
  assert.deepEqual(trr(prefs), {});
  assert.equal(prefs.user.has('network.trr.mode'), false);
});

test('Browser.setBrowserProxy writes the prefs before it lifts the hold', () => {
  const { registry, prefs } = session();
  const seen = [];
  registry.onBrowserProxyDecided(() => seen.push(trr(prefs)));
  registry.setBrowserProxy(proxy('gw.example'));
  assert.equal(seen.length, 1);
  assert.equal(seen[0]['network.trr.mode'], 3);
  assert.equal(seen[0]['network.trr.excluded-domains'], 'gw.example');
  // The same for the explicit no-proxy decision: cleared before the hold lifts.
  seen.length = 0;
  registry.setNoBrowserProxy();
  assert.deepEqual(seen, [{}]);
});

test('a proxy the engine refuses touches no pref and does not lift the hold', () => {
  const { registry, prefs, newContext } = session();
  let lifted = 0;
  registry.onBrowserProxyDecided(() => lifted++);
  assert.throws(() => registry.setBrowserProxy(proxy('gw.example', { type: 'refused' })), /NOT_IMPLEMENTED/);
  assert.throws(() => newContext('ctx-1').setProxy(proxy('c.example', { type: 'refused' })), /NOT_IMPLEMENTED/);
  assert.deepEqual(trr(prefs), {});
  assert.equal(lifted, 0);
});

test('a pref that cannot be written refuses the command and leaves the proxy unset', () => {
  const { registry, prefs, newContext } = session();
  registry.setNoBrowserProxy();
  prefs.setIntPref = () => { throw new Error('NS_ERROR_FAILURE'); };
  const context = newContext('ctx-1');
  assert.throws(() => context.setProxy(proxy('c.example')), /NS_ERROR_FAILURE/);
  assert.equal(context._proxy, null, 'a proxy with no DoH behind it would leak the names');
  assert.throws(() => registry.setBrowserProxy(proxy('gw.example')), /NS_ERROR_FAILURE/);
  assert.equal(registry._browserProxy, null);
});

test('the sync is idempotent: it writes only the prefs whose value changes', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  const written = prefs.log.length;
  registry.setBrowserProxy(proxy('gw.example', { port: 9999 }));
  newContext('ctx-1').setProxy(proxy('gw.example'));
  assert.equal(prefs.log.length, written, 'same state, no write');
  newContext('ctx-2').setProxy(proxy('other.example'));
  assert.deepEqual(prefs.log.slice(written), [['set', 'network.trr.excluded-domains', 'gw.example,other.example']]);
});

test('mode is the last pref written when DoH turns on and the first cleared when it turns off', () => {
  const { registry, prefs } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  assert.equal(prefs.log.at(-1)[1], 'network.trr.mode');
  assert.equal(prefs.log.length, 3);
  prefs.log.length = 0;
  registry.setNoBrowserProxy();
  assert.equal(prefs.log[0][1], 'network.trr.mode');
  assert.deepEqual(prefs.log.map(e => e[0]), ['clear', 'clear', 'clear']);
});

test('a mode set by something else is replaced by ours when it differs, not trusted', () => {
  const { registry, prefs } = session();
  prefs.user.set('network.trr.mode', 2);
  registry.setBrowserProxy(proxy('gw.example'));
  assert.equal(prefs.user.get('network.trr.mode'), 3);
});

test('the engine never writes or clears the provider the core declared', () => {
  const { registry, prefs, newContext } = session();
  const touched = () => prefs.log.filter(([, name]) => DECLARED.includes(name));
  registry.setBrowserProxy(proxy('gw.example'));
  newContext('ctx-1').setProxy(proxy('ctx.example'));
  registry.setNoBrowserProxy();
  registry.setBrowserProxy(proxy('gw.example', { port: 9999 }));
  assert.deepEqual(touched(), []);
  registry.setNoBrowserProxy();
  assert.equal(prefs.user.get('network.trr.mode'), 3, 'a context proxy is still present');
  assert.equal(prefs.user.get('network.trr.uri'), URI);
  assert.equal(prefs.user.get('network.trr.bootstrapAddr'), BOOTSTRAP);
});

test('with every proxy gone the three are cleared and the provider stays', () => {
  const { registry, prefs } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  registry.setNoBrowserProxy();
  assert.deepEqual(trr(prefs), {});
  assert.deepEqual(prefs.log.filter(([, name]) => DECLARED.includes(name)), []);
  assert.equal(prefs.user.get('network.trr.uri'), URI);
  assert.equal(prefs.user.get('network.trr.bootstrapAddr'), BOOTSTRAP);
});

test('a provider the core declared with other values is left as it is', () => {
  const { registry, prefs } = session();
  prefs.user.set('network.trr.uri', 'https://other.example/dns-query');
  prefs.user.set('network.trr.bootstrapAddr', '192.0.2.1');
  registry.setBrowserProxy(proxy('gw.example'));
  assert.equal(prefs.user.get('network.trr.uri'), 'https://other.example/dns-query');
  assert.equal(prefs.user.get('network.trr.bootstrapAddr'), '192.0.2.1');
  assert.equal(prefs.user.get('network.trr.mode'), 3);
});

test('mode 3 with no declared uri refuses the command, writes nothing, restores the proxy', () => {
  const { registry, prefs, newContext } = session({ declared: false });
  assert.throws(() => registry.setBrowserProxy(proxy('gw.example')), /network\.trr\.uri/);
  assert.equal(registry._browserProxy, null);
  const context = newContext('ctx-1');
  assert.throws(() => context.setProxy(proxy('c.example')), /network\.trr\.uri/);
  assert.equal(context._proxy, null, 'a proxy with no DoH behind it would leak the names');
  assert.deepEqual(prefs.log, [], 'nothing is written before the refusal');
  assert.equal(prefs.user.has('network.trr.mode'), false);
});

test('an empty uri counts as no declaration, and no proxy needs none', () => {
  const { registry, prefs } = session({ declared: false });
  prefs.user.set('network.trr.uri', '');
  assert.throws(() => registry.setBrowserProxy(proxy('gw.example')), /network\.trr\.uri/);
  registry.setNoBrowserProxy();
  assert.deepEqual(trr(prefs), {});
});

test('TrrPrefs.js holds no provider: no constant, no address, no name', () => {
  const text = read('TrrPrefs.js');
  assert.doesNotMatch(text, /TRR_URI|TRR_BOOTSTRAP|162\.159|cloudflare/i);
  assert.doesNotMatch(text, /['"]network\.trr\.bootstrapAddr['"]/);
  // The only mention of the uri is the read of the core's declaration.
  assert.equal(text.match(/['"]network\.trr\.uri['"]/g).length, 1);
  assert.match(text, /getStringPref\('network\.trr\.uri', ''\)/);
  for (const file of ['TargetRegistry.js', 'SessionRoute.js', 'EarlyProxyHold.js', 'NetworkObserver.js',
    'protocol/BrowserHandler.js'])
    assert.doesNotMatch(read(file), /162\.159\.61\.4|cloudflare-dns/, file);
});

test('the registry calls the one function at each event, before the hold lifts', () => {
  const text = read('TargetRegistry.js');
  assert.match(text, /importESModule\('resource:\/\/gre\/modules\/juggler\/TrrPrefs\.js'\)/);
  const between = (from, to) => text.slice(text.indexOf(from), text.indexOf(to, text.indexOf(from)));
  const browserProxy = between('  setBrowserProxy(proxy) {', '  get browserProxyDecided()');
  assert.ok(browserProxy.indexOf('this._syncTrrPrefs()') > 0);
  assert.ok(browserProxy.indexOf('this._syncTrrPrefs()') < browserProxy.indexOf('this._decideBrowserProxy()'));
  const noProxy = between('  setNoBrowserProxy() {', '  get browserProxyDecided()');
  assert.ok(noProxy.indexOf('this._syncTrrPrefs()') > 0);
  assert.ok(noProxy.indexOf('this._syncTrrPrefs()') < noProxy.indexOf('this._decideBrowserProxy()'));
  const contextProxy = between('  setProxy(proxy) {', '  setIgnoreHTTPSErrors(');
  assert.match(contextProxy, /this\._registry\._syncTrrPrefs\(\)/);
  const destroy = between('  async destroy() {', '  async setServiceWorkersBlocked(');
  assert.ok(destroy.indexOf('_userContextIdToBrowserContext.delete') < destroy.indexOf('this._registry._syncTrrPrefs()'));
  // One function recomputes the state; nothing else in the Juggler writes a TRR pref.
  for (const file of ['SessionRoute.js', 'EarlyProxyHold.js', 'NetworkObserver.js', 'protocol/BrowserHandler.js'])
    assert.doesNotMatch(read(file), /network\.trr\./, file);
});

test('TrrPrefs.js is installed with the other modules', () => {
  assert.match(read('moz.build'), /"StealthCursor\.js",\n\s+"TargetRegistry\.js",\n\s+"TrrPrefs\.js",\n\]/);
});
