// A context that closes while its page is still working does not turn its requests into DIRECT ones.
//
// The requests it left in flight (a lookup, a DoH request, a retry) ask the proxy filter after the context left the
// registry. With no browser-wide proxy the answer was "no proxy", which is DIRECT: the DoH request, which exists only
// because the context had a proxy, connected to the DoH server from the host's own address. TargetRegistry now keeps
// the proxy record of a context that ended (juggler/GoneContexts.js, recorded by BrowserContext.destroy before the
// maps forget the context) and proxyRecordFor asks it after the live contexts: the requests keep leaving by the proxy
// of their context. An id that never belonged to a context of ours is answered as before.
//
// The real TargetRegistry, BrowserContext and GoneContexts run here on stubs.
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

const proxy = (host, extra = {}) => ({ type: 'socks', host, port: 1080, bypass: [], ...extra });

// A registry as its constructor leaves it, on the real GoneContexts of its own (the module is one per browser).
function session() {
  let nextUserContextId = 10;
  const modules = {
    Helper,
    syncTrrPrefs() {},
    GoneContexts: load('GoneContexts.js', {}, ['GoneContexts']).GoneContexts,
    ContextualIdentityService: {
      create: () => ({ userContextId: nextUserContextId++ }),
      remove() {},
      getPublicIdentities: () => [],
    },
    newProxyInfoFor: () => ({}),
    proxyForHost: (record, host) => !record || record.bypass.some(s => host.endsWith(s)) ? null : record,
  };
  const { TargetRegistry, BrowserContext } = load('TargetRegistry.js', {
    Ci, Cc: {}, Components: { results: Cr },
    ChromeUtils: { importESModule: () => modules, getAllDOMProcesses: () => [] },
    Services: {
      prefs: {},
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
  registry._updateProxiesWithSameAuthCacheAndDifferentCredentials = () => {};
  registry._syncTrrPrefs = () => {};
  const newContext = withProxy => {
    const context = new BrowserContext(registry, 'ctx-' + nextUserContextId, false);
    if (withProxy)
      context._proxy = withProxy;
    return context;
  };
  return { registry, newContext };
}

// The channel of a request of a context, as the filter reads it.
const channelOf = (userContextId, host = 'doh.example') => ({
  URI: { host },
  loadInfo: { originAttributes: { userContextId } },
});

test('a context with a proxy that ended: its request in flight still gets the proxy, never DIRECT', async () => {
  const { registry, newContext } = session();
  const gw = proxy('gw.example');
  const context = newContext(gw);
  const id = context.userContextId;
  assert.equal(registry.getProxyInfo(channelOf(id)), gw, 'alive: its proxy');
  await context.destroy();
  assert.equal(registry.browserContextForUserContextId(id), undefined, 'the context left the registry');
  assert.equal(registry.getProxyInfo(channelOf(id)), gw, 'gone: still its proxy');
  assert.equal(registry.proxyRecordFor(id), gw);
  assert.equal(registry.proxyFor('late-1.test', id), gw);
});

test('the proxy of a context that ended keeps its own bypass list, as when it was alive', async () => {
  const { registry, newContext } = session();
  const context = newContext(proxy('gw.example', { bypass: ['.internal'] }));
  const id = context.userContextId;
  const before = ['a.internal', 'b.example'].map(host => registry.proxyFor(host, id) === null);
  await context.destroy();
  const after = ['a.internal', 'b.example'].map(host => registry.proxyFor(host, id) === null);
  assert.deepEqual(after, before);
  assert.deepEqual(after, [true, false]);
});

test('a context that ended answers by its own proxy even under a browser-wide one, as when it was alive', async () => {
  const { registry, newContext } = session();
  const launch = proxy('launch.example');
  const own = proxy('own.example');
  registry._browserProxy = launch;
  const context = newContext(own);
  const id = context.userContextId;
  assert.equal(registry.proxyRecordFor(id), own);
  await context.destroy();
  assert.equal(registry.proxyRecordFor(id), own);
  assert.equal(registry.proxyRecordFor(4242), launch, 'an id never known takes the browser proxy, as before');
});

test('a context without a proxy of its own that ended answers as it did alive: the browser proxy, or none', async () => {
  const { registry, newContext } = session();
  const context = newContext(null);
  const id = context.userContextId;
  assert.equal(registry.proxyRecordFor(id), null);
  await context.destroy();
  assert.equal(registry.proxyRecordFor(id), null);
  const launch = proxy('launch.example');
  registry._browserProxy = launch;
  assert.equal(registry.proxyRecordFor(id), launch);
});

test('an id that never was a context of ours is answered as today, and so is no context at all', () => {
  const { registry } = session();
  assert.equal(registry.proxyRecordFor(4242), null);
  assert.equal(registry.proxyRecordFor(undefined), null);
  assert.equal(registry.proxyRecordFor(0), null);
  const launch = proxy('launch.example');
  registry._browserProxy = launch;
  assert.equal(registry.proxyRecordFor(4242), launch);
  assert.equal(registry.proxyRecordFor(undefined), launch);
});

test('every context that ended keeps its own proxy: the ids do not mix', async () => {
  const { registry, newContext } = session();
  const a = newContext(proxy('a.example'));
  const b = newContext(proxy('b.example'));
  const ids = [a.userContextId, b.userContextId];
  await a.destroy();
  await b.destroy();
  assert.equal(registry.proxyRecordFor(ids[0]).host, 'a.example');
  assert.equal(registry.proxyRecordFor(ids[1]).host, 'b.example');
});

test('the default context has no proxy of its own to keep', () => {
  const { GoneContexts } = load('GoneContexts.js', {}, ['GoneContexts']);
  GoneContexts.keep(0, proxy('gw.example'));
  assert.equal(GoneContexts.recordFor(0), null);
  GoneContexts.keep(7, null);
  assert.equal(GoneContexts.recordFor(7), null);
});

test('the proxy is kept before the context is forgotten, and the module is installed', () => {
  const text = read('TargetRegistry.js');
  const destroy = text.slice(text.indexOf('  async destroy() {'), text.indexOf('  async setServiceWorkersBlocked('));
  const keep = destroy.indexOf('GoneContexts.keep(this.userContextId, this._proxy);');
  assert.ok(keep > 0, 'destroy does not keep the proxy');
  assert.ok(keep < destroy.indexOf('_browserContextIdToBrowserContext.delete('), 'kept after the maps forgot the context');
  assert.ok(keep < destroy.indexOf('_userContextIdToBrowserContext.delete('), 'kept after the maps forgot the context');
  assert.match(text, /importESModule\('resource:\/\/gre\/modules\/juggler\/GoneContexts\.js'\)/);
  assert.match(read('moz.build'), /\n    "GoneContexts\.js",\n/);
});
