// A worker is one object for the client for as long as it lives, also when its document goes into the back-forward
// cache and comes back: same `workerId`, announced again at the restore.
//
// What Firefox does (dom/workers/WorkerPrivate.cpp): freezing the window of the document (Freeze) UNREGISTERS the
// debugger of each of its workers, and thawing it (Thaw, before the pageshow) REGISTERS a new debugger object for the
// same worker, not yet initialized, with the same `id` (the UUID of the WorkerPrivate). FrameTree made a new Worker
// wrapper for the new debugger and so a new `workerId`: the restored worker was another worker for the client
// (measured on the engine: workerCreated with a new id after the restore). The unit tests of a95bbc6808 used a
// tree whose workers never left, and passed.
//
// Here the FrameTree and the PageAgent are the real ones, over a worker debugger manager that does what Firefox's does.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}
const plain = x => JSON.parse(JSON.stringify(x));

// toolkit/modules/EventEmitter.jsm: listeners get the event name first.
class EventEmitter {
  constructor() { this._l = new Map(); }
  on(name, fn) { if (!this._l.has(name)) this._l.set(name, new Set()); this._l.get(name).add(fn); }
  off(name, fn) { this._l.get(name)?.delete(fn); }
  once() {}
  emit(name, ...args) { for (const fn of [...(this._l.get(name) || [])]) fn(name, ...args); }
}

let uuids = 0;
const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({ generateUUID: () => ({ toString: () => `{generated-${++uuids}}` }) }) } },
  Ci: {},
  ChromeUtils: { importESModule: () => ({ EventEmitter }) },
}, ['Helper']);

// The worker debugger manager as Firefox's: a worker has a WorkerPrivate (a stable `id`), a debugger object is made
// each time it is registered, a frozen worker is not registered, a terminated one never comes back.
class WorkerDebuggerManager {
  constructor() { this.listeners = []; this.debuggers = new Set(); this.nextId = 0; }
  addListener(l) { this.listeners.push(l); }
  removeListener(l) { this.listeners = this.listeners.filter(x => x !== l); }
  getWorkerDebuggerEnumerator() { return [...this.debuggers]; }
  start(window, url) {
    const worker = { id: `worker-private-${++this.nextId}`, window, url };
    this.register(worker);
    return worker;
  }
  register(worker) {
    const debuggerObject = {
      id: worker.id, url: worker.url, window: worker.window, type: 0, isInitialized: false,
      initialize() { this.isInitialized = true; },
      addListener() {}, removeListener() {}, postMessage() {},
    };
    worker.debuggerObject = debuggerObject;
    this.debuggers.add(debuggerObject);
    for (const l of [...this.listeners]) l.onRegister(debuggerObject);
  }
  unregister(worker) {
    this.debuggers.delete(worker.debuggerObject);
    for (const l of [...this.listeners]) l.onUnregister(worker.debuggerObject);
  }
  freeze(worker) { this.unregister(worker); }     // WorkerPrivate::Freeze: DisableDebugger
  thaw(worker) { this.register(worker); }         // WorkerPrivate::Thaw: EnableDebugger, a new debugger object
  terminate(worker) { this.unregister(worker); }
}

const runtime = new Proxy({}, {
  get: (_, name) => name === 'executionContexts' ? () => [] : name === 'events' ? new Proxy({}, { get: () => () => () => {} }) : function () {},
});

function loadModules(manager) {
  const makeScope = () => ({
    Components: {
      interfaces: { nsIWebProgress: { NOTIFY_STATE_DOCUMENT: 1, NOTIFY_LOCATION: 2 }, nsIWorkerDebugger: { TYPE_DEDICATED: 0 } },
      results: {}, utils: {},
    },
    Cc: new Proxy({
      '@mozilla.org/dom/workers/workerdebuggermanager;1': { getService: () => manager },
      '@mozilla.org/websocketevent/service;1': { getService: () => ({}) },
    }, { get: (t, k) => t[k] || { getService: () => ({ addObserver() {}, removeObserver() {} }) } }),
    ChromeUtils: {
      generateQI: () => () => {},
      importESModule: url => url.endsWith('Helper.js') ? { Helper: class extends Helper { addObserver() { return () => {}; } addProgressListener() { return () => {}; } } }
        : { NetUtil: {}, setTimeout },
    },
    Services: { obs: { addObserver() {}, removeObserver() {} } },
    Runtime: class { constructor() { return runtime; } },
    SimpleChannel: class { constructor() {} setTransport() {} connect() { return { dispose() {}, emit() {}, send() {} }; } register() { return () => {}; } dispose() {} _onMessage() {} },
    dump: () => {},
  });
  const { FrameTree } = load('content/FrameTree.js', makeScope(), ['FrameTree']);
  const { PageAgent } = load('content/PageAgent.js', makeScope(), ['PageAgent']);
  return { FrameTree, PageAgent };
}

// A root with one frame: a real FrameTree and a real PageAgent on it. `said` is what the agents say on 'page'.
function root() {
  const manager = new WorkerDebuggerManager();
  const { FrameTree, PageAgent } = loadModules(manager);
  const docShell = {};
  const window = { docShell };
  const frame = { id: () => 'mainframe-1', docShell: () => docShell, url: () => 'http://a.test/', name: () => '',
    pendingNavigationId: () => null, lastCommittedNavigationId: () => null, domWindow: () => window, dispose() {}, parentFrame: () => null, _children: new Set(), _browsingContext: { parent: null } };
  FrameTree.prototype._createFrame = function () { this._frameIdToFrame.set('mainframe-1', frame); };
  FrameTree.prototype.frameForBrowsingContext = () => frame;
  FrameTree.prototype.frameForDocShell = ds => (ds === docShell ? frame : null);
  const bc = { group: {}, children: [], docShell: { QueryInterface: () => ({ getInterface: () => ({}) }) } };
  const tree = new FrameTree(bc);
  const said = [];
  const channel = {
    connect: ns => ({ emit: (method, params) => said.push([ns, method, plain(params)]), dispose() {} }),
    register: () => () => {},
  };
  const agent = new PageAgent(channel, tree);
  const workers = () => said.filter(([ns, m]) => ns === 'page' && /^pageWorker/.test(m)).map(([, m, p]) => `${m}:${p.workerId}`);
  return { manager, tree, agent, window, workers, said };
}

// The document goes into the cache: its pagehide (JugglerFrameChild), then its window is frozen.
function intoTheCache(r, ...workers) {
  r.agent.enterCache();
  for (const w of workers) r.manager.freeze(w);
}
// It comes back: the window is thawed, then the pageshow reattaches the channel (main.js: onReattached).
function outOfTheCache(r, ...workers) {
  for (const w of workers) r.manager.thaw(w);
  r.agent.leaveCache();
}

test('a worker that comes back from the cache is announced again under the id it had', () => {
  const r = root();
  const worker = r.manager.start(r.window, 'http://a.test/worker.js');
  const [created] = r.workers();
  assert.match(created, /^pageWorkerCreated:/);
  const id = created.split(':')[1];

  intoTheCache(r, worker);
  outOfTheCache(r, worker);

  assert.deepEqual(r.workers(), [`pageWorkerCreated:${id}`, `pageWorkerDestroyed:${id}`, `pageWorkerCreated:${id}`]);
});

test('the restored worker is the one the agent follows: its data is rebuilt on the new debugger, under the same id', () => {
  const r = root();
  const worker = r.manager.start(r.window, 'http://a.test/worker.js');
  const [id] = [...r.agent._workerData.keys()];
  intoTheCache(r, worker);
  outOfTheCache(r, worker);
  assert.deepEqual([...r.agent._workerData.keys()], [id]);
  assert.equal(r.tree.workers().length, 1);
  assert.equal(r.tree.workers()[0].id(), id);
});

test('two workers of one document, with one url, each come back under its own id', () => {
  const r = root();
  const first = r.manager.start(r.window, 'http://a.test/worker.js');
  const second = r.manager.start(r.window, 'http://a.test/worker.js');
  const ids = r.workers().map(w => w.split(':')[1]);
  assert.equal(new Set(ids).size, 2);
  intoTheCache(r, first, second);
  // the thaw order is not the freeze order
  outOfTheCache(r, second, first);
  const again = r.workers().slice(-2).map(w => w.split(':')[1]).sort();
  assert.deepEqual(again, [...ids].sort());
  assert.deepEqual(plain(r.tree.workers().map(w => w.id())).sort(), [...ids].sort());
});

test('the id is kept whichever of the thaw and the reattachment comes first', () => {
  const r = root();
  const worker = r.manager.start(r.window, 'http://a.test/worker.js');
  const id = r.workers()[0].split(':')[1];
  intoTheCache(r, worker);
  r.agent.leaveCache();           // the reattachment first: nothing to say yet
  assert.deepEqual(r.workers(), [`pageWorkerCreated:${id}`, `pageWorkerDestroyed:${id}`]);
  r.manager.thaw(worker);         // the thaw after: announced at once, under its id
  assert.deepEqual(r.workers().slice(2), [`pageWorkerCreated:${id}`]);
});

test('a worker that ends for good gets no id back: a new worker with the same url is a new worker', () => {
  const r = root();
  const worker = r.manager.start(r.window, 'http://a.test/worker.js');
  const id = r.workers()[0].split(':')[1];
  r.manager.terminate(worker);    // not in the cache: it ends
  const other = r.manager.start(r.window, 'http://a.test/worker.js');
  const ids = r.workers();
  assert.equal(ids[1], `pageWorkerDestroyed:${id}`);
  assert.notEqual(ids[2].split(':')[1], id);
  assert.ok(other);
});

test('a worker that ended in the cache does not come back, and a later worker does not take its id', () => {
  const r = root();
  const worker = r.manager.start(r.window, 'http://a.test/worker.js');
  const id = r.workers()[0].split(':')[1];
  intoTheCache(r, worker);
  r.agent.leaveCache();           // the worker never came back (its window was thawed without it)
  const other = r.manager.start(r.window, 'http://a.test/worker.js');
  const last = r.workers().at(-1);
  assert.match(last, /^pageWorkerCreated:/);
  assert.notEqual(last.split(':')[1], id);
  intoTheCache(r, other);         // the next stay in the cache forgets what did not come back
  assert.equal(r.tree._parkedWorkerIds.size, 1);
});
