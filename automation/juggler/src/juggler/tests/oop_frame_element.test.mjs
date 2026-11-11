// The <iframe> element of a frame and the frame of an <iframe> element, when the frame lives in another
// process (Fission, strategy 1). Loaded the way the content process loads PageAgent.js and FrameTree.js,
// with the real Helper that names frames.
//
// In the process of the embedder an out-of-process child has no Frame in the FrameTree (it has no docShell
// there); it is only a BrowsingContext, which every process holds for the whole tree. So the element behind
// a frame, and the frame behind an element, must be found through the BrowsingContext:
//   - Page.adoptNode without an object (the client's frame_element()) reads `bc.embedderElement`;
//   - Page.describeNode (content_frame(), frame_locator) reads `element.browsingContext`;
//   - `frameName` reads the name of the <iframe> in the embedder's process;
//   - the frame the client is told about must already be announced to it.
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

// The browsing context tree of a page, as every process holds it: a top (a.test), an <iframe> of b.test
// that is out of process here, and the <iframe> of a.test inside it. `embedderElement` is what a process
// holds for a child whose embedder is in it.
function bc(id, parent, embedderElement = null) {
  const b = { id, browserId: 9, parent: parent || null, children: [], embedderElement };
  if (parent)
    parent.children.push(b);
  return b;
}
const top = bc(14, null);
const payment = { id: 'payment', nodePrincipal: 'a.test', getAttribute: n => ({ name: 'pay', id: 'payment' })[n] || null };
const widget = { id: 'widget', nodePrincipal: 'b.test', getAttribute: n => ({ id: 'widget' })[n] || null };
const paymentBc = bc(20, top, payment);      // b.test, in another process
const widgetBc = bc(21, paymentBc, widget);  // a.test, inside it
const PAYMENT = helper.browsingContextToFrameId(paymentBc);
const WIDGET = helper.browsingContextToFrameId(widgetBc);
const MAIN = helper.browsingContextToFrameId(top);

// ---- FrameTree.browsingContextOf ------------------------------------------------------------

test('the browsing context of a frame id is found in the whole tree, local or not', () => {
  const t = { _rootBrowsingContext: top };
  assert.equal(PAYMENT, 'subframe-20');
  assert.equal(FrameTree.prototype.browsingContextOf.call(t, PAYMENT), paymentBc);
  assert.equal(FrameTree.prototype.browsingContextOf.call(t, WIDGET), widgetBc);
  assert.equal(FrameTree.prototype.browsingContextOf.call(t, MAIN), top);
  assert.equal(FrameTree.prototype.browsingContextOf.call(t, 'subframe-999'), undefined);
});

// ---- Page.adoptNode without an object: the <iframe> of a frame --------------------------------

// The agent of the embedder's process. Its FrameTree knows only the frames that have a docShell here:
// the top. The child is a BrowsingContext and nothing else.
function embedderAgent() {
  const toFrame = { domWindow: () => ({ document: { nodePrincipal: { subsumes: p => p === 'b.test' || p === 'a.test' } } }) };
  const adopted = [];
  const context = {
    auxData: () => ({ frameId: MAIN }),
    rawValueToRemoteObject: v => (adopted.push(v), { objectId: 'remote-' + v.id }),
  };
  const agent = Object.create(PageAgent.prototype);
  agent._frameTree = {
    _rootBrowsingContext: top,
    browsingContextOf: FrameTree.prototype.browsingContextOf,
    frame: id => (id === MAIN ? toFrame : null),
  };
  agent._runtime = { findExecutionContext: () => context };
  return { agent, adopted };
}

test('adopting the element of an out-of-process frame reads it from the browsing context', async () => {
  const { agent, adopted } = embedderAgent();
  const result = await agent._adoptNode({ frameId: PAYMENT, executionContextId: 'ctx' });
  assert.deepEqual(adopted, [payment]);
  assert.equal(result.remoteObject.objectId, 'remote-payment');
});

test('adopting the element of a frame nested in an out-of-process frame', async () => {
  const { agent } = embedderAgent();
  const result = await agent._adoptNode({ frameId: WIDGET, executionContextId: 'ctx' });
  assert.equal(result.remoteObject.objectId, 'remote-widget');
});

test('a frame with no embedder element has been detached', async () => {
  const { agent } = embedderAgent();
  const gone = bc(30, top, null);
  await assert.rejects(agent._adoptNode({ frameId: helper.browsingContextToFrameId(gone), executionContextId: 'ctx' }),
                       /Frame has been detached/);
});

test('a frame that is not in the tree is unknown', async () => {
  const { agent } = embedderAgent();
  await assert.rejects(agent._adoptNode({ frameId: 'subframe-999', executionContextId: 'ctx' }),
                       /Failed to find frame with id = subframe-999/);
});

test('adopting an object of a frame still reads it from the frame that holds it', async () => {
  const { agent } = embedderAgent();
  const node = { id: 'node', nodePrincipal: 'a.test' };
  agent._frameTree.frame = id => (id === MAIN ? {
    unsafeObject: objectId => (objectId === 'o' ? node : null),
    domWindow: () => ({ document: { nodePrincipal: { subsumes: () => true } } }),
  } : null);
  const result = await agent._adoptNode({ frameId: MAIN, objectId: 'o', executionContextId: 'ctx' });
  assert.equal(result.remoteObject.objectId, 'remote-node');
  await assert.rejects(agent._adoptNode({ frameId: PAYMENT, objectId: 'o', executionContextId: 'ctx' }),
                       /Failed to find frame with id = subframe-20/);
});

// ---- Page.describeNode: the frame of an <iframe>, the frame an element sits in ------------------

// The embedder's process: the top is the one local frame (with a document); the frame list may also hold a
// frame that has no docShell yet. The <iframe> element carries the browsing context of its content.
function describeAgent({ extraFrames = [] } = {}) {
  const doc = { name: 'top document' };
  const mainFrame = { id: () => MAIN, domWindow: () => ({ document: doc }), docShell: () => ({ browsingContext: top }) };
  const frames = [mainFrame, ...extraFrames];
  const nodes = new Map();
  mainFrame.unsafeObject = objectId => nodes.get(objectId);
  const agent = Object.create(PageAgent.prototype);
  agent._frameTree = {
    _rootBrowsingContext: top,
    frame: id => (id === MAIN ? mainFrame : null),
    frames: () => frames,
    // The group-wide walk of old: it reads the document of every frame it is given.
    allFramesInBrowsingContextGroup: () => frames,
  };
  return { agent, doc, nodes };
}

test('the content frame of an out-of-process <iframe> is named from its browsing context', () => {
  const { agent, doc, nodes } = describeAgent();
  // contentWindow is a WindowProxy of a remote frame: no docShell in this process.
  nodes.set('iframe', { browsingContext: paymentBc, contentWindow: { docShell: undefined }, ownerDocument: doc });
  assert.deepEqual(JSON.parse(JSON.stringify(agent._describeNode({ objectId: 'iframe', frameId: MAIN }))),
                   { contentFrameId: PAYMENT, ownerFrameId: MAIN });
});

test('a frame without a docShell here does not stop the description', () => {
  const placeholder = { id: () => PAYMENT, domWindow: () => undefined, docShell: () => null };
  const { agent, doc, nodes } = describeAgent({ extraFrames: [placeholder] });
  nodes.set('iframe', { browsingContext: paymentBc, contentWindow: {}, ownerDocument: doc });
  assert.deepEqual(JSON.parse(JSON.stringify(agent._describeNode({ objectId: 'iframe', frameId: MAIN }))),
                   { contentFrameId: PAYMENT, ownerFrameId: MAIN });
});

test('an element that holds no frame has no content frame, and says which frame it is in', () => {
  const { agent, doc, nodes } = describeAgent();
  nodes.set('div', { ownerDocument: doc });
  const described = agent._describeNode({ objectId: 'div', frameId: MAIN });
  assert.equal(described.contentFrameId, undefined);
  assert.equal(described.ownerFrameId, MAIN);
  // An <object> or <embed> with nothing loaded has no browsing context.
  nodes.set('object', { browsingContext: null, ownerDocument: doc });
  assert.equal(agent._describeNode({ objectId: 'object', frameId: MAIN }).contentFrameId, undefined);
});

test('the document itself is owned by its frame', () => {
  const { agent, doc, nodes } = describeAgent();
  nodes.set('doc', doc);
  assert.equal(agent._describeNode({ objectId: 'doc', frameId: MAIN }).ownerFrameId, MAIN);
});

test('the frame of a node skips a frame that has no window yet', () => {
  const placeholder = { id: () => PAYMENT, domWindow: () => undefined };
  const { agent, doc } = describeAgent({ extraFrames: [placeholder] });
  assert.equal(agent._findFrameForNode({ ownerDocument: doc }).id(), MAIN);
  assert.equal(agent._findFrameForNode({ ownerDocument: { other: true } }), undefined);
});

// ---- Page.describeNode in the parent: only a frame the client has been told of ----------------------

const anything = new Proxy(function () {}, { get: () => class {} });
const { PageHandler } = load('protocol/PageHandler.js', {
  Components: { classes: {}, interfaces: {}, utils: {}, results: {} },
  ChromeUtils: { importESModule: () => anything },
  dump: () => {},
}, ['PageHandler']);

function describingHandler(answer) {
  const handler = Object.create(PageHandler.prototype);
  handler._workersNotToldYet = new Map();
  handler._restoreBeingSaid = false;
  const sent = [];
  handler._contentPage = { send: async (method, params) => (sent.push([method, params]), { ...answer }) };
  handler._session = { emitEvent() {} };
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
  return { handler, sent };
}

test('the client is given the content frame once it has been announced to it', async () => {
  const { handler, sent } = describingHandler({ contentFrameId: PAYMENT, ownerFrameId: MAIN });
  const options = { objectId: 'o', frameId: MAIN };
  assert.equal((await handler['Page.describeNode'](options)).contentFrameId, undefined,
               'a frame the client does not know is not named: frame_for() would not find it');
  assert.equal((await handler['Page.describeNode'](options)).ownerFrameId, MAIN);
  handler._onFrameAttached({ frameId: PAYMENT, parentFrameId: MAIN });
  assert.equal((await handler['Page.describeNode'](options)).contentFrameId, PAYMENT, 'a live frame passes the gate');
  assert.deepEqual(sent.map(([method]) => method), ['describeNode', 'describeNode', 'describeNode']);
});

test('a frame that has been detached is not named any more', async () => {
  const { handler } = describingHandler({ contentFrameId: PAYMENT, ownerFrameId: MAIN });
  const options = { objectId: 'o', frameId: MAIN };
  handler._onFrameAttached({ frameId: PAYMENT, parentFrameId: MAIN });
  assert.equal((await handler['Page.describeNode'](options)).contentFrameId, PAYMENT);
  handler._onFrameDetached(PAYMENT);
  assert.equal((await handler['Page.describeNode'](options)).contentFrameId, undefined,
               'the client forgot the frame at frameDetached: frame_for() would not find it');
});

test('an element that holds no frame is described as it was', async () => {
  const { handler } = describingHandler({ contentFrameId: undefined, ownerFrameId: MAIN });
  const result = await handler['Page.describeNode']({ objectId: 'o', frameId: MAIN });
  assert.equal(result.contentFrameId, undefined);
  assert.equal(result.ownerFrameId, MAIN);
});

// ---- frameName: the name of an <iframe>, read in the process of its embedder -------------------

test('the name of an out-of-process frame is the name of its <iframe>, else its id, else empty', () => {
  const { agent } = embedderAgent();
  const name = childFrameId => JSON.parse(JSON.stringify(agent._frameName({ childFrameId })));
  assert.deepEqual(name(PAYMENT), { name: 'pay' });
  assert.deepEqual(name(WIDGET), { name: 'widget' });
  const bare = bc(31, top, { getAttribute: () => null });
  assert.deepEqual(name(helper.browsingContextToFrameId(bare)), { name: '' });
  const detached = bc(32, top, null);
  assert.deepEqual(name(helper.browsingContextToFrameId(detached)), { name: '' });
});

test('the name of a frame that is not in the tree is an error, as for any frame', () => {
  const { agent } = embedderAgent();
  assert.throws(() => agent._frameName({ childFrameId: 'subframe-999' }), /Failed to find frame with id = subframe-999/);
});
