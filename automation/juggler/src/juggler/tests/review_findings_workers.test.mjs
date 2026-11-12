// Review of fission/integration e51023ea3d, the findings on what the page handler and the mux keep of frames,
// contexts and workers: F5, F7, F8 and "the queues rebuilt by an event after framedropped".
// Each test fails on e51023ea3d / 644dbfc9a8.
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

function startContent(label) {
  const channel = new SimpleChannel('content-' + label, 'process-x');
  const content = { label, channel, calls: [], behavior: {} };
  const handler = new Proxy({}, { get: (_, method) => typeof method !== 'string' ? undefined : (...args) => {
    content.calls.push({ method, args: plain(args) });
    return content.behavior[method] ? content.behavior[method](...args) : { answeredBy: label };
  } });
  channel.register('page', handler);
  const actor = { browsingContext: null, actorName: 'JugglerFrame', receiveMessage() {},
    sendAsyncMessage(name, obj) { queueMicrotask(() => channel._onMessage(structuredClone(obj))); } };
  channel.setTransport({ sendMessage: obj => actor.receiveMessage({ data: structuredClone(obj) }), dispose() {} });
  content.actor = actor;
  return content;
}

function world() {
  const bcs = new Map();
  const mux = new ContentMux({
    pageTarget: { linkedBrowser: () => ({ browsingContext: { id: 1 } }), frameIdToBrowsingContext: id => bcs.get(id) },
    name: 'page', uid: 'parent', createChannel: (n, u) => new SimpleChannel(n, u),
  });
  return { bcs, mux, page: mux.connect('page'), async root(rootId, isMain = false) {
    const c = startContent('root' + rootId); mux.bindRoot(rootId, c.actor, { isMain }); await settle(3); return c; } };
}

function pageHandlerOnly() {
  const scope = { Components: { classes: {}, interfaces: {}, utils: {}, results: {} }, dump: () => {}, setTimeout, clearTimeout };
  scope.ChromeUtils = { importESModule: url => url.endsWith('Helper.js') ? { Helper, EventWatcher: class {} }
      : url.endsWith('Timer.sys.mjs') ? { setTimeout, clearTimeout } : new Proxy(function () {}, { get: () => class {} }) };
  const { PageHandler } = load('protocol/PageHandler.js', scope, ['PageHandler']);
  const events = [];
  const h = Object.create(PageHandler.prototype);
  h._workersNotToldYet = new Map();
  h._restoreBeingSaid = false;
  h._session = { emitEvent: (name, payload) => events.push([name, plain(payload)]) };
  h._reportedFrameIds = new Set(); h._liveFrameIds = new Set(); h._parentOfFrame = new Map(); h._droppedFrameIds = new Set();
  h._frameEventsBeforeFrame = new Map(); h._contextFrame = new Map(); h._contextsOfFrame = new Map();
  h._workers = new Map(); h._hitWaiters = new Set(); h._landingVerdicts = new Map(); h._pendingEventWatchers = new Set();
  h._heldBackFrames = new Map(); h._eventsAfterName = new Map(); h._frameNavigations = { forget() {} };
  h._eventListeners = []; h._pageNetwork = null; h._frameLifecycle = { reannounceBelow() {}, dispose() {} };
  return { h, events };
}

// The agents of a root, as the content process builds them: what they say when they start, and when a document comes
// back from the back-forward cache.
function pageAgentOf({ workers = [] } = {}) {
  const scope = { Components: { interfaces: {}, results: {}, utils: {} }, dump: () => {},
    Cc: new Proxy({}, { get: () => ({ getService: () => ({ addObserver() {}, removeObserver() {} }) }) }) };
  scope.ChromeUtils = { importESModule: url => url.endsWith('Helper.js') ? { Helper: class extends Helper { addObserver() { return () => {}; } } }
      : { NetUtil: {}, setTimeout } };
  const { PageAgent } = load('content/PageAgent.js', scope, ['PageAgent']);
  const said = [];
  const channel = { connect: ns => ({ emit: (method, params) => said.push([ns, method, plain(params)]), dispose() {} }), register: () => () => {} };
  const workerChannel = { connect: () => ({ dispose() {} }), register: () => () => {} };
  const main = { id: () => 'mainframe-1', docShell: () => ({}), url: () => 'about:blank', name: () => '', pendingNavigationId: () => null,
    lastCommittedNavigationId: () => null, domWindow: () => null };
  const runtime = new Proxy({}, { get: (_, name) => name === 'executionContexts' ? () => [] : name === 'events' ? new Proxy({}, { get: () => () => () => {} }) : function () {} });
  const tree = { runtime: () => runtime, mainFrame: () => main, frames: () => [], isPageReady: () => false,
    workers: () => workers.map(id => ({ id: () => id, url: () => 'w.js', frame: () => main, channel: () => workerChannel })), _isPageMain: () => true, enterCache() {}, leaveCache() {}, on() {}, off() {} };
  return { agent: new PageAgent(channel, tree), said, tree };
}

// What the handler asks of the mux for a worker: a channel to it, and the fan-in of its events.
const workerChannel = disposed => ({
  connect: id => ({ send: async () => {}, dispose: () => disposed.push(id) }),
  register: () => () => {},
});

test('F5 a frame the handler forgot at a commit keeps its contexts when it is detached later', () => {
  const { h } = pageHandlerOnly();
  h._onFrameAttached({ frameId: 'm' });
  h._onFrameAttached({ frameId: 'f', parentFrameId: 'm' });
  h._onExecutionContextCreated({ executionContextId: 'c1', auxData: { frameId: 'f' } });
  h._onNavigationCommitted({ frameId: 'm', navigationId: 'nav-9', url: 'http://a.test/' });   // _forgetFramesBelow('m')
  h._onFrameDetached('f');                                                                    // the lifecycle's detach, later
  assert.equal(h._contextFrame.size, 0, '_contextFrame');
  assert.equal(h._contextsOfFrame.size, 0, '_contextsOfFrame');
});

test('F5 the workers of a frame forgotten at a commit end with it; the detach of a frame that is not live cleans what is left', () => {
  const { h, events } = pageHandlerOnly();
  const disposed = [];
  h._contentChannel = workerChannel(disposed);
  h._onFrameAttached({ frameId: 'm' });
  h._onFrameAttached({ frameId: 'f', parentFrameId: 'm' });
  h._onWorkerCreated({ workerId: 'w1', frameId: 'f', url: 'http://a.test/w.js' });
  events.length = 0;
  h._onNavigationCommitted({ frameId: 'm', navigationId: 'nav-9', url: 'http://a.test/' });
  assert.deepEqual(disposed, ['w1'], 'the worker of the forgotten frame is disposed at the commit');
  assert.equal(h._workers.size, 0, '_workers');
  // the worker ends BEFORE the commit that takes its frame away (the stock client closes it first, too), not after
  assert.deepEqual(events.map(e => e[0]), ['Page.workerDestroyed', 'Page.navigationCommitted']);
  // a frame the handler does not hold live: the detach still leaves nothing of it behind, and tells the client nothing
  h._onExecutionContextCreated({ executionContextId: 'c2', auxData: { frameId: 'g' } });
  h._onWorkerCreated({ workerId: 'w2', frameId: 'g', url: 'http://a.test/w.js' });
  events.length = 0;
  h._onFrameDetached('g');
  assert.equal(h._contextFrame.size, 0, '_contextFrame');
  assert.equal(h._contextsOfFrame.size, 0, '_contextsOfFrame');
  assert.equal(h._workers.size, 0, '_workers');
  assert.ok(!events.some(e => e[0] === 'Page.frameDetached'), 'the client never heard of this frame');
});

test('F7 PageHandler.dispose() leaves its workers registered on the page\'s mux', async () => {
  const { h } = pageHandlerOnly();
  let disposed = 0;
  h._workers.set('w1', { dispose() { disposed++; } });
  h._contentPage = { dispose() {} };
  await h.dispose();
  assert.equal(disposed, 1);
  assert.equal(h._workers.size, 0);
});

test('F8 the workers of a root whose agents were replaced stay in the mux and in the handler', async () => {
  const w = world();
  const seen = [];
  w.mux.register('page', { pageWorkerCreated: p => seen.push(p), pageWorkerDestroyed: () => {}, runtimeExecutionContextsCleared: () => {},
    runtimeExecutionContextCreated: () => {}, runtimeExecutionContextDestroyed: () => {} });
  const first = await w.root(1, true);
  await first.channel.connect('page').send('pageWorkerCreated', { workerId: 'w1', frameId: 'mainframe-1', url: 'x' });
  assert.equal(w.mux._workerRoot.size, 1);
  // a new process takes the top: new agents, they announce nothing of the old worker, and clear their contexts
  const second = startContent('second');
  w.mux.bindRoot(1, second.actor, { isMain: true });
  await settle(3);
  await second.channel.connect('page').send('runtimeExecutionContextsCleared', {});
  assert.equal(w.mux._workerRoot.size, 0, 'the old agents died without saying so');
});

test('F8 the handler ends the workers of the root whose agents were replaced', async () => {
  const w = world();
  const { h, events } = pageHandlerOnly();
  const disposed = [];
  h._contentChannel = workerChannel(disposed);
  w.mux.register('page', { pageWorkerCreated: p => h._onWorkerCreated(p), pageWorkerDestroyed: p => h._onWorkerDestroyed(p),
    runtimeExecutionContextsCleared: p => h._onExecutionContextsCleared(p),
    runtimeExecutionContextCreated: () => {}, runtimeExecutionContextDestroyed: () => {} });
  const first = await w.root(1, true);
  await first.channel.connect('page').send('pageWorkerCreated', { workerId: 'w1', frameId: 'mainframe-1', url: 'x' });
  assert.equal(h._workers.size, 1);
  const second = startContent('second');
  w.mux.bindRoot(1, second.actor, { isMain: true });
  await settle(3);
  await second.channel.connect('page').send('runtimeExecutionContextsCleared', {});
  assert.equal(h._workers.size, 0, 'the WorkerHandler of the old agents is still in the handler');
  assert.deepEqual(disposed, ['w1']);
  assert.ok(events.some(e => e[0] === 'Page.workerDestroyed' && e[1].workerId === 'w1'), 'the client is told the worker is gone');
});

test('Sonnet #7 an event that comes after framedropped does not rebuild the state of the dropped frame', () => {
  const { h, events } = pageHandlerOnly();
  const disposed = [];
  const made = [];
  h._contentChannel = { connect: id => { made.push(id); return { send: async () => {}, dispose: () => disposed.push(id) }; }, register: () => () => {} };
  h._dropBufferedFrameEvents('d');                                                       // framedropped: 'd' was never announced
  h._handleNetworkEvent('Network.requestWillBeSent', { requestId: 'r' }, 'd');          // the late ones
  h._onExecutionContextCreated({ executionContextId: 'c', auxData: { frameId: 'd' } });
  h._onWorkerCreated({ workerId: 'w', frameId: 'd', url: 'x' });
  assert.equal(h._frameEventsBeforeFrame.size, 0, 'a queue waits for a frame that will never be announced');
  assert.equal(h._contextFrame.size, 0, '_contextFrame');
  assert.equal(h._contextsOfFrame.size, 0, '_contextsOfFrame');
  assert.equal(h._workers.size, 0, '_workers');
  assert.deepEqual(events, [], 'nothing leaves for a frame the client never heard of');
});

test('F8 the agents say the workers they hold after they clear their contexts, not before', () => {
  const { said } = pageAgentOf({ workers: ['w1'] });
  const names = said.filter(([ns]) => ns === 'page').map(([, method]) => method);
  assert.deepEqual(names.filter(n => n === 'runtimeExecutionContextsCleared' || n === 'pageWorkerCreated'),
      ['runtimeExecutionContextsCleared', 'pageWorkerCreated'],
      'the parent ends the workers it knew of the root at the clear: a worker said before it would be ended with them');
});

test('F8 a document restored from the back-forward cache says its workers again, and follows each only once', () => {
  const { agent, said, tree } = pageAgentOf({ workers: ['w1'] });
  assert.equal(agent._workerData.size, 1);
  const before = agent._workerData.get('w1');
  agent.enterCache();                                     // the pagehide: the workers end for the client (worker_follows_the_document_in_the_cache)
  said.length = 0;
  agent._onDocumentRestored(tree.mainFrame());
  const names = said.filter(([ns]) => ns === 'page').map(([, method]) => method);
  assert.deepEqual(names.filter(n => n === 'runtimeExecutionContextsCleared' || n === 'pageWorkerCreated'),
      ['runtimeExecutionContextsCleared', 'pageWorkerCreated'], 'the workers are said after the clear, once');
  assert.equal(agent._workerData.get('w1'), before, 'the worker keeps the WorkerData it had');
});

test('Sonnet #7 a frame the client was told of is not dropped by framedropped: its late events still leave', () => {
  const { h, events } = pageHandlerOnly();
  h._onFrameAttached({ frameId: 'k', parentFrameId: 'm' });              // the content process announced it before the parent did
  h._dropBufferedFrameEvents('k');
  events.length = 0;
  h._handleNetworkEvent('Network.requestWillBeSent', { requestId: 'r' }, 'k');
  h._onExecutionContextCreated({ executionContextId: 'c', auxData: { frameId: 'k' } });
  assert.deepEqual(events.map(e => e[0]), ['Network.requestWillBeSent', 'Runtime.executionContextCreated']);
});

test('a worker of a frame below a commit is told ended before the commit, once, also when the old process said nothing', () => {
  const { h, events } = pageHandlerOnly();
  const disposed = [];
  h._contentChannel = workerChannel(disposed);
  h._onFrameAttached({ frameId: 'm' });
  h._onFrameAttached({ frameId: 'f', parentFrameId: 'm' });
  h._onFrameAttached({ frameId: 'g', parentFrameId: 'f' });        // two levels down
  h._onWorkerCreated({ workerId: 'wf', frameId: 'f', url: 'http://b.test/w.js' });
  h._onWorkerCreated({ workerId: 'wg', frameId: 'g', url: 'http://c.test/w.js' });
  h._onWorkerCreated({ workerId: 'wm', frameId: 'm', url: 'http://a.test/w.js' });   // the committed frame's own: not this path's
  events.length = 0;
  h._onNavigationCommitted({ frameId: 'm', navigationId: 'nav-9', url: 'http://a.test/' });
  const names = events.map(e => e[0] + (e[0] === 'Page.workerDestroyed' ? ' ' + e[1].workerId : ''));
  const commit = names.indexOf('Page.navigationCommitted');
  assert.ok(commit >= 0);
  assert.deepEqual(names.slice(0, commit).sort(), ['Page.workerDestroyed wf', 'Page.workerDestroyed wg'], 'both before the commit');
  assert.equal(names.slice(commit).filter(n => n.startsWith('Page.workerDestroyed')).length, 0, 'none after it');
  assert.ok(h._workers.has('wm'), 'the worker of the committed frame itself is not ended by this path');
  // the old process's own message (enterCache) arriving after: nothing more is said
  events.length = 0;
  h._onWorkerDestroyed({ workerId: 'wf' });
  assert.deepEqual(events, []);
});
