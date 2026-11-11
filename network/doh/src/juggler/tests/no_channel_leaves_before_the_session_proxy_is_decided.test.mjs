// The proxy filter of the session starts PENDING: until the first command has
// decided the road (Browser.setBrowserProxy, or Browser.setNoBrowserProxy for a
// session that has none) it stores the (channel, callback) pairs and answers
// none, so no request - the DoH lookups included, they go through the same
// filter - leaves the host before the client said how it must leave. The real
// NetworkObserver and TargetRegistry run here on stubs.
//
// No secret travels in the process environment: the proxy, credentials
// included, arrives on the pipe.
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
const Ci = {
  nsIProtocolProxyChannelFilter: {},
  nsITransportSecurityInfo: { TLS_VERSION_1: 1, TLS_VERSION_1_1: 2, TLS_VERSION_1_2: 3, TLS_VERSION_1_3: 4 },
};

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

// A proxy this engine cannot express throws when its info is built, as the real
// newProxyInfoFor does for a record the proxy service refuses.
const newProxyInfoFor = proxy => {
  if (proxy.type === 'refused')
    throw new Error('NS_ERROR_NOT_IMPLEMENTED');
  return { proxyInfoOf: proxy.host };
};
const proxyForHost = (record, host) =>
  !record || record.bypass.some(suffix => host.endsWith(suffix)) ? null : record;

const modules = { Helper, newProxyInfoFor, proxyForHost, syncTrrPrefs() {} };

const { TargetRegistry } = load('TargetRegistry.js', {
  Ci, Cc: {}, Components: { results: Cr },
  ChromeUtils: { importESModule: () => modules },
  Services: {},
  NetUtil: {},
  dump: () => {},
}, ['TargetRegistry']);

// A registry with the fields its constructor sets (the constructor is checked
// in the source below), and a NetworkObserver registered on it.
function session() {
  const registry = Object.create(TargetRegistry.prototype);
  registry._browserProxy = null;
  registry._browserProxyDecided = false;
  registry._browserProxyListeners = [];
  registry._proxiesWithClashingAuthCacheKeys = new Set();
  registry._browserContextIdToBrowserContext = new Map();
  registry._userContextIdToBrowserContext = new Map();
  return { registry };
}

// What the warnings write (say() is not the global dump, which is off in an official build).
const said = [];

// NetworkObserver reads the proxy service from its own Cc.
function observe(registry) {
  let registered = null;
  const { NetworkObserver: Observer } = load('NetworkObserver.js', {
    Components: { classes: { '@mozilla.org/network/protocol-proxy-service;1': { getService: () => ({
      registerChannelFilter: f => { registered = f; },
    }) } }, interfaces: Ci, utils: {}, results: Cr, manager: {},
                  Constructor: () => function () {} },
    ChromeUtils: {
      importESModule: () => ({
        ...modules,
        ChannelEventSinkFactory: { getService: () => ({ registerCollector() {} }) },
        NetUtil: {},
        say: text => said.push(text),
      }),
      generateQI: () => () => {},
    },
    Services: { obs: { addObserver() {}, notifyObservers() {} } },
    dump: () => {},
  }, ['NetworkObserver']);
  new Observer(registry);
  assert.ok(registered, 'the filter is registered when the observer starts');
  return registered;
}

const DEFAULT = { default: true };
function ask(filter, host = 'doh.example') {
  const answers = [];
  const channel = { URI: { host }, loadInfo: { originAttributes: { userContextId: 0 } } };
  filter.applyFilter(channel, DEFAULT, { onProxyFilterResult: info => answers.push(info) });
  return answers;
}

test('channels asked before the first command get no answer', () => {
  const { registry } = session();
  const filter = observe(registry);
  const a = ask(filter);
  const b = ask(filter, 'page.example');
  assert.deepEqual([a.length, b.length], [0, 0]);
});

test('Browser.setBrowserProxy answers them with the proxy, and later ones too', () => {
  const { registry } = session();
  const filter = observe(registry);
  const a = ask(filter);
  const b = ask(filter, 'page.example');
  registry.setBrowserProxy({ type: 'socks', host: 'gw', port: 1080, bypass: [] });
  assert.deepEqual(a, [{ proxyInfoOf: 'gw' }]);
  assert.deepEqual(b, [{ proxyInfoOf: 'gw' }]);
  assert.deepEqual(ask(filter), [{ proxyInfoOf: 'gw' }]);
});

test('Browser.setNoBrowserProxy answers them with the default info', () => {
  const { registry } = session();
  const filter = observe(registry);
  const a = ask(filter);
  registry.setNoBrowserProxy();
  assert.deepEqual(a, [DEFAULT]);
  assert.deepEqual(ask(filter), [DEFAULT]);
});

test('a proxy that fails validation leaves them pending, and the decision stays open', () => {
  const { registry } = session();
  const filter = observe(registry);
  const a = ask(filter);
  assert.throws(() => registry.setBrowserProxy({ type: 'refused', host: 'gw', port: 1, bypass: [] }),
      /NS_ERROR_NOT_IMPLEMENTED/);
  assert.equal(a.length, 0);
  assert.equal(ask(filter).length, 0);
  assert.equal(registry.browserProxyDecided, false);
  // A good proxy afterwards decides, and answers every channel that waited.
  registry.setBrowserProxy({ type: 'http', host: 'gw2', port: 3128, bypass: [] });
  assert.deepEqual(a, [{ proxyInfoOf: 'gw2' }]);
});

test('the bypass list still sends a host the default way once decided', () => {
  const { registry } = session();
  const filter = observe(registry);
  const a = ask(filter, 'local.test');
  registry.setBrowserProxy({ type: 'socks', host: 'gw', port: 1080, bypass: ['.test'] });
  assert.deepEqual(a, [DEFAULT]);
});

test('a decision, once made, is changed by Browser.setBrowserProxy as before', () => {
  const { registry } = session();
  const filter = observe(registry);
  registry.setNoBrowserProxy();
  assert.deepEqual(ask(filter), [DEFAULT]);
  registry.setBrowserProxy({ type: 'socks', host: 'gw', port: 1080, bypass: [] });
  assert.deepEqual(ask(filter), [{ proxyInfoOf: 'gw' }]);
  registry.setNoBrowserProxy();
  assert.deepEqual(ask(filter), [DEFAULT]);
});

test('every waiting channel is answered even when one answer throws', () => {
  const { registry } = session();
  const filter = observe(registry);
  const answers = [];
  const channel = { URI: { host: 'a.example' }, loadInfo: { originAttributes: { userContextId: 0 } } };
  filter.applyFilter(channel, DEFAULT, { onProxyFilterResult() { throw new Error('gone'); } });
  filter.applyFilter(channel, DEFAULT, { onProxyFilterResult: info => answers.push(info) });
  said.length = 0;
  registry.setNoBrowserProxy();
  assert.deepEqual(answers, [DEFAULT]);
  // The failed answer is told through say(), not a dump that is off in an official build.
  assert.equal(said.filter(t => /a proxy answer failed: gone/.test(t)).length, 1);
});

test('the registry starts undecided, and the protocol has the command', () => {
  const registry = read('TargetRegistry.js');
  const from = registry.indexOf('  constructor({ survivesLastWindow }) {');
  const ctor = registry.slice(from, registry.indexOf('  // Firefox uses nsHttpAuthCache', from));
  assert.match(ctor, /this\._browserProxyDecided = false;/);
  assert.match(ctor, /this\._browserProxyListeners = \[\];/);
  assert.match(read('protocol/Protocol.js'), /'setNoBrowserProxy': \{\},/);
  assert.match(read('protocol/BrowserHandler.js'),
      /async \['Browser\.setNoBrowserProxy'\]\(\) \{\n\s+this\._targetRegistry\.setNoBrowserProxy\(\);\n\s+\}/);
});

test('the command that decides needs neither Browser.enable nor a network', () => {
  const handler = read('protocol/BrowserHandler.js');
  for (const name of ['setBrowserProxy', 'setNoBrowserProxy']) {
    const start = handler.indexOf(`['Browser.${name}']`);
    const body = handler.slice(start, handler.indexOf('\n  }\n', start));
    assert.doesNotMatch(body, /_enabled|_startCompletePromise|await/, name);
  }
});

test('no secret is read from the process environment, no variable carries the proxy', () => {
  const variable = ['STEALTHFOX', 'SESSION', 'PROXY'].join('_');
  for (const file of fs.readdirSync(ROOT, { recursive: true })) {
    if (file.startsWith('tests') || !/\.(js|mjs|jsm)$/.test(file))
      continue;
    const text = read(file);
    assert.ok(!text.includes(variable), file);
    assert.doesNotMatch(text, /Services\.env\./, file);
  }
});
