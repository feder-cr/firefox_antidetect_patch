// The frame tree of a fragment root (firefox-62, Fission strategy 1, step 1b).
//
// A content process holds one frame tree per fragment root: the top, and every frame whose parent is in
// another process. Its tree has only the frames this process can read. A browsing context without a docShell
// here is either the stand-in of a frame in another process or a local iframe whose docShell is not born yet
// (nsFrameLoader.cpp makes the browsing context before the docShell), and at the attach the two cannot be told
// apart: the Frame is made but stays inert - not announced, out of frames() and frame() - and is announced
// with its first window, once.
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

const { FrameTree, Frame } = load('content/FrameTree.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }), generateQI: () => ({}) },
  dump: () => {},
}, ['FrameTree', 'Frame']);

// A browsing context as far as the tree reads it.
let nextId = 100;
function bc({ parent = null, docShell = true, browserId = 7, id = ++nextId } = {}) {
  const context = { id, parent, browserId, children: [], docShell: docShell ? { browsingContext: null, domWindow: null } : null };
  if (context.docShell)
    context.docShell.browsingContext = context;
  if (parent)
    parent.children.push(context);
  return context;
}

// The tree built by the real constructor's own loop, without the services the rest of it needs.
function build(root) {
  const log = { attached: [], detached: [], contexts: [], forcePageReady: 0 };
  const tree = Object.create(FrameTree.prototype);
  tree._rootBrowsingContext = root;
  tree._frameIdToFrame = new Map();
  tree._runtime = {};
  tree._isolatedWorlds = new Map();
  tree._docShellSettings = {};
  tree._pageReady = false;
  tree._emitted = [];
  tree.emit = (name, ...args) => {
    tree._emitted.push([name, ...args]);
    if (name === FrameTree.Events.FrameAttached) log.attached.push(args[0].id());
    if (name === FrameTree.Events.FrameDetached) log.detached.push(args[0].id());
  };
  tree.forcePageReady = () => { log.forcePageReady++; return true; };
  tree.log = log;
  for (const context of helper.collectAllBrowsingContexts(root)) {
    if (tree._belongsHere(context))
      tree._createFrame(context);
  }
  tree._mainFrame = tree.frameForBrowsingContext(root);
  return tree;
}

// The frames of a tree as a plain array: the tree lives in another realm (vm), whose arrays deepEqual refuses.
const ids = tree => Array.from(tree.frames(), f => f.id());

// What a Frame does with a window is not this file's business: the calls are recorded.
Frame.prototype._onGlobalObjectCleared = function () { this._frameTree.log.contexts.push(this.id()); };
Frame.prototype._createIsolatedContext = function (name) { this._frameTree.log.isolated = (this._frameTree.log.isolated || []).concat([[this.id(), name]]); };

test('belongsToFragment: the root, a local child, a child of a stand-in, a remote child of a local frame', () => {
  const top = bc();
  const local = bc({ parent: top });
  const standIn = bc({ parent: top, docShell: false });
  const leaf = bc({ parent: standIn });
  assert.equal(helper.belongsToFragment(top, top), true);
  assert.equal(helper.belongsToFragment(local, top), true);
  assert.equal(helper.belongsToFragment(standIn, top), true, 'the stand-in is a frame of the tree (inert), decided later by its docShell');
  assert.equal(helper.belongsToFragment(leaf, top), false, 'what is below a frame of another process is not in this tree');
  // the leaf is a fragment root of its own, in the same process as the top
  assert.equal(helper.belongsToFragment(leaf, leaf), true);
  assert.equal(helper.belongsToFragment(bc(), top), false, 'another page');
});

test('a context without a docShell is made inert: not announced, not in frames() or frame()', () => {
  const top = bc();
  const standIn = bc({ parent: top, docShell: false });
  const tree = build(top);
  assert.deepEqual(tree.log.attached, ['mainframe-7']);
  assert.deepEqual(ids(tree), ['mainframe-7']);
  assert.equal(tree.frame('subframe-' + standIn.id), null);
  assert.ok(tree._frameIdToFrame.has('subframe-' + standIn.id), 'it is kept, to be announced if a docShell is born');
});

test('setInitScripts with a stand-in in the tree touches the frames of this process only', () => {
  const top = bc();
  bc({ parent: top, docShell: false });
  const local = bc({ parent: top });
  const tree = build(top);
  // a window in each frame of this process: a frame with none gets its world with its window
  for (const frame of tree.frames())
    frame.docShell().domWindow = {};
  tree.setInitScripts([{ worldName: 'w', script: '' }]);
  assert.deepEqual(tree.log.isolated.map(([id]) => id), ['mainframe-7', 'subframe-' + local.id]);
});

test('the fragment root has no parent frame and is the main frame of its tree', () => {
  const top = bc();
  const mid = bc({ parent: top, docShell: false });          // in another process, from the leaf's point of view
  const leaf = bc({ parent: mid });
  const tree = build(leaf);
  assert.equal(tree.mainFrame().id(), 'subframe-' + leaf.id);
  assert.equal(tree.mainFrame().parentFrame(), null);
  assert.deepEqual(ids(tree), ['subframe-' + leaf.id]);
});

test('the local leaf of a -> b -> a is not in the tree of the top', () => {
  const top = bc();
  const mid = bc({ parent: top, docShell: false });          // b, in another process
  const leaf = bc({ parent: mid });                          // a again, in the process of the top
  const topTree = build(top);
  assert.equal(topTree.frame('subframe-' + leaf.id), null);
  assert.equal(topTree._frameIdToFrame.has('subframe-' + leaf.id), false, 'no Frame is even made for it');
  const leafTree = build(leaf);
  assert.ok(leafTree.frame('subframe-' + leaf.id));
});

test('_isPageMain: the top yes; the root of a fragment under another process no', () => {
  const top = bc();
  const mid = bc({ parent: top, docShell: false });
  const leaf = bc({ parent: mid });
  assert.equal(build(top)._isPageMain(build(top).mainFrame()), true);
  const leafTree = build(leaf);
  assert.equal(leafTree._isPageMain(leafTree.mainFrame()), false);
});

test('a local iframe whose docShell is not born at its attach is announced once, with its first window, before its contexts', () => {
  const top = bc();
  const tree = build(top);
  tree.log.contexts.length = 0;
  const late = bc({ parent: top, docShell: false });
  tree._onBrowsingContextAttached(late);
  assert.deepEqual(tree.log.attached, ['mainframe-7'], 'inert at the attach');
  assert.equal(tree.frame('subframe-' + late.id), null);
  // the docShell is born; its window is created
  late.docShell = { browsingContext: late, domWindow: { location: { href: 'about:blank' } } };
  tree._onDOMWindowCreated({ docShell: late.docShell });
  assert.deepEqual(tree.log.attached, ['mainframe-7', 'subframe-' + late.id]);
  assert.deepEqual(tree.log.contexts, ['subframe-' + late.id], 'the contexts are made by the announcement, once');
  assert.ok(tree.frame('subframe-' + late.id));
  assert.deepEqual(ids(tree), ['mainframe-7', 'subframe-' + late.id]);
  // a second window of the same frame is a new document: new contexts, no second announcement
  tree._onDOMWindowCreated({ docShell: late.docShell });
  assert.deepEqual(tree.log.attached, ['mainframe-7', 'subframe-' + late.id]);
  assert.deepEqual(tree.log.contexts, ['subframe-' + late.id, 'subframe-' + late.id]);
});

test('a context of another page or below a frame of another process is not made into a frame', () => {
  const top = bc();
  const standIn = bc({ parent: top, docShell: false });
  const tree = build(top);
  const before = tree._frameIdToFrame.size;
  tree._onBrowsingContextAttached(bc({ parent: standIn }));
  tree._onBrowsingContextAttached(bc());
  assert.equal(tree._frameIdToFrame.size, before);
});

test('detaching a frame that was never announced says nothing to the client', () => {
  const top = bc();
  const standIn = bc({ parent: top, docShell: false });
  const local = bc({ parent: top });
  const tree = build(top);
  tree._onBrowsingContextDetached(standIn);
  tree._onBrowsingContextDetached(local);
  assert.deepEqual(tree.log.detached, ['subframe-' + local.id]);
  assert.equal(tree._frameIdToFrame.has('subframe-' + standIn.id), false, 'it is forgotten all the same');
});

test('the commit of a fragment root that is not the page main does not make the page ready', () => {
  const top = bc();
  const mid = bc({ parent: top, docShell: false });
  const leaf = bc({ parent: mid });
  const leafTree = build(leaf);
  leafTree._commit(leafTree.mainFrame(), 'http://a.test/leaf');
  assert.equal(leafTree.log.forcePageReady, 0);
  const topTree = build(top);
  topTree._commit(topTree.mainFrame(), 'http://a.test/top');
  assert.equal(topTree.log.forcePageReady, 1);
});

test('a fragment root that is not the page main restores no "main document"', () => {
  const top = bc();
  const mid = bc({ parent: top, docShell: false });
  const leaf = bc({ parent: mid });
  const leafTree = build(leaf);
  leafTree.mainFrame().domWindow = () => ({ location: { href: 'http://a.test/leaf' } });
  leafTree.announceRestoredDocument();
  assert.deepEqual(leafTree._emitted.filter(([name]) => name !== FrameTree.Events.FrameAttached), []);
  const topTree = build(top);
  topTree.mainFrame().domWindow = () => ({ location: { href: 'http://a.test/top' } });
  topTree.announceRestoredDocument();
  assert.ok(topTree._emitted.some(([name]) => name === FrameTree.Events.DocumentRestored));
});

// PageAgent: the root of a fragment under another process is announced by the parent (FrameLifecycle), not from here.
const agentSource = fs.readFileSync(path.join(ROOT, 'content/PageAgent.js'), 'utf8');

test('PageAgent does not say pageFrameAttached/Detached for the root of a fragment under another process', () => {
  const from = agentSource.indexOf('  _isFragmentRootOfAnotherProcess(frame) {');
  const to = agentSource.indexOf('  _onBindingCalled(');
  assert.ok(from > 0 && to > from, 'the three methods are in PageAgent.js');
  const Agent = vm.runInNewContext('(class { ' + agentSource.slice(from, to) + ' })');
  const top = bc();
  const mid = bc({ parent: top, docShell: false });
  const leaf = bc({ parent: mid });
  const local = bc({ parent: leaf });
  const leafTree = build(leaf);
  const say = [];
  const agent = Object.create(Agent.prototype);
  agent._frameTree = leafTree;
  agent._browserPage = { emit: (name, payload) => say.push([name, payload.frameId]) };
  agent._onFrameAttached(leafTree.mainFrame());
  agent._onFrameDetached(leafTree.mainFrame());
  assert.deepEqual(say, [], 'the root of the fragment');
  agent._onFrameAttached(leafTree.frame('subframe-' + local.id));
  assert.deepEqual(say, [['pageFrameAttached', 'subframe-' + local.id]], 'a local child of the root is still said');
  const topTree = build(top);
  say.length = 0;
  agent._frameTree = topTree;
  agent._onFrameAttached(topTree.mainFrame());
  assert.deepEqual(say, [['pageFrameAttached', 'mainframe-7']], 'the top of the page is still said');
});
