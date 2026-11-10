// ContentMux: the parent's end of a page's conversation with SEVERAL content
// processes, loaded with the real SimpleChannel.
//
// Feature (fission, strategy 1): with sites isolated a page is held by one
// content process per fragment root (the top document, and every frame whose
// parent lives in another process). Each root has its own channel to its own
// actor, but the PageHandler must still see ONE channel. The mux decides where
// each command goes (by execution context, by frame, by the parent of a frame,
// to the main root, to every root with an actor), fans the events of every root
// back in with `(payload, {rootId})`, and keeps what only it can know across
// roots: which root holds an execution context, a worker, an armed click landing.
//
// Why it matters: without it a command goes to the top whatever frame it names
// (`frame.evaluate` in an out-of-process iframe fails), a root that went away
// leaves its requests waiting for ever, a `cleared` of one root wipes the
// contexts of the others on the client, and two roots that both answer
// `token: 1` for a landing are mistaken for each other.
//
// Each test builds the parent side with the real ContentMux and, per root, a
// content-side SimpleChannel joined to it by a loopback (the actor of the
// parent, structured-cloned messages), so the READY handshake, the queueing of
// requests for a channel that is not ready and their resending at the next
// READY are the real ones. No browser.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

// An ES module of Juggler's, evaluated as a script: `export` dropped, the
// names it defines handed back.
function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

const { SimpleChannel } = load('SimpleChannel.js', { dump: () => {} }, ['SimpleChannel']);
const { ContentMux, ROUTES, routeOf } = load('ContentMux.js', {}, ['ContentMux', 'ROUTES', 'routeOf']);

const MAIN = 1;

// Objects made inside the vm have other prototypes: compare plain data.
const plain = x => JSON.parse(JSON.stringify(x));
const settle = async (n = 6) => {
  for (let i = 0; i < n; i++)
    await new Promise(resolve => setTimeout(resolve, 0));
};
const rejects = (promise, expected) => assert.rejects(Promise.resolve(promise), expected);
const defer = () => {
  const d = {};
  d.promise = new Promise(resolve => { d.resolve = resolve; });
  return d;
};

const bc = (id, parent, isProcessRoot) => ({
  id,
  parent,
  currentWindowGlobal: isProcessRoot === null ? null : {isProcessRoot: !!isProcessRoot},
});

// The content process of one root: a SimpleChannel whose handlers record what
// they were asked and answer `{answeredBy}` unless `behavior[method]` says
// otherwise. It talks to the parent end through `actor`, the parent's actor.
function startContent(label) {
  const channel = new SimpleChannel('content-' + label, 'process-x');
  const content = {label, channel, calls: [], behavior: {}};
  const handler = new Proxy({}, {
    get: (_, method) => typeof method !== 'string' ? undefined : (...args) => {
      content.calls.push({method, args: plain(args)});
      const behave = content.behavior[method];
      return behave ? behave(...args) : {answeredBy: label};
    },
  });
  content.serve = namespace => channel.register(namespace, handler);
  content.serve('page');
  content.serve('');
  content.methods = () => content.calls.map(call => call.method);
  content.of = method => content.calls.filter(call => call.method === method);
  const actor = {
    browsingContext: null,
    actorName: 'JugglerFrame',
    sent: [],
    receiveMessage() {},
    sendAsyncMessage(name, obj) {
      actor.sent.push(obj);
      queueMicrotask(() => channel._onMessage(structuredClone(obj)));
    },
  };
  channel.setTransport({
    sendMessage: obj => actor.receiveMessage({data: structuredClone(obj)}),
    dispose() {},
  });
  const page = channel.connect('page');
  content.actor = actor;
  content.emit = (name, payload) => page.send(name, payload);
  return content;
}

// An actor nothing answers through: a request sent to it waits.
function deadActor() {
  return {actorName: 'JugglerFrame', sendAsyncMessage() {}, receiveMessage() {}};
}

const EVENT_NAMES = [
  'runtimeExecutionContextCreated', 'runtimeExecutionContextDestroyed', 'runtimeExecutionContextsCleared',
  'pageWorkerCreated', 'pageWorkerDestroyed',
];

function world() {
  const bcs = new Map();
  const pageTarget = {
    linkedBrowser: () => ({browsingContext: {id: MAIN}}),
    frameIdToBrowsingContext: id => bcs.get(id),
  };
  const mux = new ContentMux({pageTarget, name: 'page', uid: 'parent', createChannel: (name, uid) => new SimpleChannel(name, uid)});
  const events = [];
  const w = {
    bcs, mux, events,
    page: mux.connect('page'),
    frame(frameId, browsingContext) { bcs.set(frameId, browsingContext); return browsingContext; },
    // What the PageHandler registers: every event recorded with the root it came from.
    register() {
      return mux.register('page', Object.fromEntries(EVENT_NAMES.map(name =>
        [name, (payload, meta) => { events.push({name, payload: plain(payload), rootId: meta.rootId}); }])));
    },
    // A root with a live content process, bound and past the READY handshake.
    async root(rootId, {isMain = false} = {}) {
      const content = startContent('root' + rootId);
      content.rootId = rootId;
      mux.bindRoot(rootId, content.actor, {isMain});
      await settle(3);
      return content;
    },
  };
  return w;
}

// The top (id MAIN), a process-root child X (2) under it, Y (3) and Z (4) below X in the
// same process, and the frame ids 'ft' 'fx' 'fy' 'fz' naming them.
function tree(w) {
  const t = w.frame('ft', bc(MAIN, null, true));
  const x = w.frame('fx', bc(2, t, true));
  const y = w.frame('fy', bc(3, x, false));
  const z = w.frame('fz', bc(4, y, false));
  return {t, x, y, z};
}

test('1. a command with an execution context goes to the root that announced it', {timeout: 10000}, async () => {
  const w = world();
  w.register();
  const a = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  await a.emit('runtimeExecutionContextCreated', {executionContextId: 7, auxData: {frameId: 'fa'}});
  await b.emit('runtimeExecutionContextCreated', {executionContextId: 8, auxData: {frameId: 'fb'}});

  assert.equal(w.mux.contextRoot(7), MAIN);
  assert.equal(w.mux.contextRoot(8), 2);

  assert.equal((await w.page.send('evaluate', {executionContextId: 7, expression: '1'})).answeredBy, 'root1');
  assert.deepEqual(a.methods(), ['evaluate']);
  assert.deepEqual(b.methods(), []);

  assert.equal((await w.page.send('callFunction', {executionContextId: 8})).answeredBy, 'root2');
  assert.deepEqual(a.methods(), ['evaluate']);
  assert.deepEqual(b.methods(), ['callFunction']);

  // An unknown context is an error, and nothing is sent anywhere.
  await rejects(w.page.send('evaluate', {executionContextId: 99}), {message: 'Failed to find execution context with id = 99'});
  assert.equal(a.calls.length + b.calls.length, 2);

  // A context destroyed is forgotten.
  await a.emit('runtimeExecutionContextDestroyed', {executionContextId: 7});
  await rejects(w.page.send('evaluate', {executionContextId: 7}), /Failed to find execution context with id = 7/);
});

test('2. a frame goes to the root of its nearest process-root ancestor; an unknown root is an error, never another root', {timeout: 10000}, async () => {
  const w = world();
  const {t, x, y, z} = tree(w);
  // N is in navigation (no window global): M, below it, cannot say where its root is.
  const n = w.frame('fn', bc(5, t, null));
  const m = w.frame('fm', bc(6, n, false));
  const a = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);

  assert.equal(w.mux.frameRootOf(t), MAIN);
  assert.equal(w.mux.frameRootOf(x), 2, 'a child that is a process root is its own root');
  assert.equal(w.mux.frameRootOf(y), 2, 'the child of a root is in the same root');
  assert.equal(w.mux.frameRootOf(z), 2);
  assert.equal(w.mux.frameRootOf(n), null);
  assert.equal(w.mux.frameRootOf(m), null, 'a null window global in an ancestor');

  assert.equal((await w.page.send('describeNode', {frameId: 'fz'})).answeredBy, 'root2');
  assert.equal((await w.page.send('describeNode', {frameId: 'ft'})).answeredBy, 'root1');
  const before = a.calls.length + b.calls.length;

  await rejects(w.page.send('describeNode', {frameId: 'fm'}), /root of frame .* unknown, retry/);
  await rejects(w.page.send('describeNode', {frameId: 'fn'}), /root of frame .* unknown, retry/);
  await settle();
  assert.equal(a.calls.length + b.calls.length, before, 'nothing is sent to another root');
  await rejects(w.page.send('describeNode', {frameId: 'nope'}), /Failed to find frame with id = nope/);

  // Once the window global is there the same command goes through.
  n.currentWindowGlobal = {isProcessRoot: false};
  assert.equal((await w.page.send('describeNode', {frameId: 'fm'})).answeredBy, 'root1');
});

test('3. a broadcast reaches every root with an actor and fails only if all refuse', {timeout: 10000}, async () => {
  const w = world();
  const a = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  const c = await w.root(3);
  // A root announced but with no actor: skipped.
  w.mux.onRootAdded(4, bc(4, bc(MAIN, null, true), true));
  assert.deepEqual(plain(w.mux.rootIds()), [MAIN, 2, 3, 4]);

  b.behavior.setInitScripts = () => { throw new Error('boom'); };
  assert.equal(await w.page.send('setInitScripts', {scripts: ['s']}), undefined, 'one refusal does not fail the broadcast');
  for (const content of [a, b, c]) {
    assert.deepEqual(content.of('setInitScripts').map(call => call.args), [[{scripts: ['s']}]], content.label);
  }

  // Each of the other broadcast commands reaches the roots too.
  await w.page.send('addBinding', {name: 'n'});
  for (const content of [a, b, c])
    assert.equal(content.of('addBinding').length, 1, content.label);

  // All refuse: the broadcast fails with a reason.
  a.behavior.setInitScripts = c.behavior.setInitScripts = () => { throw new Error('boom'); };
  await rejects(w.page.send('setInitScripts', {scripts: []}), /boom/);

  // The root with no actor was skipped, not queued: it hears nothing when it is bound.
  const d = startContent('root4');
  w.mux.bindRoot(4, d.actor, {isMain: false});
  await settle();
  assert.deepEqual(d.methods(), []);

  // Only roots with no actor: nothing to wait for.
  const lone = world();
  lone.mux.onRootAdded(7, bc(7, bc(MAIN, null, true), true));
  assert.equal(await lone.page.send('setInitScripts', {scripts: []}), undefined);
  assert.equal(await world().page.send('setInitScripts', {scripts: []}), undefined);
});

test('4. landing tokens are the mux\'s: two roots with token 1 are told apart', {timeout: 10000}, async () => {
  const w = world();
  tree(w);
  const a = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  a.behavior.armLanding = b.behavior.armLanding = () => ({token: 1, extra: 'kept'});

  const ra = await w.page.send('armLanding', {frameId: 'ft'});
  const rb = await w.page.send('armLanding', {frameId: 'fz'});
  assert.equal(ra.extra, 'kept');
  assert.ok(ra.token && rb.token);
  assert.notEqual(ra.token, rb.token);
  assert.equal(a.of('armLanding').length, 1);
  assert.equal(b.of('armLanding').length, 1);

  // The entries stand for (root, root token), and are read without being taken.
  assert.deepEqual(plain(w.mux.armedEntry(rb.token)), {rootId: 2, rootToken: 1});
  assert.deepEqual(plain(w.mux.armedEntry(ra.token)), {rootId: MAIN, rootToken: 1});
  assert.ok(w.mux.armedEntry(rb.token), 'reading does not take it');

  // Taking hands the entry over once; the other landing stays armed.
  assert.deepEqual(plain(w.mux.takeArmed(rb.token)), {rootId: 2, rootToken: 1});
  assert.equal(w.mux.armedEntry(rb.token), undefined, 'the entry is gone once taken');
  assert.equal(w.mux.takeArmed(rb.token), undefined, 'a token already taken, or never armed, is nothing');
  assert.equal(w.mux.takeArmed(0), undefined);
  assert.ok(w.mux.armedEntry(ra.token), 'the other one is still armed');

  // Nothing is sent to a root to read a landing: the mux has no route for it.
  assert.throws(() => routeOf('awaitInputHandled', {}), /no route/);
});

test('5a. cleared from a root that is not the main one: its contexts are destroyed one by one, nothing is cleared', {timeout: 10000}, async () => {
  const w = world();
  w.register();
  const m = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  const c = await w.root(3);
  await m.emit('runtimeExecutionContextCreated', {executionContextId: 1, auxData: {frameId: 'fm'}});
  await b.emit('runtimeExecutionContextCreated', {executionContextId: 10, auxData: {frameId: 'fb', name: 'n'}});
  await b.emit('runtimeExecutionContextCreated', {executionContextId: 11, auxData: {frameId: 'fb', name: 'w'}});
  await c.emit('runtimeExecutionContextCreated', {executionContextId: 20, auxData: {frameId: 'fc'}});
  w.events.length = 0;

  await b.emit('runtimeExecutionContextsCleared', {});
  assert.deepEqual(w.events, [
    {name: 'runtimeExecutionContextDestroyed', payload: {executionContextId: 10}, rootId: 2},
    {name: 'runtimeExecutionContextDestroyed', payload: {executionContextId: 11}, rootId: 2},
  ]);
  assert.throws(() => w.mux.contextRoot(10), /Failed to find execution context/);
  assert.throws(() => w.mux.contextRoot(11), /Failed to find execution context/);
  assert.equal(w.mux.contextRoot(1), MAIN);
  assert.equal(w.mux.contextRoot(20), 3);
});

test('5b. cleared from the main root: forwarded, then the contexts of the other roots are announced again', {timeout: 10000}, async () => {
  const w = world();
  w.register();
  const m = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  const c = await w.root(3);
  const auxB = {frameId: 'fb', name: 'n', isDefault: true};
  await m.emit('runtimeExecutionContextCreated', {executionContextId: 1, auxData: {frameId: 'fm'}});
  await b.emit('runtimeExecutionContextCreated', {executionContextId: 10, auxData: auxB});
  await b.emit('runtimeExecutionContextCreated', {executionContextId: 11, auxData: {frameId: 'fb', name: 'w'}});
  await c.emit('runtimeExecutionContextCreated', {executionContextId: 20, auxData: {frameId: 'fc'}});
  w.events.length = 0;

  await m.emit('runtimeExecutionContextsCleared', {why: 'bfcache'});
  assert.deepEqual(w.events, [
    {name: 'runtimeExecutionContextsCleared', payload: {why: 'bfcache'}, rootId: MAIN},
    {name: 'runtimeExecutionContextCreated', payload: {executionContextId: 10, auxData: auxB}, rootId: 2},
    {name: 'runtimeExecutionContextCreated', payload: {executionContextId: 11, auxData: {frameId: 'fb', name: 'w'}}, rootId: 2},
    {name: 'runtimeExecutionContextCreated', payload: {executionContextId: 20, auxData: {frameId: 'fc'}}, rootId: 3},
  ]);
  assert.throws(() => w.mux.contextRoot(1), /Failed to find execution context/, 'the main root lost its own');
  assert.equal(w.mux.contextRoot(10), 2);
  assert.equal(w.mux.contextRoot(20), 3);
  assert.equal((await w.page.send('evaluate', {executionContextId: 10})).answeredBy, 'root2');
});

test('6. unbinding an actor that is no longer the bound one does nothing', {timeout: 10000}, async () => {
  const w = world();
  w.frame('ft', bc(MAIN, null, true));
  w.frame('f2', bc(2, w.bcs.get('ft'), true));
  const oldContent = startContent('old');
  const newContent = startContent('new');
  w.mux.bindRoot(2, oldContent.actor, {isMain: false});
  w.mux.bindRoot(2, newContent.actor, {isMain: false});

  // The new document came first, the old one is let go after.
  assert.equal(w.mux.unbindRoot(2, oldContent.actor), null);
  assert.equal(w.mux.endpointFor(2).actor, newContent.actor);
  const answer = await w.page.send('describeNode', {frameId: 'f2'});
  assert.equal(answer.answeredBy, 'new');
  assert.deepEqual(oldContent.methods(), []);

  // The bound one can be unbound.
  const gone = w.mux.unbindRoot(2, newContent.actor);
  assert.ok(gone);
  assert.equal(w.mux.endpointFor(2).actor, null);
});

test('7. unbindRoot returns the endpoint, with the root id and whether it is the main root', {timeout: 10000}, async () => {
  const w = world();
  const m = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  const gone2 = w.mux.unbindRoot(2, b.actor);
  assert.equal(gone2.rootId, 2);
  assert.equal(gone2.isMain, false);
  const goneMain = w.mux.unbindRoot(MAIN, m.actor);
  assert.equal(goneMain.rootId, MAIN);
  assert.equal(goneMain.isMain, true);
  // The endpoints are still there, with no actor; a root never bound is null.
  assert.deepEqual(plain(w.mux.rootIds()), [MAIN, 2]);
  assert.equal(w.mux.unbindRoot(9, m.actor), null);
  assert.equal(w.mux.unbindRoot(2, b.actor), null, 'already unbound');
});

test('8. a request sent while the root has no actor waits and is delivered at bindRoot', {timeout: 10000}, async () => {
  const w = world();
  tree(w);
  w.mux.onRootAdded(2, w.bcs.get('fx'));
  assert.ok(w.mux.endpointFor(2));
  let settled = false;
  const request = w.page.send('describeNode', {frameId: 'fz'});
  request.then(() => { settled = true; }, () => { settled = true; });
  await settle();
  assert.equal(settled, false);

  const b = startContent('root2');
  w.mux.bindRoot(2, b.actor, {isMain: false});
  const answer = await request;
  assert.equal(answer.answeredBy, 'root2');
  assert.deepEqual(b.of('describeNode').map(call => call.args), [[{frameId: 'fz'}]]);
  assert.equal(b.of('describeNode').length, 1, 'delivered once');
});

test('9. a root removed: endpoint, waiting requests, contexts, landings and workers go; the main root is not removed', {timeout: 10000}, async () => {
  const w = world();
  w.register();
  tree(w);
  const m = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  await m.emit('runtimeExecutionContextCreated', {executionContextId: 1, auxData: {}});
  await m.emit('pageWorkerCreated', {workerId: 'wm'});
  await b.emit('runtimeExecutionContextCreated', {executionContextId: 10, auxData: {}});
  await b.emit('pageWorkerCreated', {workerId: 'wb'});
  b.serve('wb');
  b.behavior.armLanding = () => ({token: 1});
  const armed = await w.page.send('armLanding', {frameId: 'fz'});
  assert.ok(w.mux.armedEntry(armed.token));
  b.behavior.evaluate = () => new Promise(() => {});
  const waiting = w.page.send('evaluate', {executionContextId: 10});
  const waitingWorker = w.mux.connect('wb').send('evaluate', {});
  const rejectedContext = rejects(waiting, /disposed/);
  const rejectedWorker = rejects(waitingWorker, /disposed/);
  await settle();
  w.events.length = 0;

  w.mux.onRootRemoved(2);
  await rejectedContext;
  await rejectedWorker;
  assert.equal(w.mux.endpointFor(2), undefined);
  assert.deepEqual(plain(w.mux.rootIds()), [MAIN]);
  assert.throws(() => w.mux.contextRoot(10), /Failed to find execution context/);
  assert.equal(w.mux.armedEntry(armed.token), undefined);
  assert.deepEqual(w.events, [{name: 'pageWorkerDestroyed', payload: {workerId: 'wb'}, rootId: 2}]);
  await rejects(w.mux.connect('wb').send('evaluate', {}), /Failed to find worker with id = wb/);
  // Removing it again, or a root never known, is harmless.
  w.mux.onRootRemoved(2);
  w.mux.onRootRemoved(77);

  // The main root stays: it changes id with the top, it is not removed with it.
  w.events.length = 0;
  w.mux.onRootRemoved(MAIN);
  assert.ok(w.mux.endpointFor(MAIN));
  assert.equal(w.mux.contextRoot(1), MAIN);
  assert.deepEqual(w.events, []);
  m.serve('wm');
  assert.equal((await w.mux.connect('wm').send('evaluate', {})).answeredBy, 'root1');
});

test('10. adoptNode: by context without an object; by the frame of the object with one; refused across roots', {timeout: 10000}, async () => {
  const w = world();
  tree(w);
  const a = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  w.register();
  await a.emit('runtimeExecutionContextCreated', {executionContextId: 5, auxData: {}});
  await b.emit('runtimeExecutionContextCreated', {executionContextId: 10, auxData: {}});

  // No object: the node is adopted into the context, so it goes to the context's root, not the frame's.
  await w.page.send('adoptNode', {frameId: 'ft', executionContextId: 10});
  assert.equal(a.of('adoptNode').length, 0);
  assert.equal(b.of('adoptNode').length, 1);

  // With an object: the root of its frame, the context being of the same root.
  await w.page.send('adoptNode', {frameId: 'ft', objectId: 'o', executionContextId: 5});
  assert.equal(a.of('adoptNode').length, 1);
  assert.equal(b.of('adoptNode').length, 1);
  await w.page.send('adoptNode', {frameId: 'fz', objectId: 'o'});
  assert.equal(b.of('adoptNode').length, 2, 'with no context to compare it goes to the frame');

  // With an object and a context in another root: no node can cross, and nothing is sent.
  const refused = await w.page.send('adoptNode', {frameId: 'fz', objectId: 'o', executionContextId: 5});
  assert.equal(refused.remoteObject, null);
  assert.equal(a.of('adoptNode').length, 1);
  assert.equal(b.of('adoptNode').length, 2);
  await settle();
  assert.equal(a.of('adoptNode').length, 1);
  assert.equal(b.of('adoptNode').length, 2);
});

test('11. frame commands go to the root of the frame; frameBoxTransform to the root of its PARENT; no route is an error', {timeout: 10000}, async () => {
  const w = world();
  tree(w);
  const a = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);

  assert.equal((await w.page.send('getFrameQuads', {frameId: 'fy'})).answeredBy, 'root2');
  assert.equal((await w.page.send('frameViewport', {frameId: 'fx'})).answeredBy, 'root2');
  assert.equal((await w.page.send('getFrameQuads', {frameId: 'ft'})).answeredBy, 'root1');
  assert.deepEqual(a.methods(), ['getFrameQuads']);
  assert.deepEqual(b.methods(), ['getFrameQuads', 'frameViewport']);

  // The box of X is measured where its element is: in the top (the frame itself would say root 2).
  assert.equal((await w.page.send('frameBoxTransform', {childFrameId: 'fx'})).answeredBy, 'root1');
  // The parent of Y is X, a root of its own.
  assert.equal((await w.page.send('frameBoxTransform', {childFrameId: 'fy'})).answeredBy, 'root2');
  assert.deepEqual(a.methods(), ['getFrameQuads', 'frameBoxTransform']);
  assert.deepEqual(b.methods(), ['getFrameQuads', 'frameViewport', 'frameBoxTransform']);
  // The top has no parent; an unknown frame has none either.
  await rejects(w.page.send('frameBoxTransform', {childFrameId: 'ft'}), /Failed to find frame with id = ft/);
  await rejects(w.page.send('frameBoxTransform', {childFrameId: 'nope'}), /Failed to find frame with id = nope/);

  await rejects(w.page.send('noSuchCommand', {frameId: 'ft'}), /no route for/);
  assert.throws(() => routeOf('noSuchCommand', {}), /no route for noSuchCommand/);
  assert.throws(() => w.mux.routeOf('noSuchCommand', {}), /no route for/);
  assert.equal(a.calls.length + b.calls.length, 5, 'a command with no route is not sent to the top');
});

test('11b. every method of ROUTES resolves to a route', () => {
  const params = {executionContextId: 1, frameId: 'f', childFrameId: 'c', objectId: 'o', landingToken: 1};
  const kinds = new Set(['ctx', 'frame', 'parentOf', 'main', 'broadcast', 'landing']);
  const methods = Object.keys(ROUTES);
  assert.ok(methods.length >= 25);
  for (const method of methods) {
    for (const p of [params, {}, undefined]) {
      const route = routeOf(method, p);
      assert.ok(kinds.has(route.kind), `${method}: ${route.kind}`);
    }
  }
  assert.equal(routeOf('getFrameQuads', params).kind, 'frame');
  assert.equal(routeOf('frameViewport', params).kind, 'frame');
  assert.deepEqual(plain(routeOf('frameBoxTransform', params)), {kind: 'parentOf', id: 'c'});
  assert.deepEqual(plain(routeOf('evaluate', params)), {kind: 'ctx', id: 1});
  assert.deepEqual(plain(routeOf('adoptNode', params)), {kind: 'frame', id: 'f'});
  assert.deepEqual(plain(routeOf('adoptNode', {executionContextId: 1})), {kind: 'ctx', id: 1});
  assert.equal(routeOf('setInitScripts', params).kind, 'broadcast');
  assert.equal(routeOf('dispatchKeyEvent', params).kind, 'main');
});

test('12. a new main root takes over the endpoint of the old one: pending requests, contexts and landings follow', {timeout: 10000}, async () => {
  // A request waiting on the old main root is delivered to the new one.
  {
    const w = world();
    w.frame('f1', bc(10, null, true));
    w.mux.bindRoot(10, deadActor(), {isMain: true});
    const request = w.page.send('describeNode', {frameId: 'f1'});
    await settle();
    const oldEndpoint = w.mux.endpointFor(10);

    const b = startContent('b');
    w.mux.bindRoot(20, b.actor, {isMain: true});
    assert.deepEqual(plain(w.mux.rootIds()), [20]);
    assert.equal(w.mux.endpointFor(10), undefined);
    assert.equal(w.mux.endpointFor(20), oldEndpoint, 'the same endpoint under the new id');
    assert.equal(w.mux.endpointFor(20).isMain, true);
    assert.equal((await request).answeredBy, 'b');
    assert.deepEqual(b.of('describeNode').map(call => call.args), [[{frameId: 'f1'}]]);
  }
  // What the old root announced is the new root's, and the landings armed in it.
  {
    const w = world();
    w.register();
    w.frame('f1', bc(10, null, true));
    const a = await w.root(10, {isMain: true});
    await a.emit('runtimeExecutionContextCreated', {executionContextId: 5, auxData: {}});
    a.behavior.armLanding = () => ({token: 1});
    const armed = await w.page.send('armLanding', {frameId: 'f1'});
    assert.equal(w.mux.contextRoot(5), 10);
    assert.equal(w.mux.armedEntry(armed.token).rootId, 10);

    const b = await w.root(20, {isMain: true});
    assert.deepEqual(plain(w.mux.rootIds()), [20]);
    assert.equal(w.mux.contextRoot(5), 20);
    assert.equal(w.mux.armedEntry(armed.token).rootId, 20);
    assert.equal((await w.page.send('evaluate', {executionContextId: 5})).answeredBy, 'root20');
    assert.equal(a.of('evaluate').length, 0);
    assert.equal(b.of('evaluate').length, 1);
  }
  // A non-main root is not re-keyed: a second main root next to a non-main one keeps both.
  {
    const w = world();
    await w.root(2);
    await w.root(10, {isMain: true});
    await w.root(20, {isMain: true});
    assert.deepEqual(plain(w.mux.rootIds()), [2, 20]);
  }
});

test('13. dispose() of a connector refuses what it waits for on every root, then refuses sending', {timeout: 10000}, async () => {
  const w = world();
  w.register();
  const a = await w.root(MAIN, {isMain: true});
  a.behavior.evaluate = () => new Promise(() => {});
  await a.emit('runtimeExecutionContextCreated', {executionContextId: 5, auxData: {}});
  const connector = w.mux.connect('page');
  const other = w.mux.connect('page');

  const onA = connector.send('evaluate', {executionContextId: 5});
  const otherOnA = other.send('evaluate', {executionContextId: 5});
  let otherSettled = false;
  otherOnA.then(() => { otherSettled = true; }, () => { otherSettled = true; });
  const rejectedA = rejects(onA, /disposed/);

  // A root that came after the connector's first send, with a request of the same connector.
  const b = await w.root(2);
  b.behavior.evaluate = () => new Promise(() => {});
  await b.emit('runtimeExecutionContextCreated', {executionContextId: 6, auxData: {}});
  const onB = connector.send('evaluate', {executionContextId: 6});
  const rejectedB = rejects(onB, /disposed/);
  await settle();
  assert.equal(a.of('evaluate').length, 2);
  assert.equal(b.of('evaluate').length, 1);

  connector.dispose();
  await rejectedA;
  await rejectedB;
  await settle();
  assert.equal(otherSettled, false, 'another connector keeps its requests');

  await rejects(connector.send('evaluate', {executionContextId: 5}), /already disposed/);
  assert.equal(a.of('evaluate').length, 2, 'nothing is sent after dispose');
});

test('register: handlers get (payload, {rootId}) from every root, and the unregister removes them on all', {timeout: 10000}, async () => {
  const w = world();
  const a = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  const unregister = w.register();   // after the roots: installed on the existing ones too
  assert.equal(typeof unregister, 'function');

  await a.emit('runtimeExecutionContextCreated', {executionContextId: 1, auxData: {k: 'a'}});
  await b.emit('runtimeExecutionContextCreated', {executionContextId: 2, auxData: {k: 'b'}});
  const c = await w.root(3);   // after the registration: installed on the new one
  await c.emit('runtimeExecutionContextCreated', {executionContextId: 3, auxData: {k: 'c'}});
  assert.deepEqual(w.events.map(e => [e.payload.executionContextId, e.rootId, e.payload.auxData.k]),
      [[1, MAIN, 'a'], [2, 2, 'b'], [3, 3, 'c']]);

  // The namespace is taken.
  assert.throws(() => w.mux.register('page', {}), /double-register for namespace page/);
  // Another one is free.
  const other = w.mux.register('other', {});
  other();

  unregister();
  w.events.length = 0;
  // Nobody answers a namespace with no handler: the sends are left to wait, not awaited.
  void a.emit('runtimeExecutionContextCreated', {executionContextId: 4, auxData: {}});
  void b.emit('runtimeExecutionContextCreated', {executionContextId: 5, auxData: {}});
  void c.emit('runtimeExecutionContextCreated', {executionContextId: 6, auxData: {}});
  await settle();
  assert.deepEqual(w.events, []);
  assert.throws(() => w.mux.contextRoot(4), /Failed to find execution context/);

  // Unregistered twice is harmless, and the namespace can be registered again.
  unregister();
  w.events.length = 0;
  assert.doesNotThrow(() => w.register());
  await settle();
  assert.deepEqual(w.events.map(e => [e.payload.executionContextId, e.rootId]), [[4, MAIN], [5, 2], [6, 3]],
      'what was said meanwhile waited for the handlers');
});

test('workers: a worker namespace goes to the root of its pageWorkerCreated', {timeout: 10000}, async () => {
  const w = world();
  w.register();
  const a = await w.root(MAIN, {isMain: true});
  const b = await w.root(2);
  b.serve('worker-1');
  a.serve('worker-2');
  await b.emit('pageWorkerCreated', {workerId: 'worker-1'});
  await a.emit('pageWorkerCreated', {workerId: 'worker-2'});

  assert.equal((await w.mux.connect('worker-1').send('evaluate', {expression: 'x'})).answeredBy, 'root2');
  assert.equal((await w.mux.connect('worker-2').send('evaluate', {expression: 'y'})).answeredBy, 'root1');
  assert.deepEqual(b.of('evaluate').map(call => call.args), [[{expression: 'x'}]]);
  assert.deepEqual(a.of('evaluate').map(call => call.args), [[{expression: 'y'}]]);

  await rejects(w.mux.connect('worker-9').send('evaluate', {}), /Failed to find worker/);
  await b.emit('pageWorkerDestroyed', {workerId: 'worker-1'});
  await rejects(w.mux.connect('worker-1').send('evaluate', {}), /Failed to find worker with id = worker-1/);
  // sendTo goes to a root past the routing.
  assert.equal((await w.mux.sendTo(2, 'page', 'evaluate', {executionContextId: 12345})).answeredBy, 'root2');
});
