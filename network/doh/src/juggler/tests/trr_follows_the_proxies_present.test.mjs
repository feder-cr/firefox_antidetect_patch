// The DoH state of the session follows the proxies the engine received.
//
// Names are resolved by the browser itself (TRR, one resolver over the session
// proxy), never by the host's resolver. The core declares the provider
// (network.trr.uri and network.trr.bootstrapAddr, user prefs); the engine
// publishes the excluded names and the list of the contexts that have a proxy
// (network.trr.stealth.contexts, read by the resolver hook per lookup) from the
// proxies it receives (TrrPrefs.js), at each event that changes the set: the
// browser proxy, a context proxy, a context that ends. No global
// network.trr.mode is written. A session whose proxy comes per context
// (Browser.setContextProxy, launch without proxy) is covered too.
//
// The real TargetRegistry, BrowserContext and TrrPrefs run here on stubs.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { execFileSync } from 'node:child_process';
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
  // URL is a global of a Gecko module, and of node; a bare vm context has neither.
  return load('TrrPrefs.js', { Services: { prefs }, URL }, ['syncTrrPrefs']);
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
    'network.trr.stealth.contexts': '*',
    'network.trr.exclude-etc-hosts': false,
    'network.trr.excluded-domains': 'gw.example',
  });
  assert.equal(prefs.user.has('network.trr.mode'), false, 'no global mode');
});

test('context proxy only (launch without proxy): the session resolves by DoH too', () => {
  const { registry, prefs, newContext } = session();
  registry.setNoBrowserProxy();
  assert.deepEqual(trr(prefs), {}, 'no proxy yet: retail default, nothing written');
  const context = newContext('ctx-1');
  context.setProxy(proxy('ctx.example'));
  assert.equal(prefs.user.get('network.trr.stealth.contexts'), String(context.userContextId));
  assert.equal(prefs.user.get('network.trr.exclude-etc-hosts'), false);
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'ctx.example');
  assert.equal(prefs.user.has('network.trr.mode'), false, 'no global mode');
});

test('both: browser proxy and context proxy, names of both excluded, sorted', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('zeta.example'));
  newContext('ctx-1').setProxy(proxy('alpha.example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'alpha.example,zeta.example');
  assert.match(prefs.user.get('network.trr.stealth.contexts'), /^\*,\d+$/);
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
  assert.equal(prefs.user.get('network.trr.stealth.contexts'), '*', 'an IP proxy still turns DoH on');
  assert.equal(prefs.user.get('network.trr.excluded-domains'), '');
  newContext('ctx-1').setProxy(proxy('2001:db8::1'));
  newContext('ctx-2').setProxy(proxy('[2001:db8::2]'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), '');
  newContext('ctx-3').setProxy(proxy('Gw.Example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'gw.example');
});

// The DNS suffix match of network.trr.excluded-domains compares the ASCII name
// a lookup carries, so the list holds the ASCII (punycode, lowercase) form.
test('the excluded name is lowercase', () => {
  const { registry, prefs } = session();
  registry.setBrowserProxy(proxy('GW.Example.COM'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'gw.example.com');
});

test('an internationalised proxy name is written in its punycode form', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('proxy.b\u00fccher.example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'proxy.xn--bcher-kva.example');
  newContext('ctx-1').setProxy(proxy('PROXY.B\u00dcCHER.example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'proxy.xn--bcher-kva.example',
      'the same name in another case and spelling is one entry');
});

test('a proxy name that is already punycode is kept as it is', () => {
  const { registry, prefs } = session();
  registry.setBrowserProxy(proxy('proxy.xn--bcher-kva.example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'proxy.xn--bcher-kva.example');
  assert.equal(prefs.user.get('network.trr.stealth.contexts'), '*');
});

test('an IPv6 literal with brackets is not a name, however it is spelled', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('[2001:DB8::1]'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), '');
  newContext('ctx-1').setProxy(proxy('[::1]'));
  newContext('ctx-2').setProxy(proxy('[::ffff:203.0.113.7]'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), '');
  assert.equal(prefs.user.get('network.trr.stealth.contexts'), '*,' + [...registry._browserContextIdToBrowserContext.values()]
      .map(c => c.userContextId).join(','));
});

test('the trailing dot of a fully qualified proxy name is not written', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('Gw.Example.'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'gw.example');
  newContext('ctx-1').setProxy(proxy('gw.example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'gw.example', 'one entry for both spellings');
});

test('a name the URL parser refuses keeps its lowercased form', () => {
  const { registry, prefs } = session();
  registry.setBrowserProxy(proxy('Bad Name.Example'));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'bad name.example');
});

// A profile restarted with the previous session's engine-owned prefs.
const STALE = () => ({
  'network.trr.stealth.contexts': '*',
  'network.trr.exclude-etc-hosts': false,
  'network.trr.excluded-domains': 'old.example',
});

test("a profile that still has the previous session's TRR prefs and a launch that sends setNoBrowserProxy first: the three are cleared, the provider is untouched", () => {
  const { registry, prefs } = session();
  for (const [name, value] of Object.entries(STALE()))
    prefs.user.set(name, value);
  prefs.log.length = 0;
  registry.setNoBrowserProxy();
  assert.deepEqual(trr(prefs), {});
  assert.deepEqual(prefs.log.map(e => e[0]), ['clear', 'clear', 'clear']);
  assert.deepEqual(prefs.log.filter(([, name]) => DECLARED.includes(name)), []);
  assert.equal(prefs.user.get('network.trr.uri'), URI);
  assert.equal(prefs.user.get('network.trr.bootstrapAddr'), BOOTSTRAP);
});

test("a profile with the previous session's TRR prefs and a first setBrowserProxy of a different proxy: excluded-domains is recomputed", () => {
  const { registry, prefs } = session();
  for (const [name, value] of Object.entries(STALE()))
    prefs.user.set(name, value);
  prefs.log.length = 0;
  registry.setBrowserProxy(proxy('new.example'));
  assert.deepEqual(trr(prefs), {
    'network.trr.stealth.contexts': '*',
    'network.trr.exclude-etc-hosts': false,
    'network.trr.excluded-domains': 'new.example',
  });
  assert.deepEqual(prefs.log, [['set', 'network.trr.excluded-domains', 'new.example']],
      'only the pref whose value differs is written');
  assert.equal(prefs.user.get('network.trr.uri'), URI);
  assert.equal(prefs.user.get('network.trr.bootstrapAddr'), BOOTSTRAP);
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
  assert.equal(prefs.user.get('network.trr.stealth.contexts'), '*,' + [...registry._browserContextIdToBrowserContext.values()]
      .filter(c => c._proxy).map(c => c.userContextId).join(','));
});

test('the last proxy removed clears the three prefs (retail without proxy is the default)', async () => {
  const { registry, prefs, newContext } = session();
  registry.setNoBrowserProxy();
  const context = newContext('ctx-1');
  context.setProxy(proxy('ctx.example'));
  assert.equal(prefs.user.get('network.trr.stealth.contexts'), String(context.userContextId));
  await context.destroy();
  assert.deepEqual(trr(prefs), {});
  // And through the browser proxy.
  registry.setBrowserProxy(proxy('gw.example'));
  assert.equal(prefs.user.get('network.trr.stealth.contexts'), '*');
  registry.setNoBrowserProxy();
  assert.deepEqual(trr(prefs), {});
});

test('setNoBrowserProxy with a context proxy still present leaves DoH on', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  newContext('ctx-1').setProxy(proxy('ctx.example'));
  const context = [...registry._browserContextIdToBrowserContext.values()].find(c => c._proxy);
  registry.setNoBrowserProxy();
  assert.equal(prefs.user.get('network.trr.stealth.contexts'), String(context.userContextId));
  assert.equal(prefs.user.get('network.trr.excluded-domains'), 'ctx.example');
});

test('setNoBrowserProxy with no context proxy: mode stays the default', () => {
  const { registry, prefs, newContext } = session();
  newContext('ctx-1');
  registry.setNoBrowserProxy();
  assert.deepEqual(trr(prefs), {});
  assert.equal(prefs.user.has('network.trr.stealth.contexts'), false);
});

test('Browser.setBrowserProxy writes the prefs before it lifts the hold', () => {
  const { registry, prefs } = session();
  const seen = [];
  registry.onBrowserProxyDecided(() => seen.push(trr(prefs)));
  registry.setBrowserProxy(proxy('gw.example'));
  assert.equal(seen.length, 1);
  assert.equal(seen[0]['network.trr.stealth.contexts'], '*');
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
  prefs.setBoolPref = () => { throw new Error('NS_ERROR_FAILURE'); };
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
  assert.equal(prefs.log.length, written, 'same state, no write');
  const one = newContext('ctx-1');
  one.setProxy(proxy('gw.example'));
  assert.deepEqual(prefs.log.slice(written), [['set', 'network.trr.stealth.contexts', '*,' + one.userContextId]],
      'the same name, a new context: only the list changes');
  const again = prefs.log.length;
  one.setProxy(proxy('gw.example', { port: 9999 }));
  assert.equal(prefs.log.length, again, 'same state, no write');
  const two = newContext('ctx-2');
  two.setProxy(proxy('other.example'));
  assert.deepEqual(prefs.log.slice(again), [
    ['set', 'network.trr.excluded-domains', 'gw.example,other.example'],
    ['set', 'network.trr.stealth.contexts', `*,${one.userContextId},${two.userContextId}`],
  ], 'the names first, the list last');
});

test('the list is the last pref written when DoH turns on and the first cleared when it turns off', () => {
  const { registry, prefs } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  assert.equal(prefs.log.at(-1)[1], 'network.trr.stealth.contexts');
  assert.equal(prefs.log.length, 3);
  prefs.log.length = 0;
  registry.setNoBrowserProxy();
  assert.equal(prefs.log[0][1], 'network.trr.stealth.contexts');
  assert.deepEqual(prefs.log.map(e => e[0]), ['clear', 'clear', 'clear']);
});

// network.trr.mode is nobody's but the user's: the DoH service stays in mode 0
// and the resolver hook chooses per lookup.
test('the engine never writes network.trr.mode, whatever the events', async () => {
  const { registry, prefs, newContext } = session();
  const context = newContext('ctx-1');
  registry.setBrowserProxy(proxy('gw.example'));
  context.setProxy(proxy('ctx.example'));
  registry.setNoBrowserProxy();
  await context.destroy();
  assert.deepEqual(prefs.log.filter(([, name]) => name === 'network.trr.mode'), []);
  assert.equal(prefs.user.has('network.trr.mode'), false);
});

test('a mode set by something else is not touched', () => {
  for (const mode of [0, 2, 5]) {
    const { registry, prefs } = session();
    prefs.user.set('network.trr.mode', mode);
    registry.setBrowserProxy(proxy('gw.example'));
    registry.setNoBrowserProxy();
    assert.equal(prefs.user.get('network.trr.mode'), mode);
    assert.deepEqual(prefs.log.filter(([, name]) => name === 'network.trr.mode'), []);
  }
});

// Earlier versions wrote mode 3 while a proxy existed; a profile that keeps it
// would send every lookup over DoH and the per-lookup choice would never run.
test('a mode 3 left by an earlier version is cleared at the first sync, with or without a proxy', () => {
  for (const event of [r => r.setNoBrowserProxy(), r => r.setBrowserProxy(proxy('gw.example'))]) {
    const { registry, prefs } = session();
    prefs.user.set('network.trr.mode', 3);
    event(registry);
    assert.equal(prefs.user.has('network.trr.mode'), false);
    assert.deepEqual(prefs.log[0], ['clear', 'network.trr.mode']);
  }
});

test('a mode 3 is left alone when the provider is not declared by the core', () => {
  const { registry, prefs } = session({ declared: false });
  prefs.user.set('network.trr.mode', 3);
  registry.setNoBrowserProxy();
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
  assert.match(prefs.user.get('network.trr.stealth.contexts'), /^\d+$/, 'a context proxy is still present');
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
  assert.equal(prefs.user.get('network.trr.stealth.contexts'), '*');
});

test('DoH with no declared uri refuses the command, writes nothing, restores the proxy', () => {
  const { registry, prefs, newContext } = session({ declared: false });
  assert.throws(() => registry.setBrowserProxy(proxy('gw.example')), /network\.trr\.uri/);
  assert.equal(registry._browserProxy, null);
  const context = newContext('ctx-1');
  assert.throws(() => context.setProxy(proxy('c.example')), /network\.trr\.uri/);
  assert.equal(context._proxy, null, 'a proxy with no DoH behind it would leak the names');
  assert.deepEqual(prefs.log, [], 'nothing is written before the refusal');
  assert.equal(prefs.user.has('network.trr.stealth.contexts'), false);
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

// localhost and local resolve natively on purpose, as a retail Firefox does, in
// addition to the proxy names in network.trr.excluded-domains: the engine keeps
// Mozilla's built-in exclusions and neither writes nor clears them.
test("the built-in exclusions of retail stay: 'localhost,local', untouched by TrrPrefs.js", () => {
  const text = read('TrrPrefs.js');
  assert.doesNotMatch(text, /builtin-excluded-domains/);
  const tree = path.join(ROOT, '..');
  const lines = execFileSync('git', ['grep', '-n', 'network.trr.builtin-excluded-domains', '--', 'modules/libpref/init/all.js'],
      { cwd: tree, encoding: 'utf8' }).trim().split(/\r?\n/);
  assert.equal(lines.length, 1, 'declared once');
  assert.match(lines[0], /pref\("network\.trr\.builtin-excluded-domains", "localhost,local"\);/);
});
