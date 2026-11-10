// The roots of a page end when they should, and a command for a frame waits for its root (firefox-62, Fission; the
// review of fission/integration e51023ea3d).
//
//   F1  a connector that lives as long as the page's handler does not keep the roots that ended;
//   F2  a root whose frame turned local (its actor goes, its browsing context stays) is ended, so what waits on it is
//       asked again of the root that serves the frame now;
//   F4  the roots of a crashed frame and of the frames below it end;
//   #10 a command for a frame whose root is not known yet (the window global of a context in navigation is not there)
//       waits for it, whatever the command: nothing has been sent.
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
const { ContentMux, ROOT_UNKNOWN_ATTEMPTS } = load('ContentMux.js', {}, ['ContentMux', 'ROOT_UNKNOWN_ATTEMPTS']);

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
  channel.register('page', handler);
  channel.register('', handler);
  content.methods = () => content.calls.map(call => call.method);
  const actor = {
    browsingContext: null,
    actorName: 'JugglerFrame',
    receiveMessage() {},
    sendAsyncMessage(name, obj) {
      queueMicrotask(() => channel._onMessage(structuredClone(obj)));
    },
  };
  channel.setTransport({
    sendMessage: obj => actor.receiveMessage({data: structuredClone(obj)}),
    dispose() {},
  });
  content.actor = actor;
  return content;
}

function world() {
  const bcs = new Map();
  const pageTarget = {
    linkedBrowser: () => ({browsingContext: {id: MAIN}}),
    frameIdToBrowsingContext: id => bcs.get(id),
  };
  const mux = new ContentMux({pageTarget, name: 'page', uid: 'parent', createChannel: (name, uid) => new SimpleChannel(name, uid)});
  return {
    bcs, mux,
    page: mux.connect('page'),
    frame(frameId, browsingContext) { bcs.set(frameId, browsingContext); return browsingContext; },
    async root(rootId, {isMain = false} = {}) {
      const content = startContent('root' + rootId);
      mux.bindRoot(rootId, content.actor, {isMain});
      await settle(3);
      return content;
    },
  };
}

const never = () => new Promise(() => {});

test('F1 a connector that lives as long as the page does not keep the endpoint of a root that ended', async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  w.frame('fx', bc(2, t, true));
  await w.root(MAIN, {isMain: true});
  await w.root(2);
  let captured;
  const original = w.mux._connectorOn.bind(w.mux);
  w.mux._connectorOn = (endpoint, connector) => { captured = connector; return original(endpoint, connector); };
  await w.page.send('getFrameQuads', {frameId: 'fx'});
  assert.equal(captured.owns.size, 1);
  w.mux.disposeRoot(2);                                   // the iframe is removed
  await w.page.send('getFrameQuads', {frameId: 'ft'});    // the next use lets go of it
  assert.equal(captured.owns.size, 1, 'only the main root is held');
});

test('F2 a root whose frame turned local is ended, and the read waiting on it goes to the root that serves the frame', async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  const x = w.frame('fx', bc(2, t, true));
  const top = await w.root(MAIN, {isMain: true});
  const other = await w.root(2);
  other.behavior.getFrameQuads = never;
  const asked = w.page.send('getFrameQuads', {frameId: 'fx'});
  await settle();
  // the iframe navigated to the site of its parent: its actor goes, a document that is not a process root comes
  x.currentWindowGlobal.isProcessRoot = false;
  w.mux.unbindRoot(2, other.actor);
  w.mux.onRootTurnedLocal(2);
  assert.equal((await asked).answeredBy, 'root1');
  assert.deepEqual(top.methods(), ['getFrameQuads']);
});

test('F2b the main root is never ended by a local document', async () => {
  const w = world();
  w.frame('ft', bc(MAIN, null, true));
  await w.root(MAIN, {isMain: true});
  w.mux.onRootTurnedLocal(MAIN);
  assert.ok(w.mux.endpointFor(MAIN));
});

test('F4 the roots of a crashed frame and of the frames below it end; the others stay', async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  const x = w.frame('fx', bc(2, t, true));
  w.frame('fy', bc(3, x, true));                  // below x, in a process of its own
  w.frame('fz', bc(4, t, true));                  // a sibling
  await w.root(MAIN, {isMain: true});
  await w.root(2);
  await w.root(3);
  await w.root(4);
  w.mux.onRootAdded(2, x);
  w.mux.onRootAdded(3, w.bcs.get('fy'));
  w.mux.onRootAdded(4, w.bcs.get('fz'));
  w.mux.disposeRootsOf(x);
  assert.deepEqual(Array.from(w.mux.rootIds()).sort(), [MAIN, 4]);
});

test('#10 a command for a frame whose window global is not there yet waits for it, any command', async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  const x = w.frame('fx', bc(2, t, true));
  await w.root(MAIN, {isMain: true});
  const other = await w.root(2);
  let looks = 0;
  w.mux._wait = async () => { if (++looks === 3) x.currentWindowGlobal = {isProcessRoot: true}; };
  for (const [method, params] of [['describeNode', {frameId: 'fx', objectId: 'o'}], ['setUserInput', {frameId: 'fx'}],
                                  ['getFrameQuads', {frameId: 'fx'}]]) {
    looks = 0;
    x.currentWindowGlobal = null;                 // in navigation
    assert.equal((await w.page.send(method, params)).answeredBy, 'root2', method);
    assert.equal(looks, 3, method);
  }
  assert.deepEqual(other.methods(), ['describeNode', 'setUserInput', 'getFrameQuads']);
});

test('#10 the box of a child frame waits for the root of its parent, the same way', async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  const x = w.frame('fx', bc(2, t, true));
  w.frame('fy', bc(3, x, true));
  x.currentWindowGlobal = null;
  await w.root(MAIN, {isMain: true});
  const rootX = await w.root(2);
  w.mux._wait = async () => { x.currentWindowGlobal = {isProcessRoot: true}; };
  assert.equal((await w.page.send('frameBoxTransform', {childFrameId: 'fy'})).answeredBy, 'root2');
  assert.deepEqual(rootX.methods(), ['frameBoxTransform']);
});

test('#10 a root that stays unknown is an error after ROOT_UNKNOWN_ATTEMPTS looks, and nothing was sent', async () => {
  const w = world();
  const t = w.frame('ft', bc(MAIN, null, true));
  const x = w.frame('fx', bc(2, t, true));
  x.currentWindowGlobal = null;
  const top = await w.root(MAIN, {isMain: true});
  let looks = 0;
  w.mux._wait = async () => { looks++; };
  await rejects(w.page.send('describeNode', {frameId: 'fx', objectId: 'o'}), {message: 'root of frame fx unknown, retry'});
  assert.equal(looks, ROOT_UNKNOWN_ATTEMPTS);
  assert.deepEqual(top.methods(), []);
});
