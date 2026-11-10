// A worker belongs to the CURRENT document of its page (the model of the client). When the document goes into the
// back-forward cache (its `pagehide` with `persisted`) its workers end for the client; when the document comes back
// they are announced again, with the same ids and WITHOUT a `workerDestroyed` more.
//
// Before: nothing was said at the pagehide (the worker lives on, frozen, in the cache), and the restore said
// `runtimeExecutionContextsCleared` (the mux ends the workers the root announced: a `workerDestroyed`) and at once
// `pageWorkerCreated` for the same id: both at the restore. Each test below fails on bdf02eb9d8.
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
const settle = async (n = 8) => { for (let i = 0; i < n; i++) await new Promise(r => setTimeout(r, 0)); };

const { SimpleChannel } = load('SimpleChannel.js', { dump: () => {} }, ['SimpleChannel']);
const { ContentMux } = load('ContentMux.js', {}, ['ContentMux']);
const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } }, Ci: {}, ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);
const { JugglerFrameChild } = load('content/JugglerFrameChild.sys.mjs', {
  Components: { interfaces: {} },
  ChromeUtils: { importESModule: url => (url.endsWith('Helper.js') ? { Helper } : { initialize: () => ({}) }) },
  JSWindowActorChild: class {},
}, ['JugglerFrameChild']);

// The agents of a root, as the content process builds them. `channel` is the channel they speak on.
function pageAgentOn(channel, { workers = [], isPageMain = true } = {}) {
  const scope = { Components: { interfaces: {}, results: {}, utils: {} }, dump: () => {},
    Cc: new Proxy({}, { get: () => ({ getService: () => ({ addObserver() {}, removeObserver() {} }) }) }) };
  scope.ChromeUtils = { importESModule: url => url.endsWith('Helper.js') ? { Helper: class extends Helper { addObserver() { return () => {}; } } }
      : { NetUtil: {}, setTimeout } };
  const { PageAgent } = load('content/PageAgent.js', scope, ['PageAgent']);
  const workerChannel = { connect: () => ({ dispose() {} }), register: () => () => {} };
  const main = { id: () => 'mainframe-1', docShell: () => ({}), url: () => 'about:blank', name: () => '', pendingNavigationId: () => null,
    lastCommittedNavigationId: () => null, domWindow: () => null };
  const runtime = new Proxy({}, { get: (_, name) => name === 'executionContexts' ? () => [] : name === 'events' ? new Proxy({}, { get: () => () => () => {} }) : function () {} });
  const live = new Map(workers.map(id => [id, { id: () => id, url: () => 'w.js', frame: () => main, channel: () => workerChannel }]));
  const handlers = {};
  const tree = { runtime: () => runtime, mainFrame: () => main, frames: () => [], isPageReady: () => false,
    workers: () => [...live.values()], _isPageMain: () => isPageMain,
    on(name, listener) { handlers[name] = listener; }, off() {} };
  const agent = new PageAgent(channel, tree);
  return { agent, tree, main, live, handlers };
}

// A channel that records what the agents say on 'page'.
function recordingChannel() {
  const said = [];
  return { said, channel: { connect: ns => ({ emit: (method, params) => said.push([ns, method, plain(params)]), dispose() {} }), register: () => () => {} } };
}
const workerEvents = said => said.filter(([ns, m]) => ns === 'page' && /^pageWorker/.test(m)).map(([, m, p]) => `${m}:${p.workerId}`);

test('a document that goes into the cache ends the workers of its root, once', () => {
  const { said, channel } = recordingChannel();
  const { agent } = pageAgentOn(channel, { workers: ['w1', 'w2'] });
  said.length = 0;
  agent.enterCache();
  agent.enterCache();
  assert.deepEqual(workerEvents(said), ['pageWorkerDestroyed:w1', 'pageWorkerDestroyed:w2']);
});

test('the pagehide of a document that is persisted ends the workers BEFORE the channel detaches; the others do not', () => {
  const order = [];
  const frame = () => {
    const document = {};
    const actor = new JugglerFrameChild();
    const agents = {
      channel: { ready: true, isReady() { return this.ready; }, detachFromPeer() { order.push('detach'); } },
      pageAgent: { enterCache() { order.push('enterCache'); } },
    };
    actor._agents = () => agents;
    actor.sendAsyncMessage = () => {};
    Object.defineProperty(actor, 'browsingContext', { value: { parent: null } });
    Object.defineProperty(actor, 'document', { value: document });
    return { actor, agents, document };
  };
  const a = frame();
  a.actor.handleEvent({ type: 'pagehide', persisted: true, target: a.document });
  assert.deepEqual(order, ['enterCache', 'detach'], 'what is said after the detach waits for the restore');
  order.length = 0;
  const b = frame();
  b.actor.handleEvent({ type: 'pagehide', persisted: false, target: b.document });
  b.actor.handleEvent({ type: 'pagehide', persisted: true, target: {} });
  assert.deepEqual(order, [], 'not the cache');
  const c = frame();
  c.agents.channel.ready = false;     // a channel that is not attached is not reattached: nothing is ended for good
  c.actor.handleEvent({ type: 'pagehide', persisted: true, target: c.document });
  assert.deepEqual(order, ['detach']);
});

test('the restore of the main document says the workers again without ending them a second time', () => {
  const { said, channel } = recordingChannel();
  const { agent, main } = pageAgentOn(channel, { workers: ['w1'] });
  const data = agent._workerData.get('w1');
  agent.enterCache();
  said.length = 0;
  agent._onDocumentRestored(main);
  assert.deepEqual(workerEvents(said), ['pageWorkerCreated:w1']);
  assert.equal(agent._workerData.get('w1'), data, 'the worker keeps the WorkerData it had');
  // the reattachment hook runs after the restore: nothing more
  said.length = 0;
  agent.leaveCache();
  assert.deepEqual(workerEvents(said), []);
});

test('a root that restores no main document says its workers again at the reattachment', () => {
  const { said, channel } = recordingChannel();
  const { agent } = pageAgentOn(channel, { workers: ['w1'], isPageMain: false });
  agent.enterCache();
  said.length = 0;
  agent.leaveCache();
  assert.deepEqual(workerEvents(said), ['pageWorkerCreated:w1']);
});

test('a worker that ends in the cache is not ended twice, and is not said again at the restore', () => {
  const { said, channel } = recordingChannel();
  const { agent, live, handlers } = pageAgentOn(channel, { workers: ['w1', 'w2'] });
  agent.enterCache();
  said.length = 0;
  const gone = live.get('w1');
  live.delete('w1');
  handlers.workerdestroyed('workerdestroyed', gone);
  assert.deepEqual(workerEvents(said), [], 'the client holds no worker of this root');
  agent.leaveCache();
  assert.deepEqual(workerEvents(said), ['pageWorkerCreated:w2']);
  assert.equal(agent._workerData.has('w1'), false);
});

// ---- the whole chain: the agents, the channel, the mux, the handler -------------------------------------------

function handlerOnly() {
  const scope = { Components: { classes: {}, interfaces: {}, utils: {}, results: {} }, dump: () => {}, setTimeout, clearTimeout };
  scope.ChromeUtils = { importESModule: url => url.endsWith('Helper.js') ? { Helper, EventWatcher: class {} }
      : url.endsWith('Timer.sys.mjs') ? { setTimeout, clearTimeout } : new Proxy(function () {}, { get: () => class {} }) };
  const { PageHandler } = load('protocol/PageHandler.js', scope, ['PageHandler']);
  const events = [];
  const h = Object.create(PageHandler.prototype);
  h._session = { emitEvent: (name, payload) => events.push([name, plain(payload)]) };
  h._reportedFrameIds = new Set(); h._liveFrameIds = new Set(); h._parentOfFrame = new Map(); h._droppedFrameIds = new Set();
  h._frameEventsBeforeFrame = new Map(); h._contextFrame = new Map(); h._contextsOfFrame = new Map();
  h._workers = new Map(); h._hitWaiters = new Set(); h._landingVerdicts = new Map(); h._pendingEventWatchers = new Set();
  h._heldBackFrames = new Map(); h._eventsAfterName = new Map(); h._frameNavigations = { forget() {} };
  h._eventListeners = []; h._pageNetwork = null; h._frameLifecycle = { reannounceBelow() {}, dispose() {} };
  return { h, events };
}

test('page with a live worker -> forward with the document in the cache -> back: created, destroyed, created', async () => {
  const mux = new ContentMux({
    pageTarget: { linkedBrowser: () => ({ browsingContext: { id: 1 } }), frameIdToBrowsingContext: () => null },
    name: 'page', uid: 'parent', createChannel: (n, u) => new SimpleChannel(n, u),
  });
  const { h, events } = handlerOnly();
  h._contentChannel = mux;
  h._onFrameAttached({ frameId: 'mainframe-1' });
  mux.register('page', {
    pageWorkerCreated: p => h._onWorkerCreated(p), pageWorkerDestroyed: p => h._onWorkerDestroyed(p),
    runtimeExecutionContextsCleared: p => h._onExecutionContextsCleared(p),
    runtimeExecutionContextCreated: () => {}, runtimeExecutionContextDestroyed: () => {},
    pageEventFired: () => {}, pageReady: () => {},
  });

  // the content process: the channel, bound to the root's actor
  const channel = new SimpleChannel('content', 'process-x');
  const actor = { receiveMessage() {}, sendAsyncMessage: (name, obj) => queueMicrotask(() => channel._onMessage(structuredClone(obj))) };
  channel.setTransport({ sendMessage: obj => actor.receiveMessage({ data: structuredClone(obj) }), dispose() {} });
  mux.bindRoot(1, actor, { isMain: true });
  await settle(3);
  const { agent, main } = pageAgentOn(channel, { workers: ['w1'] });
  channel.onReattached(() => { agent._onDocumentRestored(main); agent.leaveCache(); });
  await settle(3);
  const seen = () => events.map(e => e[0]).filter(n => /^Page\.worker/.test(n)).map(n => n.replace('Page.', ''));
  assert.deepEqual(seen(), ['workerCreated']);

  // the document goes into the cache: its pagehide, as JugglerFrameChild gets it
  const frameChild = new JugglerFrameChild();
  const document = {};
  frameChild._agents = () => ({ channel, pageAgent: agent });
  frameChild.sendAsyncMessage = () => {};
  Object.defineProperty(frameChild, 'browsingContext', { value: { parent: null } });
  Object.defineProperty(frameChild, 'document', { value: document });
  frameChild.handleEvent({ type: 'pagehide', persisted: true, target: document });
  await settle(3);
  assert.deepEqual(seen(), ['workerCreated', 'workerDestroyed'], 'the worker ends when the document enters the cache');
  assert.equal(h._workers.size, 0);
  assert.equal(mux._workerRoot.size, 0);

  // the document comes back: the parent binds it again (READY), the reattachment hook runs
  frameChild.handleEvent({ type: 'pageshow', persisted: true, target: document });
  channel._onMessage({ ack: 'READY', uid: 'parent' });
  await settle(3);
  assert.deepEqual(seen(), ['workerCreated', 'workerDestroyed', 'workerCreated'], 'no workerDestroyed at the restore');
  const created = events.filter(e => e[0] === 'Page.workerCreated').map(e => e[1].workerId);
  assert.deepEqual(created, ['w1', 'w1'], 'the same worker');
  assert.equal(h._workers.size, 1);
  assert.deepEqual([...mux._workerRoot.keys()], ['w1']);
});
