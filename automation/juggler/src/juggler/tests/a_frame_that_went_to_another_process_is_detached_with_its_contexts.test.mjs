// A frame whose window went to another process is detached, and the commit of the document that replaces its page
// goes on, loaded the way the content process loads them.
//
// The navigation of a page to the address it is already at, with an iframe of another site in it, never ended: "load
// not reached in 45s (our navigation started and has neither committed nor been aborted)", on every such page (MDN,
// css-tricks, cnn) and on a page of the same kind served locally (a.test with an iframe of b.test). The iframe is
// born in the process of its page and leaves for the process of its site, and from then on its WindowProxy here
// is a remote one: no longer a global object. The new document of the page is inserted in the same process and its
// commit detaches the children of the document it replaces; the detach destroyed the contexts of the iframe, and
// Runtime.destroyExecutionContext removed the page world's debuggee by its global, the proxy, which the Debugger
// refuses ("argument is not a global object"). The throw left the commit, the document the process tells the parent
// and so the navigation, undone. A navigation to another address did not meet it: the process swap of the page
// discards the children before the new document is inserted.
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

const { FrameTree, Frame } = load('content/FrameTree.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }) },
  dump: () => {},
}, ['FrameTree', 'Frame']);

// The Debugger as the engine has it: it takes a global or the Debugger.Object that addDebuggee gave for one, and it
// refuses what is not a global object any more (a WindowProxy of a frame that left the process).
class StrictDebugger {
  constructor() {
    this.debuggees = new Set();
  }
  addDebuggee(global) {
    this.debuggees.add(global);
    return { referent: global };
  }
  removeDebuggee(object) {
    if (object.referent) {
      this.debuggees.delete(object.referent);
      return;
    }
    if (object.isRemoteProxy)
      throw new TypeError('argument is not a global object');
    this.debuggees.delete(object);
  }
}

const { Runtime, createEvent } = load('content/Runtime.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  ChromeUtils: { importESModule: () => ({}) },
  Debugger: StrictDebugger,
  dump: () => {},
}, ['Runtime', 'createEvent']);

// What the window of the page's own world is once its frame lives in another process: still a debuggee, no longer a
// global object.
const leftTheProcess = () => ({ isRemoteProxy: true });

function runtimeWithAContext(global) {
  const runtime = Object.create(Runtime.prototype);
  runtime._debugger = new StrictDebugger();
  runtime._executionContexts = new Map();
  runtime._windowToExecutionContext = new Map();
  runtime._pendingPromises = new Map();
  const destroyed = [];
  runtime.events = { onExecutionContextDestroyed: createEvent() };
  runtime.events.onExecutionContextDestroyed(context => destroyed.push(context._id));
  const context = { _id: 'ctx-1', _domWindow: global, _contextGlobal: global, _serializationGlobal: null };
  context._debuggee = runtime._debugger.addDebuggee(global);
  runtime._executionContexts.set(context._id, context);
  runtime._windowToExecutionContext.set(global, context);
  return { runtime, context, destroyed };
}

test('destroying the context of a window that left the process removes its debuggee and tells it destroyed', () => {
  const { runtime, context, destroyed } = runtimeWithAContext(leftTheProcess());
  runtime.destroyExecutionContext(context);
  assert.equal(runtime._debugger.debuggees.size, 0, 'the debugger holds no debuggee of the frame that left');
  assert.equal(runtime._executionContexts.size, 0);
  assert.equal(runtime._windowToExecutionContext.size, 0);
  assert.deepEqual(destroyed, ['ctx-1']);
});

test('the commit of a document goes through when a frame under the one it replaces lives in another process now', () => {
  const { runtime, context } = runtimeWithAContext(leftTheProcess());
  const events = [];
  const tree = Object.create(FrameTree.prototype);
  tree._frameIdToFrame = new Map();
  tree.emit = (name, frame) => events.push(name);
  tree._isPageMain = () => false;
  const main = Object.create(Frame.prototype);
  main._id = 'mainframe-1';
  main._children = new Set();
  main._pendingNavigationId = 'nav-26';
  tree._mainFrame = main;
  const child = Object.create(Frame.prototype);
  child._id = 'subframe-2';
  child._announced = true;
  child._children = new Set();
  child._parentFrame = main;
  child._frameTree = { _webSocketEventService: { hasListenerFor: () => false } };
  child._webSocketListenerInnerWindowId = 0;
  child._runtime = runtime;
  child._worldNameToContext = new Map([['', context]]);
  main._children.add(child);
  tree._frameIdToFrame.set(main._id, main);
  tree._frameIdToFrame.set(child._id, child);

  tree._frameNavigationCommitted(main, 'http://a.test/xframe');

  assert.equal(main._lastCommittedNavigationId, 'nav-26', 'the document is the navigation\'s');
  assert.equal(main._pendingNavigationId, null);
  assert.equal(main._children.size, 0);
  assert.deepEqual(events.sort(), [FrameTree.Events.FrameDetached, FrameTree.Events.NavigationCommitted].sort());
});
