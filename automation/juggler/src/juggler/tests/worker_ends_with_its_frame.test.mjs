// A worker of an iframe ends when its frame does, whichever process held it.
//
// With sites isolated a worker of an iframe is in the process of the iframe; the process that tells the page
// `pageWorkerDestroyed` is that one, and it may be gone (a crash) or may leave the frame without saying so. The
// parent knows when a frame ends (FrameLifecycle: Page.frameDetached), so the PageHandler ends the workers it
// announced for that frame, before the frame itself, and a late message from the process finds nothing to end.
// The worker's commands go to the root that holds it (the mux resolves the worker's id through the
// `pageWorkerCreated` that announced it): checked here on the real mux.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

// Juggler's files are ES modules; evaluated as a script, `export` dropped (ContentMux.js has no Gecko in it, and a
// bare import of a .js file in a package with no "type" needs a newer Node).
function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `
;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}
const { ContentMux } = load('ContentMux.js', {}, ['ContentMux']);

const modules = new Proxy(function () {}, {
  get: (target, name) => name === 'Helper' ? class { removeListeners() {} } : class {},
});
const { PageHandler } = load('protocol/PageHandler.js', {
  Components: { classes: {}, interfaces: {}, utils: {}, results: {} },
  ChromeUtils: { importESModule: () => modules },
  dump: () => {},
}, ['PageHandler']);

function pageHandler() {
  const events = [];
  const disposed = [];
  const handler = Object.create(PageHandler.prototype);
  handler._session = { emitEvent: (name, payload) => events.push([name, JSON.parse(JSON.stringify(payload))]) };
  handler._reportedFrameIds = new Set();
  handler._droppedFrameIds = new Set();
  handler._liveFrameIds = new Set();
  handler._parentOfFrame = new Map();
  handler._frameNavigations = { forget() {} };
  handler._heldBackFrames = new Map();
  handler._eventsAfterName = new Map();
  handler._frameEventsBeforeFrame = new Map();
  handler._contextFrame = new Map();
  handler._contextsOfFrame = new Map();
  handler._workers = new Map();
  handler._contentChannel = {
    connect: workerId => ({ send: async () => {}, dispose: () => disposed.push(workerId) }),
    register: () => () => {},
  };
  return { handler, events, disposed };
}

test('the workers of a frame end before the frame is detached, the workers of other frames stay', () => {
  const { handler, events, disposed } = pageHandler();
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  handler._onFrameAttached({ frameId: 'f2', parentFrameId: 'main' });
  handler._onWorkerCreated({ workerId: 'w1', frameId: 'f1', url: 'http://b.test/w.js' });
  handler._onWorkerCreated({ workerId: 'w2', frameId: 'f2', url: 'http://c.test/w.js' });
  events.length = 0;

  handler._onFrameDetached('f1');
  assert.deepEqual(events, [
    ['Page.workerDestroyed', { workerId: 'w1' }],
    ['Page.frameDetached', { frameId: 'f1' }],
  ]);
  assert.deepEqual(disposed, ['w1']);
  assert.deepEqual([...handler._workers.keys()], ['w2']);
});

test('the end of the worker that the process says later is not said twice', () => {
  const { handler, events } = pageHandler();
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  handler._onWorkerCreated({ workerId: 'w1', frameId: 'f1', url: 'http://b.test/w.js' });
  handler._onFrameDetached('f1');
  events.length = 0;
  handler._onWorkerDestroyed({ workerId: 'w1' });
  assert.deepEqual(events, []);
});

test('a frame that ends with no worker ends as before', () => {
  const { handler, events } = pageHandler();
  handler._onFrameAttached({ frameId: 'f1', parentFrameId: 'main' });
  events.length = 0;
  handler._onFrameDetached('f1');
  assert.deepEqual(events, [['Page.frameDetached', { frameId: 'f1' }]]);
});

test('the mux sends a worker\'s commands to the root that announced it, and drops the root with its workers', async () => {
  const sent = [];
  const ended = [];
  let n = 0;
  const mux = new ContentMux({
    pageTarget: { linkedBrowser: () => ({ browsingContext: { id: 1 } }) },
    name: 'page',
    createChannel: () => {
      const rootId = ++n;
      const handlers = {};
      return {
        bindToActor() {}, resetTransport() {}, dispose() {}, unregister() {},
        register: (namespace, h) => { handlers[namespace] = h; },
        say: (namespace, method, payload) => handlers[namespace][method](payload),
        connect: namespace => ({ send: async (method, params) => { sent.push([rootId, namespace, method]); }, dispose() {} }),
      };
    },
  });
  mux.register('page', {
    pageWorkerCreated() {},
    pageWorkerDestroyed(payload, { rootId }) { ended.push([payload.workerId, rootId]); },
  });
  mux.onRootAdded(1, { parent: null });
  mux.bindRoot(1, {}, { isMain: true });
  mux.onRootAdded(2, { parent: {} });
  mux.bindRoot(2, {}, { isMain: false });

  mux.endpointFor(2).channel.say('page', 'pageWorkerCreated', { workerId: 'w9', frameId: 'f', url: 'u' });
  await mux.connect('w9').send('evaluate', { expression: '1' });
  assert.deepEqual(sent, [[2, 'w9', 'evaluate']]);

  mux.disposeRoot(2);
  assert.deepEqual(ended, [['w9', 2]]);
});
