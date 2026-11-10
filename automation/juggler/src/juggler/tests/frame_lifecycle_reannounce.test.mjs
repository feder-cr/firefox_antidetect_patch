// The frames of a document restored from the back-forward cache are announced again, the ones of other processes too.
//
// The client drops the child frames of a frame when a document commits in it, and tells nobody. A document restored
// from the back-forward cache keeps its frames (their browsing contexts are cached, not discarded), so after
// `go_back()` they are all still there and the client has none. The content process reports the frames it holds
// (PageAgent._onDocumentRestored); since the content process no longer announces a frame of another process, the
// parent's FrameLifecycle, which knows it, says it again: on the commit of a navigation that is a restore
// (`nav-bfcache-N`), never on a new document, whose frames are discarded and announced by their own attach.
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

const anything = new Proxy(function () {}, { get: () => class {} });
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

const attached = sent => sent.filter(([name]) => name === 'Page.frameAttached').map(([, p]) => [p.frameId, p.parentFrameId]);

test('framesBelow lists the frames announced under a frame, parents first, and not the frame itself', () => {
  const { lifecycle, mid, leaf, id } = page();
  assert.deepEqual(Array.from(lifecycle.framesBelow('mainframe-9'), f => [f.frameId, f.parentFrameId]),
    [[id(mid), 'mainframe-9'], [id(leaf), id(mid)]]);
  assert.deepEqual(Array.from(lifecycle.framesBelow(id(mid)), f => f.frameId), [id(leaf)]);
  assert.deepEqual(Array.from(lifecycle.framesBelow(id(leaf)), f => f.frameId), []);
});

test('a frame that was detached is not said again', () => {
  const { lifecycle, mid, leaf, id } = page();
  obs.notify(leaf, 'browsing-context-discarded', 'discard');
  assert.deepEqual(Array.from(lifecycle.framesBelow('mainframe-9'), f => f.frameId), [id(mid)]);
});

test('the commit of a restored document says again the frames the client forgot, parents first', () => {
  const { handler, sent, mid, leaf, id } = page();
  assert.deepEqual(attached(sent), [['mainframe-9', undefined], [id(mid), 'mainframe-9'], [id(leaf), id(mid)]]);
  sent.length = 0;
  handler._onNavigationCommitted({ frameId: 'mainframe-9', navigationId: 'nav-bfcache-1', url: 'http://a.test/top' });
  assert.deepEqual(sent.map(([name]) => name), ['Page.navigationCommitted', 'Page.frameAttached', 'Page.frameAttached']);
  assert.deepEqual(attached(sent), [[id(mid), 'mainframe-9'], [id(leaf), id(mid)]]);
});

test('the commit of a new document does not say frames again: its old frames are discarded, not restored', () => {
  const { handler, sent } = page();
  sent.length = 0;
  handler._onNavigationCommitted({ frameId: 'mainframe-9', navigationId: 'nav-23', url: 'http://a.test/other' });
  assert.deepEqual(attached(sent), []);
});

test('a restore in a frame says only the frames below that frame', () => {
  const { handler, sent, mid, leaf, id } = page();
  sent.length = 0;
  handler._onNavigationCommitted({ frameId: id(mid), navigationId: 'nav-bfcache-2', url: 'http://b.test/mid' });
  assert.deepEqual(attached(sent), [[id(leaf), id(mid)]]);
});

test('the removed frame leaves the client, and is not said again by a later restore', () => {
  const { handler, sent, mid, leaf, id } = page();
  obs.notify(mid, 'browsing-context-discarded', 'discard');
  assert.deepEqual(sent.filter(([name]) => name === 'Page.frameDetached').map(([, p]) => p.frameId), [id(leaf), id(mid)]);
  sent.length = 0;
  handler._onNavigationCommitted({ frameId: 'mainframe-9', navigationId: 'nav-bfcache-3', url: 'http://a.test/top' });
  assert.deepEqual(attached(sent), []);
});

test('a restore says again the frames of the document that came back, not those of the other cached documents of the page', () => {
  const top1 = context();
  const child1 = context({ parent: top1 });
  const browser = { browsingContext: top1 };
  const lifecycle = new FrameLifecycle(browser);
  lifecycle.on('frameattached', () => {});
  lifecycle.start();
  // the page navigates: a new top, with a frame of its own; the old document and its frame go to the cache
  const top2 = context();
  browser.browsingContext = top2;
  obs.notify(top2, 'browsing-context-attached', 'replace');
  const child2 = context({ parent: top2 });
  obs.notify(child2, 'browsing-context-attached', 'attach');
  assert.deepEqual(Array.from(lifecycle.framesBelow('mainframe-9'), f => f.frameId), ['subframe-' + child2.id]);
  // back: the first top is the page's again
  browser.browsingContext = top1;
  assert.deepEqual(Array.from(lifecycle.framesBelow('mainframe-9'), f => f.frameId), ['subframe-' + child1.id]);
});

// test_oop_frame_has_its_url_and_answers_after_the_top_comes_back_from_the_cache: the client drops the frames below the top
// at the commit of the restore, with their addresses; a frame of another process comes back announced, so it comes back
// committed with the document it had, or `page.frames` lists it with no url and `frame.url == mid` never holds.
test('the frames that come back with a restored document are committed again with the document they had', () => {
  const { top, mid, leaf, handler, sent, id } = page();
  handler._documents.set(id(mid), { navigationId: 'nav-mid', url: 'http://b.test/mid', name: 'b' });
  handler._documents.set(id(leaf), { navigationId: 'nav-leaf', url: 'http://a.test/leaf', name: 'a' });
  sent.length = 0;
  handler._forgetFramesBelow(id(top));
  handler._reannounceAfterRestore(id(top));
  assert.deepEqual(sent.map(([name, p]) => [name, p.frameId, p.url]), [
    ['Page.frameAttached', id(mid), undefined],
    ['Page.navigationCommitted', id(mid), 'http://b.test/mid'],
    ['Page.frameAttached', id(leaf), undefined],
    ['Page.navigationCommitted', id(leaf), 'http://a.test/leaf'],
  ]);
});
