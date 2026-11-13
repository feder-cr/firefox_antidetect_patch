// A session whose proxy was never decided sends NOTHING direct, at exit too.
//
// The channel filter of the browser starts pending (see
// no_channel_leaves_before_the_session_proxy_is_decided.test.mjs), but the
// pingsender background task that the browser starts at exit is another
// process: it has its own filter, built from the record the parent hands it.
// `proxyRecordFor` answers null while the session is undecided, and a null
// record is the DEFAULT road: the shutdown pings of a session that never said
// how it leaves would go out from the host's own address.
//
// So the parent asks `TargetRegistry.roadOfSession` and does not start the
// task when the road is undecided. The real TargetRegistry.js and
// StealthPingsender.sys.mjs run here on stubs.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const TREE = path.join(ROOT, '..');
const read = rel => fs.readFileSync(path.join(ROOT, rel), 'utf8');

const Cr = { NS_BINDING_ABORTED: 0x804b0002, NS_ERROR_FAILURE: 0x80004005 };
const Ci = { nsIFile: 'nsIFile' };

class EventEmitter {
  on() {}
  off() {}
  once() {}
  emit() {}
}

function load(file, scope, names) {
  const source = read(file).replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Services: { obs: { addObserver() {}, removeObserver() {} } },
  Ci, Cr,
  ChromeUtils: { importESModule: () => ({ EventEmitter }) },
}, ['Helper']);

const newProxyInfoFor = proxy => ({ proxyInfoOf: proxy.host });
const proxyForHost = (record, host) =>
  !record || record.bypass.some(suffix => host.endsWith(suffix)) ? null : record;
const modules = { Helper, newProxyInfoFor, proxyForHost, syncTrrPrefs() {}, GoneContexts: { recordFor: () => null } };

// A fresh TargetRegistry class, so the static state of one test is not the next one's.
function freshClass() {
  return load('TargetRegistry.js', {
    Ci, Cc: {}, Components: { results: Cr },
    ChromeUtils: { importESModule: () => modules },
    Services: {},
    NetUtil: {},
    dump: () => {},
  }, ['TargetRegistry']).TargetRegistry;
}

// A registry with the fields its constructor sets, registered as THE instance
// the way the constructor does.
function instance(TargetRegistry) {
  const registry = Object.create(TargetRegistry.prototype);
  registry._browserProxy = null;
  registry._browserProxyDecided = false;
  registry._browserProxyListeners = [];
  registry._proxiesWithClashingAuthCacheKeys = new Set();
  registry._browserContextIdToBrowserContext = new Map();
  registry._userContextIdToBrowserContext = new Map();
  TargetRegistry._instance = registry;
  return registry;
}

const PROXY = { type: 'socks', host: 'gw', port: 1080, bypass: [] };

// ---- the road of the session, as the parent asks it ----

test('a browser that Juggler does not drive has no proxy to wait for: its road is direct', () => {
  const TargetRegistry = freshClass();
  assert.deepEqual({ ...TargetRegistry.roadOfSession(0) }, { decided: true, proxy: null });
});

test('a driven browser whose registry does not exist yet is undecided', () => {
  const TargetRegistry = freshClass();
  TargetRegistry.markDriven();
  assert.equal(TargetRegistry.roadOfSession(0).decided, false);
});

test('an undecided registry is undecided, even with a context proxy set', () => {
  const TargetRegistry = freshClass();
  TargetRegistry.markDriven();
  const registry = instance(TargetRegistry);
  assert.equal(TargetRegistry.roadOfSession(0).decided, false);
  registry._browserProxy = PROXY;
  assert.equal(TargetRegistry.roadOfSession(0).decided, false,
      'a stored record that no command decided is not a decision');
});

test('Browser.setBrowserProxy decides: the road carries the record', () => {
  const TargetRegistry = freshClass();
  TargetRegistry.markDriven();
  const registry = instance(TargetRegistry);
  registry.setBrowserProxy(PROXY);
  const road = TargetRegistry.roadOfSession(0);
  assert.equal(road.decided, true);
  assert.equal(road.proxy, PROXY);
});

test('Browser.setNoBrowserProxy decides: the road is direct on purpose', () => {
  const TargetRegistry = freshClass();
  TargetRegistry.markDriven();
  const registry = instance(TargetRegistry);
  registry.setNoBrowserProxy();
  assert.deepEqual({ ...TargetRegistry.roadOfSession(0) }, { decided: true, proxy: null });
});

// ---- the pingsender of the parent, on the real own file ----

const APP = path.join(TREE, 'toolkit/components/telemetry/app');

function loadPingsender(TargetRegistry, log) {
  let src = fs.readFileSync(path.join(APP, 'StealthPingsender.sys.mjs'), 'utf8');
  src = src.replace(/^import \{ AppConstants \}.*$/m, 'const AppConstants = __AppConstants;');
  src = src.replace(/^export /gm, '');
  src += '\nmodule.exports = { runAsync };\n';
  const sandbox = {
    __AppConstants: { MOZ_APP_NAME: 'firefox', platform: 'win' },
    module: { exports: {} },
    Ci,
    Services: {
      env: { set: (k, v) => log.push(['env', k, v]), get: () => '' },
      dirsvc: { get: () => ({ append() {}, path: 'P/prefs.js' }) },
    },
    Cu: { isESModuleLoaded: () => !!TargetRegistry },
    ChromeUtils: { importESModule: () => ({ TargetRegistry }) },
  };
  vm.runInNewContext(src, sandbox);
  return sandbox.module.exports;
}

function start(TargetRegistry) {
  const log = [];
  const observed = [];
  const process = { runAsync(params) { log.push(['run', params.length]); } };
  const observer = { observe: (subject, topic) => observed.push(topic) };
  loadPingsender(TargetRegistry, log).runAsync(process, ['--backgroundtask', 'pingsender'], observer);
  return { log, observed };
}

test('undecided: the pingsender is not started and the road is never put in the environment', () => {
  const TargetRegistry = freshClass();
  TargetRegistry.markDriven();
  instance(TargetRegistry);
  const { log, observed } = start(TargetRegistry);
  assert.deepEqual(log, [], 'no process, no environment variable');
  assert.deepEqual(observed, ['process-failed'], 'the caller is told it did not go out');
});

test('undecided and no registry yet: the pingsender is not started either', () => {
  const TargetRegistry = freshClass();
  TargetRegistry.markDriven();
  assert.deepEqual(start(TargetRegistry).log, []);
});

test('undecided without an observer: nothing is started and nothing throws', () => {
  const TargetRegistry = freshClass();
  TargetRegistry.markDriven();
  instance(TargetRegistry);
  const log = [];
  loadPingsender(TargetRegistry, log).runAsync({ runAsync() { log.push(['run']); } }, [], undefined);
  assert.deepEqual(log, []);
});

test('decided with a proxy: the child starts and carries the record', () => {
  const TargetRegistry = freshClass();
  TargetRegistry.markDriven();
  instance(TargetRegistry).setBrowserProxy(PROXY);
  const { log, observed } = start(TargetRegistry);
  assert.deepEqual(log.map(e => e[0]), ['env', 'run', 'env']);
  assert.deepEqual(JSON.parse(log[0][2]).proxy, PROXY);
  assert.deepEqual(observed, []);
});

test('decided as no proxy: the child starts with a null record, direct on purpose', () => {
  const TargetRegistry = freshClass();
  TargetRegistry.markDriven();
  instance(TargetRegistry).setNoBrowserProxy();
  const { log } = start(TargetRegistry);
  assert.deepEqual(log.map(e => e[0]), ['env', 'run', 'env']);
  assert.equal(JSON.parse(log[0][2]).proxy, null);
});

test('a browser Juggler does not drive keeps its pings (direct is its road)', () => {
  const TargetRegistry = freshClass();
  const { log } = start(TargetRegistry);
  assert.deepEqual(log.map(e => e[0]), ['env', 'run', 'env']);
  assert.equal(JSON.parse(log[0][2]).proxy, null);
});

// ---- the hooks ----

test('Juggler marks the browser as driven as soon as it sees the pipe flag', () => {
  const text = read('components/Juggler.js');
  const flag = text.indexOf("cmdLine.handleFlag('juggler-pipe', false)");
  const bail = text.indexOf('if (!jugglerPipeFlag)', flag);
  const mark = text.indexOf('TargetRegistry.markDriven();', bail);
  const created = text.indexOf('new TargetRegistry(');
  assert.ok(flag > 0 && bail > flag, 'the pipe flag check moved');
  assert.ok(mark > bail && mark < created,
      'markDriven runs after the flag check and before the registry exists');
});

test('the pingsender asks the registry for the road and keeps the CRLF of its file', () => {
  const bytes = fs.readFileSync(path.join(APP, 'StealthPingsender.sys.mjs'), 'utf8');
  assert.match(bytes, /roadOfSession\(0\)/);
  assert.doesNotMatch(bytes, /proxyRecordFor/);
  assert.ok(!/[^\r]\n/.test(bytes), 'every line ending of this file is CRLF');
});
