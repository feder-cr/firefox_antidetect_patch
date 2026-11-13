// The list of the contexts that have a proxy (network.trr.stealth.contexts) is
// the registry's own answer, published.
//
// The engine's resolver hook chooses per lookup by the user context of the
// lookup: a context the list covers resolves over DoH through its proxy, any
// other resolves natively. The list must therefore say, after EVERY event that
// changes a proxy, exactly what the channel filter would say: for a context id,
// proxyFor(id) is a proxy iff the list holds `*` or the id. A lookup that finds
// "no proxy" for a context whose proxy is active leaks the name to the host's
// resolver.
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

const LIST = 'network.trr.stealth.contexts';
const NAMES = 'network.trr.excluded-domains';
const ETC_HOSTS = 'network.trr.exclude-etc-hosts';

// `fail.on(kind, name)` makes a write ('set') or a clear ('clear') throw.
function fakePrefs() {
  const user = new Map();
  const log = [];
  const fail = { on: null };
  const need = name => {
    if (!user.has(name))
      throw new Error('NS_ERROR_UNEXPECTED ' + name);
    return user.get(name);
  };
  const set = (name, v) => {
    if (fail.on && fail.on('set', name))
      throw new Error('NS_ERROR_FAILURE ' + name);
    log.push(['set', name, v]);
    user.set(name, v);
  };
  return {
    user, log, fail,
    prefHasUserValue: name => user.has(name),
    getIntPref: need, getBoolPref: need,
    getStringPref: (name, fallback) => user.has(name) || fallback === undefined ? need(name) : fallback,
    setIntPref: set, setStringPref: set, setBoolPref: set,
    clearUserPref: name => {
      if (fail.on && fail.on('clear', name))
        throw new Error('NS_ERROR_FAILURE ' + name);
      log.push(['clear', name]);
      user.delete(name);
    },
  };
}

const trrModule = prefs => load('TrrPrefs.js', { Services: { prefs }, URL },
    ['syncTrrPrefs', 'contextsWithProxy', 'trrStateFor']);

// The user values, in a fixed order (a rolled back pref is re-inserted last).
const values = map => [...map].sort(([a], [b]) => a < b ? -1 : 1);

const proxy = (host, extra = {}) => ({ type: 'socks', host, port: 1080, bypass: [], ...extra });

function session() {
  const prefs = fakePrefs();
  prefs.user.set('network.trr.uri', 'https://mozilla.cloudflare-dns.com/dns-query');
  const { syncTrrPrefs } = trrModule(prefs);
  let nextUserContextId = 10;
  const modules = {
    Helper, syncTrrPrefs,
    GoneContexts: load('GoneContexts.js', {}, ['GoneContexts']).GoneContexts,
    ContextualIdentityService: {
      create: () => ({ userContextId: nextUserContextId++ }),
      remove() {},
      getPublicIdentities: () => [],
    },
    newProxyInfoFor: p => {
      if (!p || p.type === 'refused')
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
  // A context with a browser context id has its own userContextId; the default
  // context (id undefined) is userContextId 0.
  const newContext = id => new BrowserContext(registry, id, false);
  return { registry, prefs, newContext };
}

// What the list says about a user context id.
function covered(prefs, userContextId) {
  const tokens = prefs.user.has(LIST) ? prefs.user.get(LIST).split(',') : [];
  return tokens.includes('*') || tokens.includes(String(userContextId));
}

// After an event: for every live context, and for an id the registry does not
// know (the filter takes the browser proxy for it), the list agrees with the
// channel filter's view, and its text is the canonical form of the registry's
// state.
function assertMatches(registry, prefs, label) {
  const contexts = [...registry._browserContextIdToBrowserContext.values()];
  for (const id of [...contexts.map(c => c.userContextId), 4242]) {
    const proxied = registry.proxyFor('site.example', id) !== null;
    assert.equal(covered(prefs, id), proxied,
        `${label}: context ${id}, the filter says ${proxied}, the list is '${prefs.user.get(LIST)}'`);
    assert.equal(registry.proxyRecordFor(id) !== null, proxied, label);
  }
  const ids = [...new Set(contexts.filter(c => c._proxy).map(c => c.userContextId))].sort((a, b) => a - b);
  const expected = (registry._browserProxy ? ['*'] : []).concat(ids.map(String)).join(',');
  assert.equal(prefs.user.get(LIST) ?? '', expected, `${label}: canonical form`);
  assert.equal(prefs.user.has(LIST), expected !== '', `${label}: absent when no proxy`);
  assert.equal(prefs.user.has('network.trr.mode'), false, `${label}: no global mode`);
}

test('contextsWithProxy: the pure function', () => {
  const { contextsWithProxy } = trrModule(fakePrefs());
  const p = proxy('gw.example');
  const ctx = (userContextId, withProxy) => ({ userContextId, proxy: withProxy ? p : null });
  assert.equal(contextsWithProxy(null, []), '');
  assert.equal(contextsWithProxy(p, []), '*');
  assert.equal(contextsWithProxy(null, [ctx(7, true)]), '7');
  assert.equal(contextsWithProxy(null, [ctx(7, false), ctx(8, false)]), '', 'contexts without a proxy are not listed');
  assert.equal(contextsWithProxy(p, [ctx(12, true), ctx(3, true), ctx(9, false)]), '*,3,12', '* first, ids ascending');
  assert.equal(contextsWithProxy(null, [ctx(10, true), ctx(2, true)]), '2,10', 'numeric, not lexical');
  assert.equal(contextsWithProxy(null, [ctx(0, true), ctx(5, true)]), '0,5', 'the default context is 0');
  assert.equal(contextsWithProxy(p, [ctx(5, true), ctx(5, true)]), '*,5', 'once each');
  assert.equal(contextsWithProxy(null, new Set([ctx(1, true)])), '1', 'any iterable');
  // Equal states give equal strings, whatever the order they arrive in.
  assert.equal(contextsWithProxy(null, [ctx(4, true), ctx(2, true), ctx(9, true)]),
      contextsWithProxy(null, [ctx(9, true), ctx(4, true), ctx(2, true)]));
});

test('after every event the list is what the channel filter says', async () => {
  const { registry, prefs, newContext } = session();
  assertMatches(registry, prefs, 'start');
  registry.setNoBrowserProxy();
  assertMatches(registry, prefs, 'setNoBrowserProxy');

  const a = newContext('a');
  assertMatches(registry, prefs, 'context created');
  a.setProxy(proxy('a.example'));
  assertMatches(registry, prefs, 'a.setProxy');
  assert.equal(prefs.user.get(LIST), String(a.userContextId));
  a.setProxy(proxy('a2.example'));
  assertMatches(registry, prefs, 'a.setProxy replaced');

  const b = newContext('b');
  b.setProxy(proxy('b.example'));
  assertMatches(registry, prefs, 'b.setProxy');
  assert.equal(prefs.user.get(LIST), `${a.userContextId},${b.userContextId}`);

  registry.setBrowserProxy(proxy('gw.example'));
  assertMatches(registry, prefs, 'launch proxy + two context proxies');
  assert.equal(prefs.user.get(LIST), `*,${a.userContextId},${b.userContextId}`);

  const c = newContext('c');
  assertMatches(registry, prefs, 'context without its own proxy under the launch proxy');
  assert.equal(covered(prefs, c.userContextId), true, 'covered through *');

  registry.setBrowserProxy(proxy('gw2.example'));
  assertMatches(registry, prefs, 'launch proxy replaced');

  await a.destroy();
  assertMatches(registry, prefs, 'a destroyed');
  registry.setNoBrowserProxy();
  assertMatches(registry, prefs, 'launch proxy removed, b still has one');
  assert.equal(prefs.user.get(LIST), String(b.userContextId));
  assert.equal(covered(prefs, c.userContextId), false, 'c is native again');

  await c.destroy();
  assertMatches(registry, prefs, 'c destroyed (no proxy of its own)');
  await b.destroy();
  assertMatches(registry, prefs, 'last proxy gone');
  assert.equal(prefs.user.has(LIST), false);
});

test('the default context (userContextId 0) is a token like any other', () => {
  const { registry, prefs, newContext } = session();
  registry.setNoBrowserProxy();
  const defaultContext = newContext(undefined);
  assert.equal(defaultContext.userContextId, 0);
  defaultContext.setProxy(proxy('d.example'));
  assert.equal(prefs.user.get(LIST), '0');
  assertMatches(registry, prefs, 'default context proxy');
  const other = newContext('x');
  other.setProxy(proxy('x.example'));
  assert.equal(prefs.user.get(LIST), `0,${other.userContextId}`);
  assertMatches(registry, prefs, 'default + other');
});

test('an event that does not change the state writes nothing', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  const one = newContext('one');
  one.setProxy(proxy('gw.example'));
  prefs.log.length = 0;
  registry.setBrowserProxy(proxy('gw.example', { port: 4 }));
  one.setProxy(proxy('gw.example', { port: 5 }));
  assert.deepEqual(prefs.log, []);
  assertMatches(registry, prefs, 'same state');
});

test('the list is already right when the hold on proxied channels is lifted', () => {
  const { registry, prefs } = session();
  const seen = [];
  registry.onBrowserProxyDecided(() => seen.push(prefs.user.get(LIST)));
  registry.setBrowserProxy(proxy('gw.example'));
  registry.setNoBrowserProxy();
  assert.deepEqual(seen, ['*', undefined]);
});

test('the list is written in the turn that stores the proxy: no lookup can fall between the two', () => {
  const { registry, prefs, newContext } = session();
  registry.setNoBrowserProxy();
  const context = newContext('ctx');
  // At the instant the list is written the registry already answers for the new
  // proxy, and after the call returns the list says the same. Nothing is awaited.
  let atWrite = null;
  prefs.fail.on = (kind, name) => {
    if (kind === 'set' && name === LIST)
      atWrite = registry.proxyFor('site.example', context.userContextId) !== null;
    return false;
  };
  context.setProxy(proxy('c.example'));
  assert.equal(atWrite, true);
  assertMatches(registry, prefs, 'after');
  // The names that a proxied context must not send over DoH are written before the list.
  assert.deepEqual(prefs.log.map(e => e[1]), [ETC_HOSTS, NAMES, LIST]);
});

// A write that throws refuses the command and the registry goes back; the three
// prefs go back with it, whichever one failed.
test('a pref write that throws: the proxy is refused and the list is what the restored registry says', async () => {
  // exclude-etc-hosts is already false by then, so only the two prefs that change are written.
  for (const target of [NAMES, LIST]) {
    const { registry, prefs, newContext } = session();
    registry.setNoBrowserProxy();
    const a = newContext('a');
    a.setProxy(proxy('a.example'));
    registry.setBrowserProxy(proxy('gw.example'));
    const b = newContext('b');
    assertMatches(registry, prefs, `${target}: before`);
    const before = new Map(prefs.user);

    prefs.fail.on = (kind, name) => kind === 'set' && name === target;
    assert.throws(() => b.setProxy(proxy('b.example')), /NS_ERROR_FAILURE/, target);
    assert.equal(b._proxy, null);
    assert.deepEqual(values(prefs.user), values(before), `${target}: prefs rolled back after a refused context proxy`);
    assertMatches(registry, prefs, `${target}: after refused context proxy`);

    // Replacing the launch proxy by another name changes the names, not the list.
    if (target === NAMES) {
      assert.throws(() => registry.setBrowserProxy(proxy('other.example')), /NS_ERROR_FAILURE/, target);
      assert.equal(registry._browserProxy.host, 'gw.example', 'the previous launch proxy is back');
      assert.deepEqual(values(prefs.user), values(before), `${target}: prefs rolled back after a refused launch proxy`);
      assertMatches(registry, prefs, `${target}: after refused launch proxy`);
    }
    prefs.fail.on = null;
    await b.destroy();
  }
});

test('a pref write that throws on the first proxy leaves no list behind', () => {
  for (const target of [ETC_HOSTS, NAMES, LIST]) {
    const { registry, prefs, newContext } = session();
    registry.setNoBrowserProxy();
    const context = newContext('a');
    prefs.fail.on = (kind, name) => kind === 'set' && name === target;
    assert.throws(() => context.setProxy(proxy('a.example')), /NS_ERROR_FAILURE/);
    assert.throws(() => registry.setBrowserProxy(proxy('gw.example')), /NS_ERROR_FAILURE/);
    assert.deepEqual([...prefs.user.keys()].filter(k => k.startsWith('network.trr.') && k !== 'network.trr.uri'), [], target);
    assertMatches(registry, prefs, target);
  }
});

test('a clear that throws while the last proxy leaves: the proxy stays and so does the list', () => {
  for (const target of [ETC_HOSTS, NAMES, LIST]) {
    const { registry, prefs } = session();
    registry.setBrowserProxy(proxy('gw.example'));
    const before = new Map(prefs.user);
    prefs.fail.on = (kind, name) => kind === 'clear' && name === target;
    assert.throws(() => registry.setNoBrowserProxy(), /NS_ERROR_FAILURE/);
    assert.equal(registry._browserProxy.host, 'gw.example', 'the proxy is back');
    assert.deepEqual(values(prefs.user), values(before), `${target}: the list is not left cleared under a live proxy`);
    assertMatches(registry, prefs, target);
  }
});

test('a context proxy that cannot be expressed (or is null) refuses and changes no list', () => {
  const { registry, prefs, newContext } = session();
  registry.setBrowserProxy(proxy('gw.example'));
  const context = newContext('a');
  const before = new Map(prefs.user);
  assert.throws(() => context.setProxy(proxy('x.example', { type: 'refused' })), /NOT_IMPLEMENTED/);
  assert.throws(() => context.setProxy(null));
  assert.equal(context._proxy, null);
  assert.deepEqual(values(prefs.user), values(before));
  assertMatches(registry, prefs, 'refused');
});

test("without the core's declaration no list is written and the command is refused", () => {
  const { registry, prefs, newContext } = session();
  prefs.user.delete('network.trr.uri');
  const context = newContext('a');
  assert.throws(() => context.setProxy(proxy('a.example')), /network\.trr\.uri/);
  assert.throws(() => registry.setBrowserProxy(proxy('gw.example')), /network\.trr\.uri/);
  assert.equal(prefs.user.has(LIST), false);
  assertMatches(registry, prefs, 'refused');
});

// The only places a proxy of a context is stored are the ones that sync.
test('every event that changes a proxy is wired to the one sync, and nothing else stores a context proxy', () => {
  const text = read('TargetRegistry.js');
  const between = (from, to) => text.slice(text.indexOf(from), text.indexOf(to, text.indexOf(from)));
  for (const [from, to] of [
    ['  setBrowserProxy(proxy) {', '  setNoBrowserProxy() {'],
    ['  setNoBrowserProxy() {', '  // The proxy records present'],
    ['  setProxy(proxy) {', '  setIgnoreHTTPSErrors('],
    ['  async destroy() {', '  async setServiceWorkersBlocked('],
  ])
    assert.match(between(from, to), /_syncTrrPrefs\(\)/, from);
  // _proxy is assigned in the constructor (null), in setProxy and in its roll back: nowhere else.
  const assigns = text.match(/\._proxy\s*=[^=]/g) || [];
  assert.equal(assigns.length, 3, `${assigns.length} assignments of ._proxy`);
  assert.match(between('    this._proxy = null;', '    this.removeOnDetach'), /^    this\._proxy = null;/);
  // The carried data is the one the filter reads: userContextId and _proxy of each live context.
  const present = between('  _proxiesPresent() {', '  _syncTrrPrefs() {');
  assert.match(present, /_browserContextIdToBrowserContext\.values\(\)/);
  assert.match(present, /userContextId: bc\.userContextId, proxy: bc\._proxy/);
  assert.match(present, /browserProxy: this\._browserProxy/);
  const filter = between('  proxyRecordFor(userContextId) {', '  defaultContext() {');
  assert.match(filter, /browserContext\._proxy\) \|\| GoneContexts\.recordFor\(userContextId\) \|\| this\._browserProxy/);
});

test('nothing in the Juggler writes network.trr.mode', () => {
  const walk = dir => fs.readdirSync(path.join(ROOT, dir), { withFileTypes: true }).flatMap(e =>
    e.isDirectory() ? (e.name === 'tests' ? [] : walk(path.join(dir, e.name))) : [path.join(dir, e.name)]);
  const files = walk('.').filter(f => /\.(js|jsm|mjs)$/.test(f));
  const mentions = files.filter(f => /network\.trr\.mode/.test(read(f)));
  assert.deepEqual(mentions, ['TrrPrefs.js'], 'only TrrPrefs.js names the pref, to clear a leftover');
  const text = read('TrrPrefs.js');
  // MODE_PREF is read and cleared, never set.
  for (const m of text.matchAll(/\b(\w+)\(([^)]*MODE_PREF[^)]*)\)/g))
    assert.match(m[1], /^(prefHasUserValue|getIntPref|clearUserPref)$/, m[0]);
  assert.doesNotMatch(text, /set(Int|Bool|String)Pref\(\s*MODE_PREF/);
  assert.doesNotMatch(text, /\[\s*MODE_PREF\s*,/, 'not part of the state it writes');
});
