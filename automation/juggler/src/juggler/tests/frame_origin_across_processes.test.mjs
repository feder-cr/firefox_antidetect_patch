// Where the viewport of a frame starts, in the CSS pixels of the main document (Page.getFrameOrigin).
//
// The client turns a point of the main document into the point of a frame to ask the frame what is under it
// (the hit-target check before a click). It used to take the difference of window.mozInnerScreenX/Y of the main
// frame and of the frame. In a frame of another process (Fission, strategy 1) the two are the same: each process
// has its own widget, at the origin of the window like the top's, so the difference was zero, the point of the
// main document was asked of the frame as it was, and the answer was <html>: "the event would have landed
// elsewhere", and the click was never sent. The parent now measures the origin by the boxes of the <iframe>s.
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

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci: {},
  ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);

const { PageHandler } = load('protocol/PageHandler.js', {
  Components: { classes: {}, interfaces: {}, utils: {}, results: {} },
  ChromeUtils: { importESModule: () => new Proxy({ Helper }, { get: (modules, name) => (name in modules ? modules[name] : class {}) }) },
  dump: () => {},
}, ['PageHandler']);

const plain = value => JSON.parse(JSON.stringify(value));

// The page: main (root 1); 20 an <iframe> of another process in it; 21 an <iframe> of a third process in 20;
// 22 an <iframe> of the process of 20, inside 20 (the same process: not a root); 23 an <iframe> of the main process.
const mainBc = { id: 1, browserId: 9, parent: null };
const childBc = { id: 20, browserId: 9, parent: mainBc };
const grandBc = { id: 21, browserId: 9, parent: childBc };
const sameProcessBc = { id: 22, browserId: 9, parent: childBc };
const localBc = { id: 23, browserId: 9, parent: mainBc };
const bcOfFrame = {
  'mainframe-9': mainBc, 'subframe-20': childBc, 'subframe-21': grandBc, 'subframe-22': sameProcessBc, 'subframe-23': localBc,
};
const rootIdOfFrame = { 'mainframe-9': 1, 'subframe-20': 20, 'subframe-21': 21, 'subframe-22': 20, 'subframe-23': 1 };

// `boxes[childFrameId]` is what the embedder's process says of the <iframe> that holds the child; `viewports` the
// size of the viewport of a frame.
function handler(boxes, viewports) {
  const h = Object.create(PageHandler.prototype);
  h._workersNotToldYet = new Map();
  h._restoreBeingSaid = false;
  const sent = [];
  h._pageTarget = { frameIdToBrowsingContext: id => bcOfFrame[id] };
  h._contentChannel = { mainRootId: () => 1, rootOf: id => rootIdOfFrame[id] };
  h._contentPage = {
    send: async (method, params) => {
      sent.push([method, plain(params)]);
      if (method === 'frameViewport')
        return viewports[params.frameId];
      if (method === 'frameBoxTransform') {
        const box = boxes[params.childFrameId];
        if (!box)
          throw new Error('Failed to find frame with id = ' + params.childFrameId);
        return box;
      }
      throw new Error('unexpected ' + method);
    },
  };
  return { h, sent };
}

const box = (x, y, w, h, parentRootFrameId) => ({ O: { x, y }, Ex: { x: w, y: 0 }, Ey: { x: 0, y: h }, parentRootFrameId });
const origin = (h, frameId) => h['Page.getFrameOrigin']({ frameId }).then(plain);

test('the main frame starts at the origin of the main document, and nothing is asked', async () => {
  const { h, sent } = handler({}, {});
  assert.deepEqual(await origin(h, 'mainframe-9'), { x: 0, y: 0 });
  assert.deepEqual(sent, []);
});

test('a frame of another process starts where the content box of its <iframe> starts', async () => {
  const { h, sent } = handler({ 'subframe-20': box(68, 68, 600, 500, 'mainframe-9') }, { 'subframe-20': { width: 600, height: 500 } });
  assert.deepEqual(await origin(h, 'subframe-20'), { x: 68, y: 68 });
  assert.deepEqual(sent.map(([method]) => method), ['frameViewport', 'frameBoxTransform']);
  assert.deepEqual(sent[1][1], { childFrameId: 'subframe-20' });
});

test('a frame in an <iframe> in another process adds the boxes of both, across the boundary', async () => {
  // The widget (21) in the payment frame (20) in the top: 20 starts at (68,68) in the top; 21 at (43,38) in 20.
  const { h, sent } = handler({
    'subframe-21': box(43, 38, 400, 250, 'subframe-20'),
    'subframe-20': box(68, 68, 600, 500, 'mainframe-9'),
  }, { 'subframe-21': { width: 400, height: 250 }, 'subframe-20': { width: 600, height: 500 } });
  assert.deepEqual(await origin(h, 'subframe-21'), { x: 111, y: 106 });
  assert.deepEqual(sent.map(([method, params]) => [method, params.childFrameId || params.frameId]),
                   [['frameViewport', 'subframe-21'], ['frameBoxTransform', 'subframe-21'],
                    ['frameViewport', 'subframe-20'], ['frameBoxTransform', 'subframe-20']]);
});

test('a frame of the same process as its embedder is one step: the box of its <iframe> in the root of that process', async () => {
  // 22 sits inside 20 (the same process): its <iframe> is measured against the document of the root 20.
  const { h } = handler({
    'subframe-22': box(10, 20, 100, 50, 'subframe-20'),
    'subframe-20': box(68, 68, 600, 500, 'mainframe-9'),
  }, { 'subframe-22': { width: 100, height: 50 }, 'subframe-20': { width: 600, height: 500 } });
  assert.deepEqual(await origin(h, 'subframe-22'), { x: 78, y: 88 });
});

test('a frame of the main process is one step too, with no boundary to cross', async () => {
  const { h, sent } = handler({ 'subframe-23': box(30, 40, 200, 100, 'mainframe-9') }, { 'subframe-23': { width: 200, height: 100 } });
  assert.deepEqual(await origin(h, 'subframe-23'), { x: 30, y: 40 });
  assert.equal(sent.length, 2);
});

test('an <iframe> shown at half size puts the origin of the frame at the corner of its box', async () => {
  const { h } = handler({ 'subframe-20': box(100, 50, 200, 150, 'mainframe-9') }, { 'subframe-20': { width: 400, height: 300 } });
  assert.deepEqual(await origin(h, 'subframe-20'), { x: 100, y: 50 });
});

test('an <iframe> with no box, or a frame that is gone, has no origin: an error, not a guess', async () => {
  const { h } = handler({ 'subframe-20': { empty: true, parentRootFrameId: 'mainframe-9' } }, { 'subframe-20': { width: 600, height: 500 } });
  await assert.rejects(origin(h, 'subframe-20'), /Frame subframe-20 has no box/);
  const gone = handler({}, { 'subframe-20': { width: 600, height: 500 } });
  await assert.rejects(origin(gone.h, 'subframe-20'), /Failed to find frame with id = subframe-20/);
});

test('a frame that says it is its own parent does not loop', async () => {
  const { h } = handler({ 'subframe-20': box(1, 1, 10, 10, 'subframe-20') }, { 'subframe-20': { width: 10, height: 10 } });
  await assert.rejects(origin(h, 'subframe-20'), /is its own parent/);
});

test('the protocol declares the command, and the page handler answers it', () => {
  const protocol = fs.readFileSync(path.join(ROOT, 'protocol/Protocol.js'), 'utf8');
  assert.match(protocol, /'getFrameOrigin': \{\s*params: \{\s*frameId: t\.String,\s*\},\s*returns: \{\s*x: t\.Number,\s*y: t\.Number,/);
  assert.equal(typeof PageHandler.prototype['Page.getFrameOrigin'], 'function');
});
