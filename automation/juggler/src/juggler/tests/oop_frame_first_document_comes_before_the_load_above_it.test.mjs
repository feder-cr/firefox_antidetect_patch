// The load of a frame waits for the first document of every frame below it that the parent knows is loading
// (firefox-62, Fission, strategy 1).
//
// Measured on candidate #5 (Linux, the bench's test_the_frames_have_their_urls_when_goto_returns): 1 run in 50, right after
// `page.goto(..., wait_until="load")` the iframe of another site was in `page.frames` with an EMPTY url. The `load` of the
// top comes from the process of the top; the document of the iframe is relayed by the iframe's own process; the two
// channels are not ordered, and the `load` reached the parent first. 14e826b7b5 held the load behind a commit that waits
// for the name of its <iframe>, which needs the relay to have come; here it had not come yet.
//
// In the page, the `load` of a document comes after the load of each of its iframes that loads (a lazy one out of view
// does not), so each of them has its first document by then. The parent sees the request of that document (the network
// is the parent's) before any `load` above it: the frame's first navigation is in flight in the registry. So the `load` of
// a frame is told when no frame below it waits for its first document: committed, refused (its first load failed), or
// gone. No time is waited for.
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


test('the load of the top waits for the first document of an iframe whose relay has not come yet', async () => {
  const { h, events, names } = pageHandler();
  // the parent sees the request of the iframe's document; then the top's process says `load`, before the iframe's process
  // has relayed the document
  h._frameNavigations.onDocumentRequest('subframe-2', 'nav-2', 'http://b.test/mid');
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'load' }, 'mainframe-1');
  assert.deepEqual(told(events), ['Page.navigationStarted'], 'the load of the top waits: the iframe has no document yet');
  h._frameNavigations.onDocumentInserted('subframe-2', 'http://b.test/mid', '', 'nav-2');
  names[0].resolve({ name: 'f' });
  await settle();
  assert.deepEqual(told(events), ['Page.navigationStarted', 'commit subframe-2', 'load mainframe-1']);
});

test('the load of the top waits for a frame two down, and what the top says after its load stays behind it', async () => {
  const { h, events, names } = pageHandler();
  h._frameNavigations.onDocumentRequest('subframe-3', 'nav-3', 'http://c.test/leaf');
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'load' }, 'mainframe-1');
  h._emitFrameKeyed('Page.navigationStarted', { frameId: 'mainframe-1', navigationId: 'nav-9' }, 'mainframe-1');
  assert.deepEqual(told(events), ['Page.navigationStarted'], 'only the start of the leaf');
  h._frameNavigations.onDocumentInserted('subframe-3', 'http://c.test/leaf', '', 'nav-3');
  names[0].resolve({ name: '' });
  await settle();
  assert.deepEqual(told(events), ['Page.navigationStarted', 'commit subframe-3', 'load mainframe-1', 'Page.navigationStarted']);
});

test('an iframe that goes, or whose first load is refused, lets the load of the top go', async () => {
  for (const end of ['detach', 'refuse']) {
    const { h, events } = pageHandler();
    h._frameNavigations.onDocumentRequest('subframe-2', 'nav-2', 'http://b.test/mid');
    h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'load' }, 'mainframe-1');
    assert.ok(!told(events).includes('load mainframe-1'), end);
    if (end === 'detach')
      h._onFrameDetached('subframe-2');
    else
      h._frameNavigations.onNavigationAborted('subframe-2', 'nav-2', 'NS_BINDING_ABORTED');
    assert.ok(told(events).includes('load mainframe-1'), end + ': ' + told(events).join(', '));
  }
});

test('an iframe that already has a document, or that does not load, does not hold the load of the top', async () => {
  const { h, events, names } = pageHandler();
  // a lazy iframe out of view: attached, no navigation
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'load' }, 'mainframe-1');
  assert.deepEqual(told(events), ['load mainframe-1']);
  events.length = 0;
  // an iframe that had its first document and is navigated again by the page: the top's next load does not wait for it
  h._frameNavigations.onDocumentRequest('subframe-2', 'nav-2', 'http://b.test/mid');
  h._frameNavigations.onDocumentInserted('subframe-2', 'http://b.test/mid', '', 'nav-2');
  names[0].resolve({ name: 'f' });
  await settle();
  h._frameNavigations.onDocumentRequest('subframe-2', 'nav-5', 'http://b.test/next');
  events.length = 0;
  h._emitFrameKeyed('Page.eventFired', { frameId: 'mainframe-1', name: 'load' }, 'mainframe-1');
  assert.deepEqual(told(events), ['load mainframe-1']);
});
