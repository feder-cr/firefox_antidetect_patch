// The geometry of an element in a frame that lives in another process (Fission, strategy 1), loaded the way the
// content process loads PageAgent.js and the parent loads PageHandler.js.
//
// The client wants the quads of a node in the CSS pixels of the top document, the viewport of the top as origin,
// scroll applied (they are what getBoundingClientRect of the top would say). The layout can give that in one
// call only inside one process. Across a process boundary the parent adds the boxes up:
//   - getFrameQuads: the quads of the node in the document of the fragment root of its own process;
//   - frameBoxTransform, in the process of the EMBEDDER: the content box of the <iframe> as an affine map from the
//     child's viewport to the embedder's fragment root; no compositor matrix, which lags after a scroll;
//   - frameViewport: the size of the child's viewport, the unit of that map;
//   - Page.getContentQuads (PageHandler): one getFrameQuads, then one frameBoxTransform per process boundary
//     up to the main root. In the main root it is one call, the numbers of before.
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
const helper = new Helper();

const { FrameTree } = load('content/FrameTree.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }) },
  dump: () => {},
}, ['FrameTree']);

const { PageAgent } = load('content/PageAgent.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  Cc: new Proxy({}, { get: () => ({ getService: () => ({}) }) }),
  ChromeUtils: { importESModule: () => ({ Helper, NetUtil: {}, setTimeout }) },
  dump: () => {},
}, ['PageAgent']);

const plain = value => JSON.parse(JSON.stringify(value));
const quad = (x, y, w, h) => ({ p1: { x, y }, p2: { x: x + w, y }, p3: { x: x + w, y: y + h }, p4: { x, y: y + h } });

function bc(id, parent, embedderElement = null) {
  const b = { id, browserId: 9, parent: parent || null, children: [], embedderElement };
  if (parent)
    parent.children.push(b);
  return b;
}

// ---- frameBoxTransform and frameViewport, in the process of the embedder ----------------------------

// The embedder's process: the top (a.test) is its fragment root and its only Frame. The <iframe> #payment
// holds b.test, out of process, so it is only a browsing context here.
const top = bc(14, null);
const document = { name: 'top document' };
const asked = [];
const payment = { getBoxQuads: options => (asked.push(options), [quad(220, 150, 400, 300)]) };
const paymentBc = bc(20, top, payment);
const PAYMENT = helper.browsingContextToFrameId(paymentBc);
const MAIN = helper.browsingContextToFrameId(top);

function embedderAgent() {
  const mainFrame = {
    id: () => MAIN,
    domWindow: () => ({ document, innerWidth: 1280, innerHeight: 720 }),
  };
  const agent = Object.create(PageAgent.prototype);
  agent._frameTree = {
    _rootBrowsingContext: top,
    browsingContextOf: FrameTree.prototype.browsingContextOf,
    mainFrame: () => mainFrame,
    frame: id => (id === MAIN ? mainFrame : null),
  };
  return agent;
}

test('the box of an out-of-process frame is the content box of its <iframe> in the embedder\'s root', () => {
  asked.length = 0;
  const transform = plain(embedderAgent()._frameBoxTransform({ childFrameId: PAYMENT }));
  assert.deepEqual(transform, {
    O: { x: 220, y: 150 }, Ex: { x: 400, y: 0 }, Ey: { x: 0, y: 300 }, parentRootFrameId: MAIN,
  });
  assert.equal(asked.length, 1, 'one layout call');
  assert.equal(asked[0].box, 'content');
  assert.equal(asked[0].relativeTo, document, 'relative to the document of the fragment root, so a local chain is one call');
});

test('a rotated and scaled <iframe> gives the axes of its box', () => {
  const rotated = bc(22, top, { getBoxQuads: () => [{ p1: { x: 10, y: 20 }, p2: { x: 10, y: 120 }, p3: { x: -40, y: 120 }, p4: { x: -40, y: 20 } }] });
  const transform = plain(embedderAgent()._frameBoxTransform({ childFrameId: helper.browsingContextToFrameId(rotated) }));
  assert.deepEqual(transform.Ex, { x: 0, y: 100 });
  assert.deepEqual(transform.Ey, { x: -50, y: 0 });
});

test('an <iframe> with no box answers empty, and still says which root it is in', () => {
  const hidden = bc(23, top, { getBoxQuads: () => [] });
  assert.deepEqual(plain(embedderAgent()._frameBoxTransform({ childFrameId: helper.browsingContextToFrameId(hidden) })),
                   { empty: true, parentRootFrameId: MAIN });
});

test('the box of a detached or unknown frame is an error', () => {
  const agent = embedderAgent();
  assert.throws(() => agent._frameBoxTransform({ childFrameId: helper.browsingContextToFrameId(bc(24, top, null)) }), /Frame has been detached/);
  assert.throws(() => agent._frameBoxTransform({ childFrameId: 'subframe-999' }), /Failed to find frame with id = subframe-999/);
});

test('the viewport of a frame is the inner size of its window', () => {
  const agent = embedderAgent();
  assert.deepEqual(plain(agent._frameViewport({ frameId: MAIN })), { width: 1280, height: 720 });
  assert.throws(() => agent._frameViewport({ frameId: 'subframe-999' }), /Failed to find frame with id = subframe-999/);
});

test('the agent registers the commands the mux routes to it', () => {
  const source = fs.readFileSync(path.join(ROOT, 'content/PageAgent.js'), 'utf8');
  for (const name of ['frameBoxTransform', 'frameViewport', 'frameName'])
    assert.ok(source.includes(`${name}: this._${name}.bind(this)`), name);
});

// ---- getFrameQuads: the node in the document of the fragment root of its process ----------------------

function nodeAgent({ rootFrameId = MAIN, innerWidth = 1280, innerHeight = 720, quads = [quad(10, 20, 40, 30)] } = {}) {
  const options = [];
  const rootDocument = { name: 'root document' };
  const rootFrame = { id: () => rootFrameId, domWindow: () => ({ document: rootDocument, innerWidth, innerHeight }) };
  const node = { getBoxQuads: o => (options.push(o), quads) };
  const frame = { unsafeObject: objectId => (objectId === 'o' ? node : {}) };
  const agent = Object.create(PageAgent.prototype);
  agent._frameTree = { mainFrame: () => rootFrame, frame: id => (id === 'f' ? frame : null) };
  return { agent, options, rootDocument };
}

test('the quads of a node are those of the fragment root of its process, with the root and its viewport', () => {
  const { agent, options, rootDocument } = nodeAgent({ rootFrameId: 'subframe-20', innerWidth: 400, innerHeight: 300 });
  const answer = plain(agent._getFrameQuads({ objectId: 'o', frameId: 'f' }));
  assert.deepEqual(answer, {
    quads: [{ p1: { x: 10, y: 20 }, p2: { x: 50, y: 20 }, p3: { x: 50, y: 50 }, p4: { x: 10, y: 50 } }],
    rootFrameId: 'subframe-20',
    viewport: { width: 400, height: 300 },
  });
  assert.equal(options.length, 1, 'one layout call');
  assert.equal(options[0].relativeTo, rootDocument);
  assert.equal('recurseWhenNoFrame' in options[0], false, 'a key BoxQuadOptions does not have');
});

test('the quads of something that is not a node, or of an unknown frame, are an error', () => {
  const { agent } = nodeAgent();
  assert.throws(() => agent._getFrameQuads({ objectId: 'x', frameId: 'f' }), /RemoteObject is not a node/);
  assert.throws(() => agent._getFrameQuads({ objectId: 'o', frameId: 'nope' }), /Failed to find frame with id = nope/);
});

test('a node with no box has no quads', () => {
  const { agent } = nodeAgent({ quads: [] });
  assert.deepEqual(plain(agent._getFrameQuads({ objectId: 'o', frameId: 'f' }).quads), []);
});

test('nothing in the agent measures a node against the main document of another process', () => {
  const source = fs.readFileSync(path.join(ROOT, 'content/PageAgent.js'), 'utf8');
  assert.equal(source.includes('_getNodeBoundingBox'), false, 'a dead method that did');
  assert.equal(source.includes('recurseWhenNoFrame'), false);
  assert.ok(source.includes('getFrameQuads: this._getFrameQuads.bind(this)'));
  assert.equal(source.includes('getContentQuads: this'), false);
});

// ---- Page.getContentQuads in the parent: the quads carried up to the main root -------------------------

const { PageHandler } = load('protocol/PageHandler.js', {
  Components: { classes: {}, interfaces: {}, utils: {}, results: {} },
  ChromeUtils: { importESModule: () => new Proxy({ Helper }, { get: (modules, name) => (name in modules ? modules[name] : class {}) }) },
  dump: () => {},
}, ['PageHandler']);

// The page's roots: the main root is 1; 'subframe-20' is a root in another process (root 20) inside it;
// 'subframe-21' is a root (root 21) inside that one (a -> b -> a). `answers[method]` is what each command says.
const rootOfFrame = { 'mainframe-9': 1, 'subframe-20': 20, 'subframe-21': 21 };
function handler(answers) {
  const h = Object.create(PageHandler.prototype);
  h._workersNotToldYet = new Map();
  h._restoreBeingSaid = false;
  const sent = [];
  h._contentChannel = { mainRootId: () => 1, rootOf: id => rootOfFrame[id] };
  h._contentPage = {
    send: async (method, params) => {
      sent.push([method, plain(params)]);
      const answer = answers[method];
      if (!answer)
        throw new Error('no answer for ' + method);
      return typeof answer === 'function' ? answer(params) : answer;
    },
  };
  return { h, sent };
}
const call = h => h['Page.getContentQuads']({ frameId: 'f', objectId: 'o' });

test('a node of the main root costs one command, and its quads are the ones the page measured', async () => {
  const quads = [quad(100, 200, 50, 20)];
  const { h, sent } = handler({ getFrameQuads: { quads, rootFrameId: 'mainframe-9', viewport: { width: 1280, height: 720 } } });
  assert.deepEqual(plain(await call(h)), { quads: plain(quads) });
  assert.deepEqual(sent.map(([method]) => method), ['getFrameQuads']);
});

test('a node of an out-of-process frame is carried into the main root by the box of its <iframe>', async () => {
  // The node is at (10,20)-(50,50) in a child viewport of 400x300; the <iframe> content box is at (220,150), 400x300.
  const { h, sent } = handler({
    getFrameQuads: { quads: [quad(10, 20, 40, 30)], rootFrameId: 'subframe-20', viewport: { width: 400, height: 300 } },
    frameBoxTransform: { O: { x: 220, y: 150 }, Ex: { x: 400, y: 0 }, Ey: { x: 0, y: 300 }, parentRootFrameId: 'mainframe-9' },
  });
  assert.deepEqual(plain(await call(h)), { quads: [quad(230, 170, 40, 30)] });
  assert.deepEqual(sent.map(([method]) => method), ['getFrameQuads', 'frameBoxTransform']);
  assert.deepEqual(sent[1][1], { childFrameId: 'subframe-20' }, 'the box is asked for the root, not for the frame of the node');
});

test('an <iframe> shown at half size moves and scales the quads with it', async () => {
  // Child viewport 400x300 shown in a 200x150 box at (100,50): a point (x,y) lands at (100+x/2, 50+y/2).
  const { h } = handler({
    getFrameQuads: { quads: [quad(40, 60, 80, 20)], rootFrameId: 'subframe-20', viewport: { width: 400, height: 300 } },
    frameBoxTransform: { O: { x: 100, y: 50 }, Ex: { x: 200, y: 0 }, Ey: { x: 0, y: 150 }, parentRootFrameId: 'mainframe-9' },
  });
  assert.deepEqual(plain(await call(h)), { quads: [quad(120, 80, 40, 10)] });
});

test('a rotated <iframe> carries the quads along its axes', async () => {
  // Quarter turn: the child's x axis goes down the page, its y axis goes left.
  const { h } = handler({
    getFrameQuads: { quads: [{ p1: { x: 100, y: 0 }, p2: { x: 100, y: 0 }, p3: { x: 100, y: 0 }, p4: { x: 100, y: 0 } }],
                     rootFrameId: 'subframe-20', viewport: { width: 200, height: 100 } },
    frameBoxTransform: { O: { x: 300, y: 10 }, Ex: { x: 0, y: 200 }, Ey: { x: -100, y: 0 }, parentRootFrameId: 'mainframe-9' },
  });
  assert.deepEqual(plain((await call(h)).quads[0].p1), { x: 300, y: 110 });
});

test('a -> b -> a is carried across two boundaries, the viewport of the middle root read on the way', async () => {
  const { h, sent } = handler({
    getFrameQuads: { quads: [quad(20, 10, 40, 10)], rootFrameId: 'subframe-21', viewport: { width: 200, height: 100 } },
    frameBoxTransform: params => params.childFrameId === 'subframe-21'
      // the <iframe> of the grandchild in the middle root, 100x50 at (50,60)
      ? { O: { x: 50, y: 60 }, Ex: { x: 100, y: 0 }, Ey: { x: 0, y: 50 }, parentRootFrameId: 'subframe-20' }
      // the <iframe> of the middle root in the top, 400x300 at (220,150)
      : { O: { x: 220, y: 150 }, Ex: { x: 400, y: 0 }, Ey: { x: 0, y: 300 }, parentRootFrameId: 'mainframe-9' },
    frameViewport: { width: 400, height: 300 },
  });
  // (20,10) -> (50+10, 60+5) = (60,65) in the middle root -> (220+60, 150+65) = (280,215).
  assert.deepEqual(plain((await call(h)).quads[0].p1), { x: 280, y: 215 });
  assert.deepEqual(sent.map(([method, params]) => [method, params.childFrameId || params.frameId]),
                   [['getFrameQuads', 'f'], ['frameBoxTransform', 'subframe-21'], ['frameViewport', 'subframe-20'],
                    ['frameBoxTransform', 'subframe-20']]);
});

test('an <iframe> with no box has no quads, as an element that is not shown', async () => {
  const { h } = handler({
    getFrameQuads: { quads: [quad(10, 20, 40, 30)], rootFrameId: 'subframe-20', viewport: { width: 400, height: 300 } },
    frameBoxTransform: { empty: true, parentRootFrameId: 'mainframe-9' },
  });
  assert.deepEqual(plain(await call(h)), { quads: [] });
});

test('a failure on the way is the answer: no half-carried quads', async () => {
  const { h } = handler({
    getFrameQuads: { quads: [quad(10, 20, 40, 30)], rootFrameId: 'subframe-20', viewport: { width: 400, height: 300 } },
    frameBoxTransform: () => { throw new Error('Failed to find frame with id = subframe-20'); },
  });
  await assert.rejects(call(h), /Failed to find frame with id = subframe-20/);
  const { h: sized } = handler({
    getFrameQuads: { quads: [quad(10, 20, 40, 30)], rootFrameId: 'subframe-20', viewport: { width: 0, height: 300 } },
    frameBoxTransform: { O: { x: 0, y: 0 }, Ex: { x: 1, y: 0 }, Ey: { x: 0, y: 1 }, parentRootFrameId: 'mainframe-9' },
  });
  await assert.rejects(call(sized), /Frame has no size/);
});

test('a root the mux does not know yet is a retry, not a guess', async () => {
  const { h } = handler({
    getFrameQuads: { quads: [quad(10, 20, 40, 30)], rootFrameId: 'subframe-99', viewport: { width: 400, height: 300 } },
  });
  h._contentChannel.rootOf = () => null;
  await assert.rejects(call(h), /root of frame subframe-99 unknown, retry/);
});

// ---- Page.scrollIntoViewIfNeeded: the answer waits for the embedders ----------------------------------

// Browsing contexts as the parent holds them: main (14, root 1 for the mux), the out-of-process child 20, and
// 21 inside it in another process again (a -> b -> a); 22 is a frame in the same process as 20.
const mainBc = { id: 1, browserId: 9, parent: null };
const childBc = { id: 20, browserId: 9, parent: mainBc };
const grandBc = { id: 21, browserId: 9, parent: childBc };
const sameProcessBc = { id: 22, browserId: 9, parent: childBc };
const bcOfFrame = { 'mainframe-9': mainBc, 'subframe-20': childBc, 'subframe-21': grandBc, 'subframe-22': sameProcessBc };
const rootIdOfFrame = { 'mainframe-9': 1, 'subframe-20': 20, 'subframe-21': 21, 'subframe-22': 20 };

function scroller() {
  const h = Object.create(PageHandler.prototype);
  h._workersNotToldYet = new Map();
  h._restoreBeingSaid = false;
  const log = [];
  let scrolled = false;
  h._pageTarget = { frameIdToBrowsingContext: id => bcOfFrame[id] };
  h._contentChannel = { mainRootId: () => 1, rootOf: id => rootIdOfFrame[id] };
  h._contentPage = {
    send: async (method, params) => {
      log.push([method, plain(params)]);
      if (method === 'scrollIntoViewIfNeeded') {
        // The command answers on a later turn: what it sends the embedder of its frame is already on its way.
        await new Promise(resolve => setImmediate(resolve));
        scrolled = true;
        return { scrolled: 'result' };
      }
      assert.equal(scrolled, true, method + ' is asked only after the node has scrolled');
      return { parentRootFrameId: params.childFrameId === 'subframe-21' ? 'subframe-20' : 'mainframe-9' };
    },
  };
  return { h, log };
}

test('a node in the main root scrolls with one command and nothing waits', async () => {
  const { h, log } = scroller();
  const result = await h['Page.scrollIntoViewIfNeeded']({ frameId: 'mainframe-9', objectId: 'o' });
  assert.deepEqual(plain(result), { scrolled: 'result' });
  assert.deepEqual(log.map(([method]) => method), ['scrollIntoViewIfNeeded']);
});

test('a node in an out-of-process frame is answered after its embedder has been asked', async () => {
  const { h, log } = scroller();
  const result = await h['Page.scrollIntoViewIfNeeded']({ frameId: 'subframe-20', objectId: 'o' });
  assert.deepEqual(plain(result), { scrolled: 'result' }, 'the answer is the scroll\'s');
  assert.deepEqual(log, [
    ['scrollIntoViewIfNeeded', { frameId: 'subframe-20', objectId: 'o' }],
    ['frameBoxTransform', { childFrameId: 'subframe-20' }],
  ]);
});

test('a frame in the process of an out-of-process root waits for the same one embedder', async () => {
  const { h, log } = scroller();
  await h['Page.scrollIntoViewIfNeeded']({ frameId: 'subframe-22', objectId: 'o' });
  assert.deepEqual(log.map(([method, params]) => [method, params.childFrameId]),
                   [['scrollIntoViewIfNeeded', undefined], ['frameBoxTransform', 'subframe-20']],
                   'the embedder is the one of the root, not of the frame');
});

test('a -> b -> a waits for each embedder going up, one after the other', async () => {
  const { h, log } = scroller();
  await h['Page.scrollIntoViewIfNeeded']({ frameId: 'subframe-21', objectId: 'o' });
  assert.deepEqual(log.map(([method, params]) => [method, params.childFrameId]),
                   [['scrollIntoViewIfNeeded', undefined], ['frameBoxTransform', 'subframe-21'], ['frameBoxTransform', 'subframe-20']]);
});

test('an embedder that is gone fails the command, as a node that is gone does', async () => {
  const { h } = scroller();
  const send = h._contentPage.send;
  h._contentPage.send = async (method, params) => {
    if (method === 'frameBoxTransform')
      throw new Error('Failed to find frame with id = ' + params.childFrameId);
    return send(method, params);
  };
  await assert.rejects(h['Page.scrollIntoViewIfNeeded']({ frameId: 'subframe-20', objectId: 'o' }), /Failed to find frame with id = subframe-20/);
});
