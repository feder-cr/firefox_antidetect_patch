// The load of a frame does not go before the commit of a frame below it (firefox-62, Fission, strategy 1).
//
// Measured on the engine (Windows, 3 runs of 20): right after `page.goto(..., wait_until="load")` the iframe of another
// site was in `page.frames` with an EMPTY url, 3 times in 20. The embedder's process says `load` of the top once the
// iframe has loaded; the commit of the iframe comes from the PARENT, which first asks the embedder the name of the
// <iframe> (`frameName`, a round trip to the process that is busy saying `load`). The log of the parent, per run: the
// relay of the iframe's document at t, the name asked at t..t+1, the `load` of the top told at t+4, the name answered
// at t+5 (empty runs) or at t+1..t+3 (the others). So the `load` overtook the commit, and the client read the frames.
//
// The `DOMContentLoaded` and `load` of a frame now wait behind the commits that wait for a name below it.
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

const told = events => events.map(([name, p]) =>
  name === 'Page.navigationCommitted' ? 'commit ' + p.frameId : name === 'Page.eventFired' ? p.name + ' ' + p.frameId : name);
const settle = () => new Promise(resolve => setTimeout(resolve, 0));

test('the load of the top comes after the commit of an iframe whose name is still asked', async () => {
  const { h, events, names } = pageHandler();
  h._onNavigationCommitted({ frameId: 'subframe-2', navigationId: 'nav-2', url: 'http://b.test/mid', name: '' });
  assert.equal(names.length, 1, 'the name of the iframe is asked of its embedder');
  // the process of the top says `load` before it answers
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'DOMContentLoaded' }, 'mainframe-1');
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'load' }, 'mainframe-1');
  assert.deepEqual(told(events), [], 'nothing of the top goes before the commit of the iframe');
  names[0].resolve({ name: 'f' });
  await settle();
  assert.deepEqual(told(events), ['commit subframe-2', 'DOMContentLoaded mainframe-1', 'load mainframe-1']);
  assert.equal(events[0][1].name, 'f', 'the commit carries the name');
});

test('the load of the iframe comes after its own commit, and the load of the top after both, in the order they came', async () => {
  const { h, events, names } = pageHandler();
  h._onNavigationCommitted({ frameId: 'subframe-2', navigationId: 'nav-2', url: 'http://b.test/mid', name: '' });
  h._emitFrameKeyed('Page.eventFired', { frameId: 'subframe-2', name: 'load' }, 'subframe-2');
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'load' }, 'mainframe-1');
  names[0].resolve({ name: 'f' });
  await settle();
  assert.deepEqual(told(events), ['commit subframe-2', 'load subframe-2', 'load mainframe-1']);
});

test('the load of the top waits for a commit two frames down: it does not pass the load of the frame between', async () => {
  const { h, events, names } = pageHandler();
  h._onNavigationCommitted({ frameId: 'subframe-3', navigationId: 'nav-3', url: 'http://c.test/leaf', name: '' });
  // the middle frame says its load (held behind the grandchild's commit), then the top says its own
  h._emitFrameKeyed('Page.eventFired', { frameId: 'subframe-2', name: 'load' }, 'subframe-2');
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'load' }, 'mainframe-1');
  assert.deepEqual(told(events), []);
  names[0].resolve({ name: 'w' });
  await settle();
  assert.deepEqual(told(events), ['commit subframe-3', 'load subframe-2', 'load mainframe-1']);
});

test('what is not the end of a load, and the events of a frame with nothing waiting below it, are not held', async () => {
  const { h, events, names } = pageHandler();
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'load' }, 'mainframe-1');
  assert.deepEqual(told(events), ['load mainframe-1'], 'nothing waits: it goes at once');
  events.length = 0;
  h._onNavigationCommitted({ frameId: 'subframe-3', navigationId: 'nav-3', url: 'http://c.test/leaf', name: '' });
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'someOtherEvent' }, 'mainframe-1');
  h._emitFrameKeyed('Page.eventFired', { frameId: 'subframe-2', name: 'load' }, 'subframe-2');
  assert.deepEqual(told(events), ['someOtherEvent mainframe-1'], 'only DOMContentLoaded and load are held');
  names[0].resolve({ name: '' });
  await settle();
});
