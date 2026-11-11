// A worker is told to the client after its frame, and, when a document restored from the back-forward cache is being
// said, after the contexts of the document are cleared.
//
// A worker of a frame in another process is announced by that process, which has no say in the order of what the
// page's own process says: the restored top commits, its frames are announced and committed again, its contexts
// are cleared. The client drops the frames below a frame when a document commits in it, and the contexts on a
// `Runtime.executionContextsCleared`. A worker told before its frame was announced again, or between the frame and the
// clearing, reached a client that did not hold the frame (measured on the engine: `workerCreated` before the commit
// of the restored top, and between the commit of the frame and the clearing). Each test below fails without the wait.
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

// The real EventEmitter hands the listener the event NAME first, then the payload: this one does too, so a listener
// that forgets to drop the name is a listener that fails here as it fails in the engine.
class EventEmitter {
  constructor() { this.handlers = new Map(); }
  on(name, handler) { this.handlers.set(name, [...(this.handlers.get(name) || []), handler]); }
  off() {}
  once() {}
  emit(name, ...args) { for (const handler of this.handlers.get(name) || []) handler(name, ...args); }
}

function observers() {
  const byTopic = new Map();
  return {
    addObserver: (handler, topic) => byTopic.set(topic, [...(byTopic.get(topic) || []), handler]),
    removeObserver: () => {},
    notify: (browsingContext, topic, why) => { for (const handler of byTopic.get(topic) || []) handler(browsingContext, topic, why); },
  };
}

const modules = { 'resource://gre/modules/EventEmitter.sys.mjs': { EventEmitter } };
const obs = observers();
const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci: {},
  Services: { obs },
  ChromeUtils: { importESModule: url => modules[url] || {} },
}, ['Helper']);
modules['resource://gre/modules/juggler/Helper.js'] = { Helper };
const { FrameLifecycle } = load('FrameLifecycle.js', { ChromeUtils: { importESModule: url => modules[url] } }, ['FrameLifecycle']);

const anything = new Proxy(function () {}, { get: (target, name) => name === 'Helper' ? class { removeListeners() {} } : class {} });
const { PageHandler } = load('protocol/PageHandler.js', {
  Components: { classes: {}, interfaces: {}, utils: {}, results: {} },
  ChromeUtils: { importESModule: () => anything },
  dump: () => {},
}, ['PageHandler']);

let nextId = 100;
function context({ parent = null, browserId = 9 } = {}) {
  const bc = { id: nextId++, parent, browserId, isContent: true, webProgress: {}, currentRemoteType: 'web', children: [] };
  bc.top = parent ? parent.top : bc;
  if (parent) parent.children.push(bc);
  return bc;
}

// A lifecycle started on a page of a top, a frame in another process and the frame below it, and a handler that hears it,
// the way PageHandler's constructor wires them.
function page() {
  const top = context();
  const mid = context({ parent: top });
  const leaf = context({ parent: mid });
  const lifecycle = new FrameLifecycle({ browsingContext: top });
  const sent = [];
  const handler = Object.create(PageHandler.prototype);
  handler._workersNotToldYet = new Map();
  handler._restoreBeingSaid = false;
  handler._session = { emitEvent: (name, payload) => sent.push([name, JSON.parse(JSON.stringify(payload))]) };
  handler._reportedFrameIds = new Set();
  handler._droppedFrameIds = new Set();
  handler._liveFrameIds = new Set();
  handler._workers = new Map();
  handler._parentOfFrame = new Map();
  handler._frameEventsBeforeFrame = new Map();
  handler._eventsAfterName = new Map();
  handler._contextFrame = new Map();
  handler._contextsOfFrame = new Map();
  handler._contentChannel = { connect: workerId => ({ send: async () => {}, dispose: () => {} }), register: () => () => {} };
  handler._frameLifecycle = lifecycle;
  handler._heldBackFrames = new Map();
  handler._documents = new Map();
  handler._frameNavigations = { forget() {}, pendingNavigationId() {}, lastDocument: id => handler._documents.get(id) };
  lifecycle.on('frameattached', (_, payload) => handler._onFrameAttached(payload));
  lifecycle.on('framedetached', (_, { frameId }) => handler._onFrameDetached(frameId));
  lifecycle.start();
  const id = bc => (bc.parent ? 'subframe-' + bc.id : 'mainframe-' + bc.browserId);
  return { top, mid, leaf, lifecycle, handler, sent, id };
}


const names = sent => sent.map(([name, p]) => name + (p.workerId ? ' ' + p.workerId : ''));
const restore = (handler, frameId = 'mainframe-9') => handler._onNavigationCommitted({ frameId, navigationId: 'nav-bfcache-1', url: 'http://a.test/top' });
const toAnotherPage = handler => handler._onNavigationCommitted({ frameId: 'mainframe-9', navigationId: 'nav-23', url: 'http://a.test/other' });

test('a worker of a frame the client dropped waits for the frame, and for the contexts of the restore to be cleared', () => {
  const { handler, sent, mid, id } = page();
  toAnotherPage(handler);                      // the client drops the frames below the top
  sent.length = 0;
  handler._onWorkerCreated({ workerId: 'w1', frameId: id(mid), url: 'http://b.test/w.js' });
  assert.deepEqual(names(sent), [], 'the client does not hold the frame: nothing is told');

  restore(handler);                            // the top commits, the frames are said again
  assert.deepEqual(names(sent), ['Page.navigationCommitted', 'Page.frameAttached', 'Page.frameAttached']);
  handler._onExecutionContextsCleared({});
  assert.deepEqual(names(sent).slice(3), ['Runtime.executionContextsCleared', 'Page.workerCreated w1']);
});

test('a worker that comes between the commit of the restore and the clearing of the contexts waits for the clearing', () => {
  const { handler, sent, mid, id } = page();
  toAnotherPage(handler);
  restore(handler);
  sent.length = 0;
  handler._onWorkerCreated({ workerId: 'w1', frameId: id(mid), url: 'http://b.test/w.js' });
  assert.deepEqual(names(sent), []);
  handler._onExecutionContextsCleared({});
  assert.deepEqual(names(sent), ['Runtime.executionContextsCleared', 'Page.workerCreated w1']);
});

test('a worker that comes after the restore was said is told at once', () => {
  const { handler, sent, mid, id } = page();
  toAnotherPage(handler);
  restore(handler);
  handler._onExecutionContextsCleared({});
  sent.length = 0;
  handler._onWorkerCreated({ workerId: 'w1', frameId: id(mid), url: 'http://b.test/w.js' });
  assert.deepEqual(names(sent), ['Page.workerCreated w1']);
});

test('a worker of a frame the client holds, with no restore going on, is told at once', () => {
  const { handler, sent, mid, id } = page();
  sent.length = 0;
  handler._onWorkerCreated({ workerId: 'w1', frameId: id(mid), url: 'http://b.test/w.js' });
  handler._onWorkerCreated({ workerId: 'w0', frameId: 'mainframe-9', url: 'http://a.test/w.js' });
  assert.deepEqual(names(sent), ['Page.workerCreated w1', 'Page.workerCreated w0']);
});

test('a worker that ends while it waits is never told, and its end is not told either', () => {
  const { handler, sent, mid, id } = page();
  toAnotherPage(handler);
  sent.length = 0;
  handler._onWorkerCreated({ workerId: 'w1', frameId: id(mid), url: 'http://b.test/w.js' });
  handler._onWorkerDestroyed({ workerId: 'w1' });
  restore(handler);
  handler._onExecutionContextsCleared({});
  assert.deepEqual(names(sent).filter(n => /^Page\.worker/.test(n)), []);
  assert.equal(handler._workers.size, 0);
});

test('a frame that is announced later than its worker (not a restore) tells the worker after the frame', () => {
  const { handler, sent, mid, id } = page();
  toAnotherPage(handler);
  sent.length = 0;
  handler._onWorkerCreated({ workerId: 'w1', frameId: id(mid), url: 'http://b.test/w.js' });
  handler._onFrameAttached({ frameId: id(mid), parentFrameId: 'mainframe-9' });
  assert.deepEqual(names(sent), ['Page.frameAttached', 'Page.workerCreated w1']);
});
