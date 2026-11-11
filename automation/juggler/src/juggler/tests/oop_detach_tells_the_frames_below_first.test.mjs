// A frame that ends takes the frames below it with it, each one told once, the deepest first, and nothing more is
// said of them (firefox-62, Fission, strategy 1).
//
// Measured on the engine (Windows, 6 to 8 copies of the stress at once, 1 run in about 30): the client threw
// `list.remove(x): x not in list` on a frame it was told to drop. The wire of the page, in the order the handler said it:
//   Page.frameAttached P (an iframe of another site)       Page.frameAttached X (the iframe inside P)
//   Page.frameDetached P                                   <- the client drops P AND X: a frame takes its subtree along
//   Page.navigationStarted X                               <- the handler still held X live: a late event of a frame the client lost
//   Page.frameDetached X                                   <- and its detach, for a frame the client had already dropped
// The late event made the client side build X again, without ever announcing it, and the detach of X then named a frame
// the client did not hold. The handler said a detach for a frame that was gone with its parent (it kept X live).
// Now the detach of a frame is first the detach of each frame below it that the client knows, deepest first, and each
// frame is detached once.
//
// Run: node --test juggler/tests/

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

// A page: the top, an iframe P of another site, an iframe X inside P, an iframe Y inside X; all announced.
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
  h._reportedFrameIds = new Set();
  h._liveFrameIds = new Set();
  h._parentOfFrame = new Map();
  h._frameEventsBeforeFrame = new Map(); h._contextFrame = new Map(); h._contextsOfFrame = new Map();
  h._eventsAfterName = new Map(); h._heldBackFrames = new Map();
  h._pageNetwork = null; h._frameLifecycle = { reannounceBelow() {}, framesBelow: () => [] };
  h._frameNavigations = new FrameNavigations(h._onFrameNavigationEvent.bind(h));
  h._pageTarget = { frameIdToBrowsingContext: id => ({ id: Number(id.split('-')[1]) }) };
  h._contentChannel = { frameRootOf: browsingContext => browsingContext.id };
  h._contentPage = { send: () => new Promise(() => {}) };
  h._onFrameAttached({ frameId: 'mainframe-1', parentFrameId: undefined });
  h._onFrameAttached({ frameId: 'subframe-2', parentFrameId: 'mainframe-1' });
  h._onFrameAttached({ frameId: 'subframe-3', parentFrameId: 'subframe-2' });
  h._onFrameAttached({ frameId: 'subframe-4', parentFrameId: 'subframe-3' });
  events.length = 0;
  return { h, events };
}

const told = events => events.map(([name, p]) =>
  name === 'Page.frameDetached' ? 'detached ' + p.frameId : name === 'Page.frameAttached' ? 'attached ' + p.frameId
    : name === 'Page.navigationStarted' ? 'started ' + p.frameId : name);

test('the detach of a frame is the detach of every frame below it, the deepest first, then its own', () => {
  const { h, events } = pageHandler();
  h._onFrameDetached('subframe-2');
  assert.deepEqual(told(events), ['detached subframe-4', 'detached subframe-3', 'detached subframe-2']);
});

test('the detach the engine says later of a frame that ended with the frame above it is not said again', () => {
  const { h, events } = pageHandler();
  h._onFrameDetached('subframe-2');
  events.length = 0;
  h._onFrameDetached('subframe-3');
  h._onFrameDetached('subframe-4');
  assert.deepEqual(told(events), [], 'a frame the client lost is not detached again');
});

test('a detach is said once however many times the engine says it', () => {
  const { h, events } = pageHandler();
  h._onFrameDetached('subframe-4');
  h._onFrameDetached('subframe-4');
  h._onFrameDetached('subframe-3');
  h._onFrameDetached('subframe-3');
  assert.deepEqual(told(events), ['detached subframe-4', 'detached subframe-3']);
});

test('a frame announced again is a new one: its end is said', () => {
  const { h, events } = pageHandler();
  h._onFrameDetached('subframe-3');
  events.length = 0;
  h._onFrameAttached({ frameId: 'subframe-3', parentFrameId: 'subframe-2' });
  h._onFrameDetached('subframe-3');
  assert.deepEqual(told(events), ['attached subframe-3', 'detached subframe-3']);
});

test('the frames held back below a frame that ends are dropped with it, never announced', () => {
  const { h, events } = pageHandler();
  h._heldBackFrames.set('subframe-9', { frameId: 'subframe-9', parentFrameId: 'subframe-3' });
  h._onFrameDetached('subframe-2');
  assert.equal(h._heldBackFrames.has('subframe-9'), false);
  assert.deepEqual(told(events), ['detached subframe-4', 'detached subframe-3', 'detached subframe-2']);
});

test('the crash of a frame (the frames below it, deepest first, then it) still says each once', () => {
  const { h, events } = pageHandler();
  for (const id of ['subframe-4', 'subframe-3', 'subframe-2'])
    h._onFrameDetached(id);
  assert.deepEqual(told(events), ['detached subframe-4', 'detached subframe-3', 'detached subframe-2']);
});
