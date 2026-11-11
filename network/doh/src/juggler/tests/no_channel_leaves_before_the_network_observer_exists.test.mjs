// A driven browser holds its proxied channels from the first point where the
// command line is handled, not from final-ui-startup.
//
// The NetworkObserver (and so the pending proxy filter of the session) is built
// at final-ui-startup. A request made between the start of the browser and that
// point had no filter and went DIRECT, even in a session started with the pipe.
// Juggler now registers a HOLD filter at command-line-startup, the first point
// where it sees the pipe flag, and the NetworkObserver adopts it: the same
// object is registered once for the whole life of the browser, the channels it
// stored are not lost and not answered early, and the decision of the registry
// answers them in order. Without the flag nothing is registered.
//
// The real Juggler.js, EarlyProxyHold.js, NetworkObserver.js and TargetRegistry.js
// run here on stubs.
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

const newProxyInfoFor = proxy => {
  if (proxy.type === 'refused')
    throw new Error('NS_ERROR_NOT_IMPLEMENTED');
  return { proxyInfoOf: proxy.host };
};
const proxyForHost = (record, host) =>
  !record || record.bypass.some(suffix => host.endsWith(suffix)) ? null : record;

const DEFAULT = { default: true };

// One browser: the real files, loaded fresh so no static state is shared.
// `options.flag`: the pipe flag is on the command line.
// `options.failFirstRegistration`: the proxy service refuses the first filter.
function browser({ flag = true, failFirstRegistration = false } = {}) {
  const registered = [];      // every filter handed to the proxy service
  let refusals = failFirstRegistration ? 1 : 0;
  const observers = new Map();  // topic -> observer
  const components = {
    classes: {
      '@mozilla.org/network/protocol-proxy-service;1': { getService: () => ({
        registerChannelFilter: f => {
          if (refusals-- > 0)
            throw new Error('NS_ERROR_FAILURE');
          registered.push(f);
        },
      }) },
      '@mozilla.org/gfx/info;1': { getService: () => ({ isHeadless: false }) },
      '@mozilla.org/juggler/remotedebuggingpipe;1': { getService: () => ({ init() {}, stop() {}, sendMessage() {} }) },
    },
    interfaces: Ci, utils: {}, results: Cr, manager: {},
    Constructor: () => function () {},
  };
  const services = {
    obs: {
      addObserver: (o, topic) => observers.set(topic, o),
      removeObserver: (o, topic) => observers.delete(topic),
      notifyObservers() {},
    },
    scriptloader: { loadSubScript() {} },
    appShell: { hasHiddenWindow: false },
    prefs: {
      prefHasUserValue: () => false,
      getBoolPref: (name, dflt) => dflt,
      setBoolPref() {},
      clearUserPref() {},
    },
    startup: {},
  };
  const dumped = [];
  const dump = text => dumped.push(text);

  const { TargetRegistry: RealRegistry } = load('TargetRegistry.js', {
    Ci, Cc: {}, Components: { results: Cr },
    ChromeUtils: { importESModule: () => ({ Helper, newProxyInfoFor, proxyForHost }) },
    Services: {}, NetUtil: {}, dump,
  }, ['TargetRegistry']);
  // The registry as the constructor leaves it (the constructor is checked in
  // no_channel_leaves_before_the_session_proxy_is_decided.test.mjs).
  const TargetRegistry = function() {
    const registry = Object.create(RealRegistry.prototype);
    registry._browserProxy = null;
    registry._browserProxyDecided = false;
    registry._browserProxyListeners = [];
    registry._proxiesWithClashingAuthCacheKeys = new Set();
    registry._browserContextIdToBrowserContext = new Map();
    registry._userContextIdToBrowserContext = new Map();
    TargetRegistry.created.push(registry);
    return registry;
  };
  TargetRegistry.created = [];
  TargetRegistry.markDriven = () => RealRegistry.markDriven();

  const observerModules = { Helper, newProxyInfoFor, proxyForHost };
  const { NetworkObserver } = load('NetworkObserver.js', {
    Components: components,
    ChromeUtils: {
      importESModule: () => ({
        ...observerModules,
        ChannelEventSinkFactory: { getService: () => ({ registerCollector() {} }) },
        NetUtil: {},
      }),
      generateQI: () => () => {},
    },
    Services: services, dump,
  }, ['NetworkObserver']);

  const { EarlyProxyHold } = load('EarlyProxyHold.js', {
    Components: components,
    ChromeUtils: { generateQI: () => () => {} },
    Services: services, dump,
  }, ['EarlyProxyHold']);

  class Dispatcher {
    rootSession() { return { setHandler() {} }; }
  }
  class BrowserHandler {}
  const byUrl = {
    'protocol/Dispatcher.js': { Dispatcher },
    'protocol/BrowserHandler.js': { BrowserHandler },
    'NetworkObserver.js': { NetworkObserver },
    'TargetRegistry.js': { TargetRegistry },
    'EarlyProxyHold.js': { EarlyProxyHold },
    'Helper.js': { Helper },
    'ActorManagerParent.sys.mjs': { ActorManagerParent: { addJSWindowActors() {} } },
  };
  const { Juggler } = load('components/Juggler.js', {
    Components: components, Services: services, dump,
    ChromeUtils: {
      importESModule: url => {
        const key = Object.keys(byUrl).find(k => url.endsWith('/' + k));
        assert.ok(key, 'unexpected import ' + url);
        return byUrl[key];
      },
      generateQI: () => () => {},
    },
  }, ['Juggler']);

  const juggler = new Juggler();
  const cmdLine = {
    handleFlag: name => (name === 'juggler-pipe' ? flag : false),
    findFlag: () => -1,
  };
  const log = [];
  const ask = label => {
    const answers = [];
    const channel = { URI: { host: label + '.example' }, loadInfo: { originAttributes: { userContextId: 0 } } };
    // The filter the proxy service calls: the first one registered.
    registered[0].applyFilter(channel, DEFAULT, {
      onProxyFilterResult: info => { answers.push(info); log.push([label, info]); },
    });
    return answers;
  };
  return {
    juggler, cmdLine, registered, observers, log, ask, dumped, TargetRegistry, EarlyProxyHold,
    start: async () => {
      await juggler.observe(null, 'profile-after-change');
      await juggler.observe(cmdLine, 'command-line-startup');
    },
    finalUi: () => juggler.observe(null, 'final-ui-startup'),
    registry: () => TargetRegistry.created[0],
  };
}

test('with the pipe flag the hold is registered at command-line-startup, before the registry exists', async () => {
  const b = browser();
  assert.equal(b.registered.length, 0, 'nothing before the command line');
  await b.start();
  assert.equal(b.registered.length, 1);
  assert.equal(b.registry(), undefined, 'the registry and the observer are not built yet');
  const a = b.ask('early');
  assert.deepEqual(a, [], 'a channel asked before the registry exists is held');
});

test('the NetworkObserver takes the hold over: still one registration, nothing answered early', async () => {
  const b = browser();
  await b.start();
  const first = b.ask('first');
  const second = b.ask('second');
  await b.finalUi();
  assert.ok(b.registry(), 'the registry is built at final-ui-startup');
  assert.equal(b.registered.length, 1, 'registerChannelFilter is called once for the whole life');
  assert.deepEqual([first.length, second.length], [0, 0], 'the held channels are still held');
  const third = b.ask('third');
  assert.deepEqual(third, [], 'a channel asked after the hand-over is held too');
  assert.deepEqual(b.log, []);
});

test('the decision answers the channels that waited, in the order they asked', async () => {
  const b = browser();
  await b.start();
  b.ask('first');
  b.ask('second');
  await b.finalUi();
  b.ask('third');
  b.registry().setBrowserProxy({ type: 'socks', host: 'gw', port: 1080, bypass: [] });
  assert.deepEqual(b.log.map(([label]) => label), ['first', 'second', 'third']);
  assert.deepEqual(b.log.map(([, info]) => info), Array(3).fill({ proxyInfoOf: 'gw' }));
  // Decided: the same filter now answers at once.
  assert.deepEqual(b.ask('late'), [{ proxyInfoOf: 'gw' }]);
  assert.equal(b.registered.length, 1);
});

test('setNoBrowserProxy answers them with the default info', async () => {
  const b = browser();
  await b.start();
  const early = b.ask('early');
  await b.finalUi();
  b.registry().setNoBrowserProxy();
  assert.deepEqual(early, [DEFAULT]);
  assert.deepEqual(b.ask('late'), [DEFAULT]);
});

test('a proxy that fails validation leaves the early channels held until a good one decides', async () => {
  const b = browser();
  await b.start();
  const early = b.ask('early');
  await b.finalUi();
  assert.throws(() => b.registry().setBrowserProxy({ type: 'refused', host: 'gw', port: 1, bypass: [] }),
      /NS_ERROR_NOT_IMPLEMENTED/);
  assert.deepEqual(early, []);
  assert.equal(b.registry().browserProxyDecided, false);
  b.registry().setBrowserProxy({ type: 'http', host: 'gw2', port: 3128, bypass: [] });
  assert.deepEqual(early, [{ proxyInfoOf: 'gw2' }]);
});

test('without the pipe flag nothing is registered and no later topic is observed', async () => {
  const b = browser({ flag: false });
  await b.start();
  assert.equal(b.registered.length, 0);
  assert.equal(b.observers.has('final-ui-startup'), false);
  assert.equal(b.registry(), undefined);
});

test('a hold that could not be registered early is registered by the NetworkObserver, once', async () => {
  const b = browser({ failFirstRegistration: true });
  await b.start();
  assert.equal(b.registered.length, 0);
  assert.match(b.dumped.join(''), /early proxy hold/);
  await b.finalUi();
  assert.equal(b.registered.length, 1);
  const a = b.ask('late');
  assert.deepEqual(a, [], 'the fallback filter is pending too');
  b.registry().setNoBrowserProxy();
  assert.deepEqual(a, [DEFAULT]);
});

test('the hold registers its filter once however often it is asked', async () => {
  const b = browser();
  const one = b.EarlyProxyHold.register();
  const two = b.EarlyProxyHold.register();
  assert.equal(one, two);
  assert.equal(b.registered.length, 1);
});

test('source: the hook runs at command-line-startup, after the flag is known, and nowhere before', () => {
  const juggler = read('components/Juggler.js');
  const start = juggler.indexOf('case "command-line-startup":');
  const end = juggler.indexOf('case "browser-idle-startup-tasks-finished":');
  const topic = juggler.slice(start, end);
  const flag = topic.indexOf("handleFlag('juggler-pipe', false)");
  const guard = topic.indexOf('if (!jugglerPipeFlag)');
  const hold = topic.indexOf('EarlyProxyHold.register()');
  assert.ok(flag >= 0 && guard > flag && hold > guard, 'registered after the flag check');
  const afterChange = juggler.slice(juggler.indexOf('case "profile-after-change":'), start);
  assert.doesNotMatch(afterChange, /EarlyProxyHold/);
  assert.match(juggler, /new NetworkObserver\(targetRegistry, this\._earlyHold\)/);
  assert.doesNotMatch(juggler, /registerChannelFilter/);
});

test('source: the observer registers a filter only when it was not handed the hold, and keeps its pairs', () => {
  const observer = read('NetworkObserver.js');
  assert.match(observer, /constructor\(targetRegistry, earlyHold = null\)/);
  assert.match(observer, /this\._pendingProxyFilters = earlyHold \? earlyHold\.pending : \[\];/);
  assert.equal(observer.match(/registerChannelFilter\(/g).length, 1);
  assert.match(observer, /if \(earlyHold\)\s+earlyHold\.adopt\(/);
});

test('source: the hold file is ours, listed in moz.build, and asks only for what is there at that topic', () => {
  assert.match(read('moz.build'), /"EarlyProxyHold\.js",/);
  const hold = read('EarlyProxyHold.js');
  // Only the proxy service (needs prefs and the observer service) is used.
  const services = Array.from(hold.matchAll(/Cc\[['"]([^'"]+)['"]\]/g), m => m[1]);
  assert.deepEqual(services, ['@mozilla.org/network/protocol-proxy-service;1']);
  assert.doesNotMatch(hold, /TargetRegistry|importESModule/);
});

// A driver that sends neither Browser.setBrowserProxy nor
// Browser.setNoBrowserProxy would hang without a word: one line says why.
const WAITING = 'juggler: waiting for Browser.setBrowserProxy or Browser.setNoBrowserProxy before the first proxied channel can start\n';
const waitingLines = b => b.dumped.filter(text => /waiting for Browser\.setBrowserProxy/.test(text));

test('the first held channel writes one line, however many are held', async () => {
  const b = browser();
  await b.start();
  assert.deepEqual(waitingLines(b), [], 'nothing before a channel is held');
  for (let i = 0; i < 5; i++)
    b.ask('early' + i);
  assert.deepEqual(waitingLines(b), [WAITING]);
  await b.finalUi();
  for (let i = 0; i < 5; i++)
    b.ask('late' + i);
  assert.deepEqual(waitingLines(b), [WAITING], 'still one line after the observer took over');
  assert.ok(!b.dumped.join('').includes('.example'), 'no URL or host is printed');
});

test('a hold that the observer registered itself writes the line once too', async () => {
  const b = browser({ failFirstRegistration: true });
  await b.start();
  await b.finalUi();
  for (let i = 0; i < 3; i++)
    b.ask('late' + i);
  assert.deepEqual(waitingLines(b), [WAITING]);
});

test('no line when the decision already exists', async () => {
  const b = browser();
  await b.start();
  await b.finalUi();
  b.registry().setNoBrowserProxy();
  for (let i = 0; i < 3; i++)
    b.ask('late' + i);
  assert.deepEqual(waitingLines(b), []);
});

test('a decision that comes before any channel is held writes no line, and a proxy neither', async () => {
  const b = browser();
  await b.start();
  await b.finalUi();
  b.registry().setBrowserProxy({ type: 'http', host: 'gw', port: 3128, username: 'u', password: 'secret', bypass: [] });
  b.ask('late');
  assert.deepEqual(waitingLines(b), []);
  assert.ok(!b.dumped.join('').includes('secret'));
});

test('source: the line is written by the hold and, for the fallback, by the observer, with the same words', () => {
  const hold = read('EarlyProxyHold.js');
  const observer = read('NetworkObserver.js');
  const line = /'(juggler: waiting for Browser\.setBrowserProxy or Browser\.setNoBrowserProxy before the first proxied channel can start)\\n'/;
  assert.equal(hold.match(line)[1], observer.match(line)[1]);
});
