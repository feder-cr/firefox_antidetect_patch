// Where the commands of the steps "element <-> frame" and "geometry" go, in the parent's ContentMux, loaded
// the way the parent loads it, with the routes read from the real table.
//
//   describeNode, getFrameQuads, frameViewport   -> the process of the frame
//   frameBoxTransform, frameName                 -> the process of the PARENT of the child frame: the box and the
//                                                   attributes of an <iframe> are measured where the element is
//   adoptNode without an object                  -> the process of the execution context (the parent's world)
//   adoptNode with an object                     -> the process of the frame, and nothing is sent when the
//                                                   destination context is in another process
//   getContentQuads                              -> not a command of the content: the parent adds the boxes up
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

const { ROUTES, ContentMux, routeOf } = load('ContentMux.js', {}, ['ROUTES', 'ContentMux', 'routeOf']);
const plain = value => JSON.parse(JSON.stringify(value));

test('describeNode, getFrameQuads and frameViewport go to the frame', () => {
  for (const method of ['describeNode', 'getFrameQuads', 'frameViewport'])
    assert.deepEqual(plain(routeOf(method, { frameId: 'subframe-5' })), { kind: 'frame', id: 'subframe-5' }, method);
});

test('frameBoxTransform and frameName go to the parent of the child frame', () => {
  for (const method of ['frameBoxTransform', 'frameName'])
    assert.deepEqual(plain(routeOf(method, { childFrameId: 'subframe-5' })), { kind: 'parentOf', id: 'subframe-5' }, method);
});

test('getContentQuads is not routed: the parent assembles it from getFrameQuads and frameBoxTransform', () => {
  assert.equal('getContentQuads' in ROUTES, false);
  assert.throws(() => routeOf('getContentQuads', { frameId: 'f' }), /no route for getContentQuads/);
});

test('adoptNode goes by the context without an object and by the frame with one', () => {
  assert.deepEqual(plain(routeOf('adoptNode', { frameId: 'f', executionContextId: 7 })), { kind: 'ctx', id: 7 });
  assert.deepEqual(plain(routeOf('adoptNode', { frameId: 'f', objectId: 'o', executionContextId: 7 })), { kind: 'frame', id: 'f' });
});

// ---- the mux in action: a top (root 14, the main one), an out-of-process child of it (root 20) ----------

const top = { id: 14, browserId: 9, parent: null, currentWindowGlobal: { isProcessRoot: true } };
const child = { id: 20, browserId: 9, parent: top, currentWindowGlobal: { isProcessRoot: true } };       // other process
const local = { id: 21, browserId: 9, parent: child, currentWindowGlobal: { isProcessRoot: false } };    // same process as child
const byFrameId = { 'mainframe-9': top, 'subframe-20': child, 'subframe-21': local };

function mux() {
  const sent = [];
  const channel = rootId => ({
    bindToActor() {}, resetTransport() {}, register() {}, unregister() {}, dispose() {},
    connect: ns => ({ send: async (method, ...args) => (sent.push({ rootId, ns, method, args }), { from: rootId }), dispose() {} }),
  });
  const m = new ContentMux({
    pageTarget: { linkedBrowser: () => ({ browsingContext: top }), frameIdToBrowsingContext: id => byFrameId[id] },
    name: 'page', uid: 'u',
    createChannel: name => channel(Number(/root-(\d+)/.exec(name)[1])),
  });
  m.onRootAdded(14, top);
  m.onRootAdded(20, child);
  return { m, sent };
}

const announce = (m, rootId, executionContextId, frameId) =>
  m._onEvent('runtimeExecutionContextCreated', { executionContextId, auxData: { frameId } }, rootId, { runtimeExecutionContextCreated() {} });

test('the box of an out-of-process child is asked of the root of its parent, the quads of the root of the child', async () => {
  const { m, sent } = mux();
  const page = m.connect('page');
  await page.send('frameBoxTransform', { childFrameId: 'subframe-20' });
  await page.send('getFrameQuads', { frameId: 'subframe-20', objectId: 'o' });
  await page.send('frameViewport', { frameId: 'subframe-21' });
  await page.send('frameName', { childFrameId: 'subframe-20' });
  assert.deepEqual(sent.map(s => [s.method, s.rootId]),
                   [['frameBoxTransform', 14], ['getFrameQuads', 20], ['frameViewport', 20], ['frameName', 14]]);
});

test('a child that is in the process of its own parent is asked in that process', async () => {
  const { m, sent } = mux();
  await m.connect('page').send('frameName', { childFrameId: 'subframe-21' });
  assert.deepEqual(sent.map(s => [s.method, s.rootId]), [['frameName', 20]], 'the parent of subframe-21 is in root 20');
});

test('adoptNode of the <iframe> element goes to the process of the context it is adopted into', async () => {
  const { m, sent } = mux();
  announce(m, 14, 7, 'mainframe-9');
  await m.connect('page').send('adoptNode', { frameId: 'subframe-20', executionContextId: 7 });
  assert.deepEqual(sent.map(s => [s.method, s.rootId]), [['adoptNode', 14]]);
});

test('adoptNode of an object into a context of another process answers null and sends nothing', async () => {
  const { m, sent } = mux();
  announce(m, 14, 7, 'mainframe-9');
  const result = await m.connect('page').send('adoptNode', { frameId: 'subframe-20', objectId: 'o', executionContextId: 7 });
  assert.deepEqual(plain(result), { remoteObject: null });
  assert.deepEqual(sent, []);
  // Into a context of the same process, the object is adopted where it lives.
  announce(m, 20, 8, 'subframe-20');
  await m.connect('page').send('adoptNode', { frameId: 'subframe-20', objectId: 'o', executionContextId: 8 });
  assert.deepEqual(sent.map(s => [s.method, s.rootId]), [['adoptNode', 20]]);
});
