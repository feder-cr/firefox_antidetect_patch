// A frame of another process comes back from the back-forward cache with the name of its <iframe> (firefox-62, Fission,
// strategy 1).
//
// Measured on the engine (Windows, test_thirty_context_cycles_leave_no_process, 60 of 60 cycles): after `goto` to
// another page and `go_back()`, 9 of the 11 frames of the tab had the name ''. The frames that come back from the cache are
// said again by the parent from the last document it told committed in each frame (FrameNavigations.lastDocument).
// That record kept the name the registry was given, and a frame that is the root of its own process cannot give it: it
// holds no <iframe> element, so the name is asked of the embedder when the commit is told (PageHandler), and only the
// commit that is told carried it. The record now keeps the name the client was told.
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
const plain = x => JSON.parse(JSON.stringify(x));

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } }, Ci: {}, ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);
const { FrameNavigations } = load('FrameNavigations.js', {}, ['FrameNavigations']);

function pageHandler() {
  const scope = { Components: { classes: {}, interfaces: {}, utils: {}, results: {} }, dump: () => {}, setTimeout, clearTimeout };
  scope.ChromeUtils = { importESModule: url => url.endsWith('Helper.js') ? { Helper, EventWatcher: class {} }
      : url.endsWith('Timer.sys.mjs') ? { setTimeout, clearTimeout }
      : url.endsWith('FrameNavigations.js') ? { FrameNavigations } : new Proxy(function () {}, { get: () => class {} }) };
  const { PageHandler } = load('protocol/PageHandler.js', scope, ['PageHandler']);
  const events = [];
  const h = Object.create(PageHandler.prototype);
  h._workersNotToldYet = new Map();
  h._workers = new Map();
  h._droppedFrameIds = new Set();
  h._restoreBeingSaid = false;
  h._session = { emitEvent: (name, payload) => events.push([name, plain(payload)]) };
  h._reportedFrameIds = new Set(['mainframe-1', 'subframe-2', 'subframe-3']);
  h._liveFrameIds = new Set(['mainframe-1', 'subframe-2', 'subframe-3']);
  // the page: a top, an iframe of another site below it, and an iframe of that one
  h._parentOfFrame = new Map([['subframe-2', 'mainframe-1'], ['subframe-3', 'subframe-2']]);
  h._frameEventsBeforeFrame = new Map(); h._contextFrame = new Map(); h._contextsOfFrame = new Map();
  h._eventsAfterName = new Map(); h._heldBackFrames = new Map();
  h._pageNetwork = null; h._frameLifecycle = { reannounceBelow() {}, framesBelow: () => [] };
  h._frameNavigations = new FrameNavigations(h._onFrameNavigationEvent.bind(h));
  const names = [];
  h._pageTarget = { frameIdToBrowsingContext: id => ({ id: Number(id.split('-')[1]) }) };
  h._contentChannel = { frameRootOf: browsingContext => browsingContext.id };      // each is a root of its own process
  h._contentPage = { send: (method, params) => new Promise(resolve => names.push({ frameId: params.childFrameId, resolve })) };
  return { h, events, names };
}


const settle = () => new Promise(resolve => setTimeout(resolve, 0));
const commits = events => events.filter(([name]) => name === 'Page.navigationCommitted').map(([, p]) => [p.frameId, p.url, p.name]);

// The page: a top and an <iframe> of another site below it, whose process is a root of its own.
function restoredFrame(framesBelow) {
  const { h, events, names } = pageHandler();
  h._frameLifecycle = { framesBelow: () => framesBelow, reannounceBelow() {} };
  return { h, events, names };
}

test('a frame of another process is said again after the cache with the name its <iframe> had when it was committed', async () => {
  const below = [{ frameId: 'subframe-2', parentFrameId: 'mainframe-1' }];
  const { h, events, names } = restoredFrame(below);
  // the document appears in the process of the frame, which has no element to read the name from
  h._frameNavigations.onContentNavigationStarted('subframe-2', 'nav-2');
  h._frameNavigations.onDocumentInserted('subframe-2', 'http://b.test/mid', '', 'nav-2');
  names[0].resolve({ name: 'f1' });
  await settle();
  assert.deepEqual(commits(events), [['subframe-2', 'http://b.test/mid', 'f1']], 'the client is told the name');
  // the top goes away and comes back: the client dropped the frame
  events.length = 0;
  h._forgetFramesBelow('mainframe-1');
  h._reannounceAfterRestore('mainframe-1');
  assert.deepEqual(commits(events), [['subframe-2', 'http://b.test/mid', 'f1']], 'the restore says the same name');
});

test('the restore says the name of the document the frame is in, not the one before it', async () => {
  const below = [{ frameId: 'subframe-2', parentFrameId: 'mainframe-1' }];
  const { h, events, names } = restoredFrame(below);
  for (const [id, url, name] of [['nav-2', 'http://b.test/one', 'first'], ['nav-3', 'http://a.test/two', 'second']]) {
    h._frameNavigations.onContentNavigationStarted('subframe-2', id);
    h._frameNavigations.onDocumentInserted('subframe-2', url, '', id);
    names.at(-1).resolve({ name });
    await settle();
  }
  events.length = 0;
  h._forgetFramesBelow('mainframe-1');
  h._reannounceAfterRestore('mainframe-1');
  assert.deepEqual(commits(events), [['subframe-2', 'http://a.test/two', 'second']]);
});

test('a name that arrives for a document the frame has left does not rename the one it is in', async () => {
  const { h, names } = restoredFrame([]);
  h._frameNavigations.onContentNavigationStarted('subframe-2', 'nav-2');
  h._frameNavigations.onDocumentInserted('subframe-2', 'http://b.test/one', '', 'nav-2');
  h._frameNavigations.onContentNavigationStarted('subframe-2', 'nav-3');
  h._frameNavigations.onDocumentInserted('subframe-2', 'http://a.test/two', 'own', 'nav-3');
  names[0].resolve({ name: 'late' });
  await settle();
  assert.equal(h._frameNavigations.lastDocument('subframe-2').name, 'own');
});
