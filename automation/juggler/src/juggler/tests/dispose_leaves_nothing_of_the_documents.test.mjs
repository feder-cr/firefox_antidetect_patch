// What a tab's content-process agents keep of the documents they served, once the tab's top-level document is
// destroyed, loaded the way the content process loads them.
//
// After 95 navigations in one reused tab the content process held 141 MB of "detached" windows (documents the tab
// had left; 203 MB at the end of the run) that a retail Firefox, and this engine with Juggler off, did not
// keep: 0 after "Minimize memory usage". Four things of the disposal were incomplete:
// - FrameTree.dispose destroyed no execution context and disposed no frame, so the runtime kept every context
//   (each holding its window) and the debugger kept its debuggees;
// - Runtime.dispose only unregistered the console listeners;
// - Frame.dispose left its WebSocket listener registered with the service;
// - JugglerFrameChild.didDestroy ran the three disposals in a row, so one that threw (or Helper.removeListeners
//   stopping at the first tear-down that threw) left the tree alive with its observers.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

// A module of Juggler's, evaluated as a script: `export` dropped, the names it defines handed back.
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

const dumped = [];
const dump = line => dumped.push(line);

const { FrameTree, Frame } = load('content/FrameTree.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }) },
  dump,
}, ['FrameTree', 'Frame']);

class FakeDebugger {
  constructor() {
    this.debuggees = new Set();
  }
  addDebuggee(global) {
    this.debuggees.add(global);
    return { referent: global };
  }
  removeDebuggee(object) {
    // A Debugger.Object or a global, as the real one takes both.
    this.debuggees.delete(object && object.referent ? object.referent : object);
  }
  removeAllDebuggees() {
    this.debuggees.clear();
  }
}

const { Runtime } = load('content/Runtime.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  ChromeUtils: { importESModule: () => ({}) },
  Debugger: FakeDebugger,
  dump,
}, ['Runtime']);

// A runtime as far as its disposal needs it: the real methods on an object that skips the constructor's plumbing.
function runtimeWithContexts(contexts, order = []) {
  const runtime = Object.create(Runtime.prototype);
  runtime._debugger = new FakeDebugger();
  runtime._executionContexts = new Map();
  runtime._windowToExecutionContext = new Map();
  runtime._pendingPromises = new Map();
  runtime._eventListeners = [() => order.push('console listener removed')];
  runtime.events = { onExecutionContextDestroyed: { emit() {} } };
  for (const [id, window] of contexts) {
    const context = { _id: id, _domWindow: window, _contextGlobal: { id }, _serializationGlobal: null };
    runtime._executionContexts.set(id, context);
    runtime._windowToExecutionContext.set(window, context);
    context._debuggee = runtime._debugger.addDebuggee(context._contextGlobal);
  }
  return runtime;
}

test('Helper.removeListeners runs every tear-down and then throws the first error', () => {
  const helper = new Helper();
  const ran = [];
  const listeners = [
    () => ran.push('one'),
    () => { ran.push('two'); throw new Error('half-destroyed web progress'); },
    () => ran.push('three'),
  ];
  assert.throws(() => helper.removeListeners(listeners), /half-destroyed web progress/);
  assert.deepEqual(ran, ['one', 'two', 'three']);
  assert.equal(listeners.length, 0, 'the list is emptied, so a second call does not run them again');
});

test('Runtime.dispose destroys every context and lets the debugger go of its debuggees', () => {
  const order = [];
  const runtime = runtimeWithContexts([['id-1', { name: 'old document' }], ['id-2', { name: 'older document' }]], order);
  assert.equal(runtime._debugger.debuggees.size, 2);
  runtime.dispose();
  assert.equal(runtime._executionContexts.size, 0, 'no context left, each of them holding its window');
  assert.equal(runtime._windowToExecutionContext.size, 0);
  assert.equal(runtime._debugger.debuggees.size, 0, 'the debugger holds no debuggee');
  assert.deepEqual(order, ['console listener removed']);
});

test('Runtime.dispose still destroys the contexts when a console listener fails to be removed', () => {
  const runtime = runtimeWithContexts([['id-1', {}]]);
  runtime._eventListeners = [() => { throw new Error('gone'); }];
  assert.throws(() => runtime.dispose(), /gone/);
  assert.equal(runtime._executionContexts.size, 0);
  assert.equal(runtime._debugger.debuggees.size, 0);
});

function frameWith(id, destroyed, webSocketService, innerWindowId = 0) {
  const frame = Object.create(Frame.prototype);
  frame._id = id;
  frame._frameTree = { _webSocketEventService: webSocketService };
  frame._webSocketListener = { name: 'listener of ' + id };
  frame._webSocketListenerInnerWindowId = innerWindowId;
  frame._worldNameToContext = new Map([['', { name: 'context of ' + id }]]);
  frame._runtime = { destroyExecutionContext: context => destroyed.push(context.name) };
  frame._serializationSandbox = {};
  frame._textInputProcessor = {};
  return frame;
}

function webSocketService(registered) {
  return {
    hasListenerFor: id => registered.has(id),
    removeListener: (id, listener) => registered.delete(id),
  };
}

test('Frame.dispose removes its WebSocket listener, destroys its contexts and drops what it held', () => {
  const registered = new Set([77]);
  const destroyed = [];
  const frame = frameWith('subframe-1', destroyed, webSocketService(registered), 77);
  frame.dispose();
  assert.equal(registered.has(77), false, 'the service no longer holds the frame through its listener');
  assert.deepEqual(destroyed, ['context of subframe-1']);
  assert.equal(frame._worldNameToContext.size, 0);
  assert.equal(frame._serializationSandbox, null);
  assert.equal(frame._textInputProcessor, null);
});

test('Frame.dispose still destroys its contexts when the WebSocket service refuses', () => {
  const destroyed = [];
  const service = { hasListenerFor: () => { throw new Error('service is gone'); }, removeListener() {} };
  const frame = frameWith('subframe-1', destroyed, service, 77);
  frame.dispose();
  assert.deepEqual(destroyed, ['context of subframe-1']);
});

// A tree as far as its disposal needs it.
function treeWith(frames, order) {
  const tree = Object.create(FrameTree.prototype);
  const group = { __jugglerFrameTrees: new Set([tree]) };
  tree._browsingContextGroup = group;
  tree._wdm = { removeListener: () => order.push('wdm listener removed') };
  tree._wdmListener = {};
  tree._frameIdToFrame = new Map(frames.map(frame => [frame._id, frame]));
  tree._workers = new Map([['debugger', { dispose: () => order.push('worker disposed') }]]);
  tree._pointerLanding = new Map([['mousemove', { target: {}, document: { name: 'old document' } }]]);
  tree._runtime = { dispose: () => order.push('runtime disposed') };
  tree._eventListeners = [() => order.push('observer removed')];
  tree._inputEventListeners = [() => order.push('input listener removed')];
  return tree;
}

test('FrameTree.dispose disposes every frame, worker and map, whatever one of them throws', () => {
  const order = [];
  const destroyed = [];
  const registered = new Set([1, 2]);
  const service = webSocketService(registered);
  const good = frameWith('mainframe-3', destroyed, service, 1);
  const bad = frameWith('subframe-9', destroyed, service, 2);
  bad.dispose = () => { throw new Error('this frame cannot be disposed'); };
  const tree = treeWith([bad, good], order);
  const group = tree._browsingContextGroup;
  tree.dispose();
  assert.equal(group.__jugglerFrameTrees.size, 0, 'the tree is out of its group');
  assert.deepEqual(destroyed, ['context of mainframe-3'], 'the frame after the failing one is disposed');
  assert.equal(registered.has(1), false);
  assert.equal(tree._frameIdToFrame.size, 0);
  assert.equal(tree._workers.size, 0);
  assert.equal(tree._pointerLanding.size, 0, 'no node of the last pointer event is held');
  assert.deepEqual(order.filter(step => step !== 'wdm listener removed'),
      ['worker disposed', 'runtime disposed', 'observer removed', 'input listener removed']);
});

test('FrameTree.dispose removes the observers even when the runtime or the progress listener fails', () => {
  const order = [];
  const tree = treeWith([], order);
  tree._runtime = { dispose: () => { throw new Error('runtime failed'); } };
  tree._eventListeners = [() => { throw new Error('half-destroyed webProgress'); }, () => order.push('observer removed')];
  tree.dispose();
  assert.deepEqual(order.filter(step => step !== 'wdm listener removed' && step !== 'worker disposed'),
      ['observer removed', 'input listener removed']);
});

// JugglerFrameChild, with the actor machinery it needs faked.
function frameChild() {
  const helper = new Helper();
  const calls = [];
  const agentsOf = (name, failing) => {
    const step = what => () => {
      calls.push(`${name}.${what}`);
      if (failing === what)
        throw new Error(`${what} failed`);
    };
    return {
      channel: { bindToActor() {}, resetTransport: step('channel.resetTransport') },
      pageAgent: { dispose: step('pageAgent.dispose') },
      frameTree: { dispose: step('frameTree.dispose') },
    };
  };
  const scope = {
    Components: { interfaces: {} },
    ChromeUtils: { importESModule: url => url.endsWith('Helper.js') ? { Helper } : { initialize: () => scope.nextAgents } },
    JSWindowActorChild: class {},
    dump,
  };
  const { JugglerFrameChild } = load('content/JugglerFrameChild.sys.mjs', scope, ['JugglerFrameChild']);
  void helper;
  return { JugglerFrameChild, scope, calls, agentsOf };
}

function actorFor(JugglerFrameChild, browsingContext) {
  const actor = new JugglerFrameChild();
  Object.defineProperty(actor, 'browsingContext', { value: browsingContext });
  Object.defineProperty(actor, 'contentWindow', { value: { addEventListener() {}, removeEventListener() {} } });
  Object.defineProperty(actor, 'document', { value: { documentURI: 'https://example.com/' } });
  Object.defineProperty(actor, 'docShell', { value: {} });
  return actor;
}

for (const failing of ['channel.resetTransport', 'pageAgent.dispose', 'frameTree.dispose']) {
  test(`didDestroy disposes the frame tree and the agent even when ${failing} throws`, () => {
    const { JugglerFrameChild, scope, calls, agentsOf } = frameChild();
    const browsingContext = { browserId: 3, id: 12, parent: null, top: null };
    browsingContext.top = browsingContext;
    scope.nextAgents = agentsOf('tab', failing);
    const actor = actorFor(JugglerFrameChild, browsingContext);
    actor.actorCreated();
    actor.didDestroy();
    assert.deepEqual(calls, ['tab.channel.resetTransport', 'tab.pageAgent.dispose', 'tab.frameTree.dispose'],
        'each of the three runs, in order, whatever the one before it did');
  });
}

test('Runtime.dispose removes the second console listener too when the first refuses', () => {
  const removed = [];
  const runtime = runtimeWithContexts([['id-1', {}]]);
  runtime._eventListeners = [
    () => { throw new Error('console service is gone'); },
    () => removed.push('ConsoleAPIStorage listener'),
  ];
  assert.throws(() => runtime.dispose(), /console service is gone/);
  assert.deepEqual(removed, ['ConsoleAPIStorage listener'], 'the listener that holds this runtime is removed');
  assert.equal(runtime._executionContexts.size, 0);
});

test('a second dispose of a frame, a runtime or a tree is harmless', () => {
  const order = [];
  const destroyed = [];
  const registered = new Set([5]);
  const frame = frameWith('subframe-2', destroyed, webSocketService(registered), 5);
  frame.dispose();
  frame.dispose();
  assert.deepEqual(destroyed, ['context of subframe-2'], 'its context is destroyed once');
  const runtime = runtimeWithContexts([['id-1', {}]]);
  runtime.dispose();
  runtime.dispose();
  const tree = treeWith([frameWith('mainframe-3', destroyed, webSocketService(new Set()), 0)], order);
  tree.dispose();
  tree.dispose();
  assert.equal(tree._frameIdToFrame.size, 0);
});

test('Frame.dispose clears what it held even when a context refuses to be destroyed', () => {
  const frame = frameWith('subframe-3', [], webSocketService(new Set()), 0);
  frame._runtime = { destroyExecutionContext: () => { throw new Error('context is gone'); } };
  assert.throws(() => frame.dispose(), /context is gone/);
  assert.equal(frame._worldNameToContext.size, 0);
  assert.equal(frame._serializationSandbox, null);
  assert.equal(frame._textInputProcessor, null);
});
