// An event of a frame must not reach the client before the frame itself, loaded the way the parent loads PageHandler.js.
//
// The client (Playwright) builds its Frame objects from Page.frameAttached; a navigation, a network request, an
// execution context created for a frame it has not heard of is an event it cannot place. Two voices announce the
// frames of a page: the parent process (FrameLifecycle, from browsing-context-attached/-discarded, whatever process
// holds the frame) and the content process. Both go through PageHandler._onFrameAttached/_onFrameDetached, which are
// idempotent, so a frame is announced once and detached once. Frame-keyed events wait in one queue per frame until
// the frame is announced, in the order they came, up to a cap; the execution contexts of a frame are tracked so that
// a frame's contexts are destroyed before the frame is detached, and so that a destroyed never leaves before its
// created.
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

const anything = new Proxy(function () {}, { get: () => class {} });
const dumps = [];
const scope = () => ({
  Components: { classes: {}, interfaces: {}, utils: {}, results: {} },
  ChromeUtils: { importESModule: () => anything },
  dump: message => dumps.push(message),
});
const { PageHandler } = load('protocol/PageHandler.js', scope(), ['PageHandler']);

// Payloads are copied out of the vm realm, so deepEqual compares plain objects.
// A handler with the fields the frame gate uses, as the constructor leaves them, and the list of what leaves for the client.
function pageHandler() {
  const events = [];
  const handler = Object.create(PageHandler.prototype);
  handler._session = { emitEvent: (name, payload) => events.push([name, JSON.parse(JSON.stringify(payload))]) };
  handler._reportedFrameIds = new Set();
  handler._droppedFrameIds = new Set();
  handler._liveFrameIds = new Set();
  handler._parentOfFrame = new Map();
  handler._heldBackFrames = new Map();
  handler._frameNavigations = { forget() {} };
  handler._frameEventsBeforeFrame = new Map();
  handler._eventsAfterName = new Map();
  handler._contextFrame = new Map();
  handler._contextsOfFrame = new Map();
  handler._workers = new Map();
  return { handler, events };
}

const names = events => events.map(([name]) => name);
const created = (executionContextId, frameId) => ({ executionContextId, auxData: { frameId } });
const destroyed = executionContextId => ({ executionContextId });

test('a frame is announced once and detached once, and a frame the client never heard of is not detached', () => {
  const { handler, events } = pageHandler();

  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  assert.deepEqual(events, [['Page.frameAttached', { frameId: 'f1', parentFrameId: 'main' }]]);

  handler._onFrameDetached('f1');
  handler._onFrameDetached('f1');
  assert.deepEqual(events.slice(1), [['Page.frameDetached', { frameId: 'f1' }]]);

  events.length = 0;
  handler._onFrameDetached('never-announced');
  assert.deepEqual(events, [], 'a detach for a frame nobody announced emits nothing');
});

test('a context created and destroyed before its frame is announced leaves after the frame, in order', () => {
  const { handler, events } = pageHandler();

  handler._onExecutionContextCreated(created('ctx1', 'f1'));
  handler._onExecutionContextDestroyed(destroyed('ctx1'));
  assert.deepEqual(events, [], 'nothing leaves for a frame the client does not know');

  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  assert.deepEqual(events, [
    ['Page.frameAttached', { frameId: 'f1', parentFrameId: 'main' }],
    ['Runtime.executionContextCreated', created('ctx1', 'f1')],
    ['Runtime.executionContextDestroyed', destroyed('ctx1')],
  ]);
  assert.equal(handler._contextFrame.size, 0, 'the destroyed context is forgotten');
});

test('a context of an announced frame leaves at once', () => {
  const { handler, events } = pageHandler();
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  events.length = 0;

  handler._onExecutionContextCreated(created('ctx1', 'f1'));
  assert.deepEqual(names(events), ['Runtime.executionContextCreated']);
  handler._onExecutionContextDestroyed(destroyed('ctx1'));
  assert.deepEqual(names(events), ['Runtime.executionContextCreated', 'Runtime.executionContextDestroyed']);
  assert.equal(handler._frameEventsBeforeFrame.size, 0, 'nothing waits');
});

test('the detach of a frame destroys its known contexts first, then detaches the frame', () => {
  const { handler, events } = pageHandler();
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  handler._onFrameAttached({ frameId: 'f2', parentFrameId: 'main' });
  handler._onExecutionContextCreated(created('a', 'f1'));
  handler._onExecutionContextCreated(created('b', 'f1'));
  handler._onExecutionContextCreated(created('c', 'f2'));
  events.length = 0;

  handler._onFrameDetached('f1');
  assert.deepEqual(events, [
    ['Runtime.executionContextDestroyed', destroyed('a')],
    ['Runtime.executionContextDestroyed', destroyed('b')],
    ['Page.frameDetached', { frameId: 'f1' }],
  ], 'every context of f1 and none of f2, before the frame');
  assert.equal(handler._contextFrame.has('a'), false);
  assert.equal(handler._contextFrame.has('b'), false);
  assert.equal(handler._contextFrame.get('c'), 'f2', 'the context of the other frame is kept');
  assert.equal(handler._contextsOfFrame.has('f1'), false);
});

test('a late destroyed from the content for a context the detach already destroyed is dropped', () => {
  const { handler, events } = pageHandler();
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  handler._onExecutionContextCreated(created('a', 'f1'));
  handler._onFrameDetached('f1');
  events.length = 0;

  handler._onExecutionContextDestroyed(destroyed('a'));
  assert.deepEqual(events, [], 'the client has been told once');
  assert.equal(handler._frameEventsBeforeFrame.size, 0, 'and nothing was queued for the gone frame');

  handler._onExecutionContextDestroyed(destroyed('never-created'));
  assert.deepEqual(events, [], 'a context nobody created is not ours to destroy');
});

test('executionContextsCleared forgets every context, drops only the waiting contexts, keeps the waiting network events', () => {
  const { handler, events } = pageHandler();
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  handler._onExecutionContextCreated(created('live', 'f1'));
  handler._onExecutionContextCreated(created('waiting', 'f2'));
  handler._handleNetworkEvent('Network.requestWillBeSent', { requestId: 'r1' }, 'f2');
  handler._onExecutionContextDestroyed(destroyed('waiting'));
  handler._onExecutionContextCreated(created('waiting2', 'f2'));
  handler._handleNetworkEvent('Network.responseReceived', { requestId: 'r1' }, 'f2');
  events.length = 0;
  assert.equal(handler._contextFrame.size, 2);

  handler._onExecutionContextsCleared({ some: 'payload' });
  assert.deepEqual(events, [['Runtime.executionContextsCleared', { some: 'payload' }]], 'forwarded, once');
  assert.equal(handler._contextFrame.size, 0);
  assert.equal(handler._contextsOfFrame.size, 0);
  assert.deepEqual(
      Array.from(handler._frameEventsBeforeFrame.get('f2'), event => event.protocolEventName),
      ['Network.requestWillBeSent', 'Network.responseReceived'],
      'no Runtime.executionContext* entry waits any more, the network events do');

  events.length = 0;
  handler._onFrameAttached({ frameId: 'f2', parentFrameId: 'main' });
  assert.deepEqual(names(events), ['Page.frameAttached', 'Network.requestWillBeSent', 'Network.responseReceived'],
      'the client never hears of a context the clear removed');
});

test('the cap: the 1001st event waiting for a frame is dropped with a warning, the first 1000 leave in order', () => {
  const { handler, events } = pageHandler();
  dumps.length = 0;

  for (let i = 0; i < 1001; i++)
    handler._handleNetworkEvent('Network.requestWillBeSent', { requestId: 'r' + i }, 'f1');
  assert.equal(handler._frameEventsBeforeFrame.get('f1').length, 1000);
  assert.equal(dumps.length, 1, 'one warning for the one dropped event');
  assert.match(dumps[0], /^WARNING: 1000 events wait for frame f1, Network\.requestWillBeSent is dropped/);
  assert.deepEqual(events, [], 'nothing leaves before the frame');

  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  assert.equal(events.length, 1001, 'frameAttached and the 1000 kept');
  assert.equal(events[0][0], 'Page.frameAttached');
  const ids = events.slice(1).map(([, payload]) => payload.requestId);
  assert.deepEqual(ids, Array.from({ length: 1000 }, (_, i) => 'r' + i), 'the first 1000, in order, the 1001st is gone');
  assert.equal(handler._frameEventsBeforeFrame.has('f1'), false, 'the queue is emptied');
});

test('the cap is per frame', () => {
  const { handler, events } = pageHandler();
  dumps.length = 0;
  for (let i = 0; i < 1000; i++)
    handler._handleNetworkEvent('Network.requestWillBeSent', { requestId: 'a' + i }, 'f1');
  handler._handleNetworkEvent('Network.requestWillBeSent', { requestId: 'b' }, 'f2');
  assert.deepEqual(dumps, [], 'another frame has its own queue');
  handler._onFrameAttached({ frameId: 'f2', parentFrameId: 'main' });
  assert.deepEqual(events, [
    ['Page.frameAttached', { frameId: 'f2', parentFrameId: 'main' }],
    ['Network.requestWillBeSent', { requestId: 'b' }],
  ]);
});

test('_dropBufferedFrameEvents empties the queue of the frame and forgets its contexts', () => {
  const { handler, events } = pageHandler();
  handler._onExecutionContextCreated(created('a', 'f1'));
  handler._handleNetworkEvent('Network.requestWillBeSent', { requestId: 'r1' }, 'f1');
  handler._onExecutionContextCreated(created('x', 'f2'));
  assert.equal(handler._frameEventsBeforeFrame.get('f1').length, 2);

  handler._dropBufferedFrameEvents('f1');
  assert.equal(handler._frameEventsBeforeFrame.has('f1'), false);
  assert.equal(handler._contextFrame.has('a'), false);
  assert.equal(handler._contextsOfFrame.has('f1'), false);
  assert.equal(handler._contextFrame.get('x'), 'f2', 'another frame is untouched');
  assert.equal(handler._frameEventsBeforeFrame.get('f2').length, 1);

  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  assert.deepEqual(events, [['Page.frameAttached', { frameId: 'f1', parentFrameId: 'main' }]],
      'nothing of the dropped frame leaves');
  handler._onExecutionContextDestroyed(destroyed('a'));
  assert.deepEqual(names(events), ['Page.frameAttached'], 'its context is not ours any more');
});

test('network events wait for their frame like the contexts and keep the order with them', () => {
  const { handler, events } = pageHandler();

  handler._handleNetworkEvent('Network.requestWillBeSent', { requestId: 'r1' }, 'f1');
  handler._onExecutionContextCreated(created('ctx1', 'f1'));
  handler._handleNetworkEvent('Network.responseReceived', { requestId: 'r1' }, 'f1');
  handler._onExecutionContextDestroyed(destroyed('ctx1'));
  handler._handleNetworkEvent('Network.requestFinished', { requestId: 'r1' }, 'f1');
  assert.deepEqual(events, [], 'nothing leaves for a frame the client does not know');

  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  assert.deepEqual(names(events), [
    'Page.frameAttached',
    'Network.requestWillBeSent',
    'Runtime.executionContextCreated',
    'Network.responseReceived',
    'Runtime.executionContextDestroyed',
    'Network.requestFinished',
  ]);

  events.length = 0;
  handler._handleNetworkEvent('Network.requestWillBeSent', { requestId: 'r2' }, 'f1');
  assert.deepEqual(events, [['Network.requestWillBeSent', { requestId: 'r2' }]], 'once the frame is known an event leaves at once');
});

test('frame-keyed events without a frame leave at once, and a late event of a detached frame is still delivered', () => {
  const { handler, events } = pageHandler();
  handler._emitFrameKeyed('Page.something', { a: 1 }, undefined);
  assert.deepEqual(events, [['Page.something', { a: 1 }]]);

  events.length = 0;
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  handler._onFrameDetached('f1');
  events.length = 0;
  handler._handleNetworkEvent('Network.requestFinished', { requestId: 'late' }, 'f1');
  assert.deepEqual(events, [['Network.requestFinished', { requestId: 'late' }]],
      'the frame was announced once; an event that comes late is delivered, as it always was');
});

test('a frame can be detached and a new one with the same id is announced again only after the detach', () => {
  const { handler, events } = pageHandler();
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  handler._onFrameDetached('f1');
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  assert.deepEqual(names(events), ['Page.frameAttached', 'Page.frameDetached', 'Page.frameAttached']);
});

// The client drops the child frames of a frame when a document commits in it, and says nothing. A document restored
// from the back-forward cache announces its iframes again, because the client dropped them at the commit of the
// document that replaced it: an announcement the handler took for a repeat left the restored page without its iframes
// (measured on a page with one iframe: frames 2, then after goto('about:blank') and go_back() 1).
test('the commit of a main document makes its frames announce-able again: the iframes of a restored document are told again', () => {
  const { handler, events } = pageHandler();
  handler._onFrameAttached({ frameId: 'mainframe-1', parentFrameId: undefined });
  handler._onFrameAttached({ frameId: 'subframe-2', parentFrameId: 'mainframe-1' });
  handler._onFrameAttached({ frameId: 'subframe-3', parentFrameId: 'subframe-2' });
  events.length = 0;

  handler._onNavigationCommitted({ frameId: 'mainframe-1', navigationId: 'nav-1', url: 'about:blank' });
  handler._onFrameAttached({ frameId: 'mainframe-1', parentFrameId: undefined });
  handler._onFrameAttached({ frameId: 'subframe-2', parentFrameId: 'mainframe-1' });
  handler._onFrameAttached({ frameId: 'subframe-3', parentFrameId: 'subframe-2' });
  assert.deepEqual(events.map(([name, payload]) => [name, payload.frameId]), [
    ['Page.navigationCommitted', 'mainframe-1'],
    ['Page.frameAttached', 'subframe-2'],
    ['Page.frameAttached', 'subframe-3'],
  ], 'the main frame stays announced; its children, which the client dropped, are announced again, and once');
});

test('the commit of a subframe forgets only the frames below it', () => {
  const { handler, events } = pageHandler();
  handler._onFrameAttached({ frameId: 'mainframe-1', parentFrameId: undefined });
  handler._onFrameAttached({ frameId: 'subframe-2', parentFrameId: 'mainframe-1' });
  handler._onFrameAttached({ frameId: 'subframe-3', parentFrameId: 'subframe-2' });
  handler._onFrameAttached({ frameId: 'subframe-4', parentFrameId: 'mainframe-1' });
  events.length = 0;

  handler._onNavigationCommitted({ frameId: 'subframe-2', navigationId: 'nav-2', url: 'https://example.test/' });
  for (const [frameId, parentFrameId] of [['subframe-2', 'mainframe-1'], ['subframe-3', 'subframe-2'], ['subframe-4', 'mainframe-1']])
    handler._onFrameAttached({ frameId, parentFrameId });
  assert.deepEqual(events.map(([name, payload]) => [name, payload.frameId]), [
    ['Page.navigationCommitted', 'subframe-2'],
    ['Page.frameAttached', 'subframe-3'],
  ], 'the frame that committed and its sibling stay announced; only its child is told again');
});

// A frame the parent sees attach while its parent frame navigates would be dropped by the client at once: the client
// forgets the frames below a frame when a document commits in it. (Measured: attach, commit, attach again left the
// first, dropped Frame object in the client's child_frames: main_frame.child_frames == [Frame(url=''), payment].)
function navigatingPage() {
  const { handler, events } = pageHandler();
  const pending = new Set();
  handler._frameNavigations = { forget() {}, pendingNavigationId: frameId => (pending.has(frameId) ? 'nav-1' : undefined) };
  const below = [];
  handler._frameLifecycle = { framesBelow: () => below, reannounceBelow() {} };
  handler._onFrameAttached({ frameId: 'mainframe-1', parentFrameId: undefined });
  events.length = 0;
  return { handler, events, pending, below };
}

test('a frame that attaches while its parent navigates is held back, and told when the parent commits', () => {
  const { handler, events, pending, below } = navigatingPage();
  pending.add('mainframe-1');
  const child = { frameId: 'subframe-2', parentFrameId: 'mainframe-1' };
  const grandchild = { frameId: 'subframe-3', parentFrameId: 'subframe-2' };
  handler._onFrameSeenByTheParent(child);
  handler._onFrameSeenByTheParent(grandchild);
  assert.deepEqual(events, [], 'nothing is told while the parent navigates, and the child of a held frame is held too');

  pending.clear();
  below.push(child, grandchild);
  handler._onNavigationCommitted({ frameId: 'mainframe-1', navigationId: 'nav-1', url: 'https://example.test/' });
  assert.deepEqual(events.map(([name, payload]) => [name, payload.frameId]), [
    ['Page.navigationCommitted', 'mainframe-1'],
    ['Page.frameAttached', 'subframe-2'],
    ['Page.frameAttached', 'subframe-3'],
  ], 'the commit comes first, then the frames that attached meanwhile, parents first, once');

  events.length = 0;
  handler._onNavigationCommitted({ frameId: 'mainframe-1', navigationId: 'nav-2', url: 'https://example.test/' });
  assert.deepEqual(events.map(([name]) => name), ['Page.navigationCommitted'], 'a later commit does not tell them again');
});

test('a frame that attaches while its parent does not navigate is told at once', () => {
  const { handler, events } = navigatingPage();
  handler._onFrameSeenByTheParent({ frameId: 'subframe-2', parentFrameId: 'mainframe-1' });
  assert.deepEqual(events.map(([name, payload]) => [name, payload.frameId]), [['Page.frameAttached', 'subframe-2']]);
});

test('a held frame that goes before the commit is never told, and its detach says nothing', () => {
  const { handler, events, pending, below } = navigatingPage();
  pending.add('mainframe-1');
  const child = { frameId: 'subframe-2', parentFrameId: 'mainframe-1' };
  handler._onFrameSeenByTheParent(child);
  handler._onFrameDetached('subframe-2');
  pending.clear();
  below.push(child);
  handler._onNavigationCommitted({ frameId: 'mainframe-1', navigationId: 'nav-1', url: 'https://example.test/' });
  assert.deepEqual(events.map(([name]) => name), ['Page.navigationCommitted']);
});

// The name of a frame is the name, else the id, of its <iframe>, which only the embedder's process can read. A frame that
// is the root of its own process cannot say it: the commit carries the empty name and the client could not find the frame
// by name (test_a_frame_is_found_by_the_name_or_the_id_of_its_iframe).
function namedPage({ ownRoot = true, answer }) {
  const { handler, events } = pageHandler();
  const asked = [];
  const browsingContext = { id: 7 };
  handler._pageTarget = { frameIdToBrowsingContext: frameId => (frameId === 'subframe-7' ? browsingContext : undefined) };
  handler._contentChannel = { frameRootOf: bc => (ownRoot ? bc.id : 1) };
  handler._contentPage = { send: (method, params) => { asked.push([method, params]); return answer(); } };
  handler._pageNetwork = { onMainFrameNavigationCommitted() {} };
  handler._frameNavigations = { forget() {} };
  handler._heldBackFrames = new Map();
  handler._onFrameAttached({ frameId: 'subframe-7', parentFrameId: 'mainframe-1' });
  events.length = 0;
  return { handler, events, asked };
}

test('the commit of a frame that is the root of its process waits for the name its embedder gives, and what follows waits behind it', async () => {
  let reply;
  const { handler, events, asked } = namedPage({ answer: () => new Promise(resolve => { reply = resolve; }) });
  handler._onNavigationCommitted({ frameId: 'subframe-7', navigationId: 'nav-1', url: 'https://b.test/', name: '' });
  handler._emitFrameKeyed('Page.eventFired', { frameId: 'subframe-7', name: 'load' }, 'subframe-7');
  assert.deepEqual(JSON.parse(JSON.stringify(asked)), [['frameName', { childFrameId: 'subframe-7' }]], 'the embedder is asked for the name of the child');
  assert.deepEqual(events, [], 'nothing of the frame leaves while the name is awaited');
  reply({ name: 'pay' });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(events.map(([name, payload]) => [name, payload.name]), [
    ['Page.navigationCommitted', 'pay'],
    ['Page.eventFired', 'load'],
  ], 'the commit carries the name and comes first');
});

test('a frame that is not the root of its process, or that has a name already, is committed at once', () => {
  let { handler, events, asked } = namedPage({ ownRoot: false, answer: () => Promise.resolve({ name: 'x' }) });
  handler._onNavigationCommitted({ frameId: 'subframe-7', navigationId: 'nav-1', url: 'https://b.test/', name: '' });
  assert.deepEqual(events.map(([name]) => name), ['Page.navigationCommitted']);
  assert.deepEqual(asked, []);
  ({ handler, events, asked } = namedPage({ answer: () => Promise.resolve({ name: 'x' }) }));
  handler._onNavigationCommitted({ frameId: 'subframe-7', navigationId: 'nav-1', url: 'https://b.test/', name: 'given' });
  assert.deepEqual(events.map(([name, payload]) => [name, payload.name]), [['Page.navigationCommitted', 'given']]);
  assert.deepEqual(asked, []);
});

test('an embedder that cannot say the name leaves the commit with the name it had', async () => {
  const { handler, events } = namedPage({ answer: () => Promise.reject(new Error('Frame has been detached')) });
  handler._onNavigationCommitted({ frameId: 'subframe-7', navigationId: 'nav-1', url: 'https://b.test/', name: '' });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(events.map(([name, payload]) => [name, payload.name]), [['Page.navigationCommitted', '']]);
});

// test_nested_parent_chain (strategy 1): a frame in another process, whose commit waits for the name of its <iframe>, has a
// frame of its own that attaches in the meantime. The navigation of the parent is no longer in flight (the registry
// committed it) but the client has not been told: told now, the frame is dropped by the commit that follows, and the
// client keeps a stale, unnamed Frame in the parent's child list (payment.child_frames == [Frame(url=''), widget]).
test('a frame that attaches while the commit of its parent waits for the name of the parent is told after the commit', async () => {
  let reply;
  const { handler, events } = namedPage({ answer: () => new Promise(resolve => { reply = resolve; }) });
  handler._frameNavigations = { forget() {}, pendingNavigationId: () => undefined };   // the registry has committed it
  handler._onNavigationCommitted({ frameId: 'subframe-7', navigationId: 'nav-1', url: 'https://b.test/', name: '' });
  handler._onFrameSeenByTheParent({ frameId: 'subframe-8', parentFrameId: 'subframe-7' });
  assert.deepEqual(events, [], 'nothing leaves while the name is awaited: not the commit, and not the frame below it');
  reply({ name: 'pay' });
  await new Promise(resolve => setTimeout(resolve, 0));
  assert.deepEqual(events.map(([name, payload]) => [name, payload.frameId]), [
    ['Page.navigationCommitted', 'subframe-7'],
    ['Page.frameAttached', 'subframe-8'],
  ], 'the commit first, then the frame that attached meanwhile');
});
