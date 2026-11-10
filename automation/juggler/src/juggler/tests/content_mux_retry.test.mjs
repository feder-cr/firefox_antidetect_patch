// A command that only READS, for a frame, is asked again when the root it went to is gone (firefox-62, Fission,
// step 8.1): the frame left its process for another, or for its parent's, and the answer is the one of the root that
// holds it now. The signal is the mux's own mark on the endpoint (`gone`, set before the channel refuses what waits),
// not the text of the refusal. Commands with effects, and those that carry an object or a context, are not asked
// again: a reference belongs to a document, and "Cannot find object" is the answer for one that is gone.
//
// Loaded the way content_mux.test.mjs loads the mux, with the real SimpleChannel. No browser.
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

const { SimpleChannel } = load('SimpleChannel.js', { dump: () => {} }, ['SimpleChannel']);
const { ContentMux, ROUTES, MAX_RETRY_ATTEMPTS } = load('ContentMux.js', {}, ['ContentMux', 'ROUTES', 'MAX_RETRY_ATTEMPTS']);

const MAIN = 1;

const plain = x => JSON.parse(JSON.stringify(x));
const settle = async (n = 6) => {
  for (let i = 0; i < n; i++)
    await new Promise(resolve => setTimeout(resolve, 0));
};
const rejects = (promise, expected) => assert.rejects(Promise.resolve(promise), expected);

const bc = (id, parent, isProcessRoot) => ({
  id,
  parent,
  currentWindowGlobal: isProcessRoot === null ? null : {isProcessRoot: !!isProcessRoot},
});

// The content process of one root: a SimpleChannel whose handlers record what they were asked and answer
// `{answeredBy}` unless `behavior[method]` says otherwise.
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
  content.actor = actor;
  content.emit = (name, payload) => channel.connect('page').send(name, payload);
  return content;
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
  const w = {
    bcs, mux,
    page: mux.connect('page'),
    frame(frameId, browsingContext) { bcs.set(frameId, browsingContext); return browsingContext; },
    register() {
      return mux.register('page', Object.fromEntries(EVENT_NAMES.map(name => [name, () => {}])));
    },
    async root(rootId, {isMain = false} = {}) {
      const content = startContent('root' + rootId);
      mux.bindRoot(rootId, content.actor, {isMain});
      await settle(3);
      return content;
    },
  };
  return w;
}

const never = () => new Promise(() => {});

test('only the reads of a frame are marked to be asked again', () => {
  const retried = Object.keys(ROUTES).filter(method => ROUTES[method].retryOnRootGone).sort();
  assert.deepEqual(Array.from(retried), ['frameBoxTransform', 'frameViewport', 'getFrameQuads']);
});

test('a read for a frame whose root went and whose frame is now local goes to the root that holds it now', {timeout: 10000}, async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  const x = w.frame('fx', bc(2, t, true));          // a root of its own, in another process
  const top = await w.root(MAIN, {isMain: true});
  const other = await w.root(2);
  other.behavior.getFrameQuads = never;              // the old process does not answer
  const asked = w.page.send('getFrameQuads', {frameId: 'fx'});
  await settle();
  assert.deepEqual(other.methods(), ['getFrameQuads']);
  // the frame becomes local to the top; its root is discarded
  x.currentWindowGlobal.isProcessRoot = false;
  w.mux.disposeRoot(2);
  const answer = await asked;
  assert.equal(answer.answeredBy, 'root1');
  assert.deepEqual(top.methods(), ['getFrameQuads']);
});

test('the same for the box of a child frame, asked of the root of its parent', {timeout: 10000}, async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  const x = w.frame('fx', bc(2, t, true));
  w.frame('fy', bc(3, x, true));                      // below x, in a process of its own
  await w.root(MAIN, {isMain: true});
  const rootX = await w.root(2);
  rootX.behavior.frameBoxTransform = never;
  const asked = w.page.send('frameBoxTransform', {childFrameId: 'fy'});
  await settle();
  assert.deepEqual(rootX.methods(), ['frameBoxTransform']);
  // x becomes a frame of the top's process: the parent of y is now served by the main root
  x.currentWindowGlobal.isProcessRoot = false;
  w.mux.disposeRoot(2);
  assert.equal((await asked).answeredBy, 'root1');
});

test('a frame that is gone with its root is an error at once, not ten attempts', {timeout: 10000}, async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  w.frame('fx', bc(2, t, true));
  await w.root(MAIN, {isMain: true});
  const other = await w.root(2);
  other.behavior.getFrameQuads = never;
  const asked = w.page.send('getFrameQuads', {frameId: 'fx'});
  await settle();
  w.bcs.delete('fx');
  w.mux.disposeRoot(2);
  await rejects(asked, {message: 'Failed to find frame with id = fx'});
});

test('a command with effects, or one that carries a context, is not asked again', {timeout: 10000}, async () => {
  const w = world();
  w.register();
  const t = w.frame('ft', bc(MAIN, null, true));
  w.frame('fx', bc(2, t, true));
  const top = await w.root(MAIN, {isMain: true});
  const other = await w.root(2);
  await other.emit('runtimeExecutionContextCreated', {executionContextId: 8, auxData: {frameId: 'fx'}});
  other.behavior.evaluate = never;
  other.behavior.describeNode = never;
  other.behavior.setUserInput = never;
  const results = [
    w.page.send('evaluate', {executionContextId: 8, expression: '1'}),
    w.page.send('describeNode', {frameId: 'fx'}),
    w.page.send('setUserInput', {frameId: 'fx'}),
  ].map(promise => promise.then(() => 'answered', e => e.message));
  await settle();
  w.mux.disposeRoot(2);
  for (const result of await Promise.all(results))
    assert.match(result, /is disposed/);
  assert.deepEqual(top.methods(), [], 'nothing was sent to another root');
});

test('the attempts are bounded: a frame whose root keeps going is refused after MAX_RETRY_ATTEMPTS', {timeout: 10000}, async () => {
  let asks = 0;
  const pageTarget = {
    linkedBrowser: () => ({browsingContext: {id: MAIN}}),
    // each look finds the frame under a new root, which is gone as soon as it is waited on
    frameIdToBrowsingContext: () => {
      asks++;
      const id = 100 + asks;
      setTimeout(() => mux.disposeRoot(id), 0);
      return bc(id, bc(MAIN, null, true), true);
    },
  };
  const mux = new ContentMux({pageTarget, name: 'page', uid: 'parent', createChannel: (name, uid) => new SimpleChannel(name, uid)});
  await rejects(mux.connect('page').send('getFrameQuads', {frameId: 'fx'}), /is disposed/);
  assert.equal(asks, MAX_RETRY_ATTEMPTS + 1);
});

test('the root is marked gone before its channel refuses what waits, and the nextTask of the mux is used between attempts', {timeout: 10000}, async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  const x = w.frame('fx', bc(2, t, true));
  await w.root(MAIN, {isMain: true});
  const other = await w.root(2);
  other.behavior.getFrameQuads = never;
  const endpoint = w.mux.endpointFor(2);
  assert.equal(endpoint.gone, false);
  let tasks = 0;
  w.mux._nextTask = callback => { tasks++; Promise.resolve().then(callback); };
  const asked = w.page.send('getFrameQuads', {frameId: 'fx'});
  await settle();
  x.currentWindowGlobal.isProcessRoot = false;
  w.mux.disposeRoot(2);
  await asked;
  assert.equal(endpoint.gone, true);
  assert.equal(tasks, 1);
});
