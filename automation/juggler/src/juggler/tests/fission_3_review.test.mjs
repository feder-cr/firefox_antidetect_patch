// Findings of the review of the navigations (steps 2b + 3), each a test that failed on the tree that was reviewed:
// the abort of the process an iframe leaves, the
// navigations a newer one replaced, the commits of one frame that wait for its name, and the frames held back while
// their parent navigates and then cut.
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

const Cr = { NS_BINDING_ABORTED: 0x804b0002 };
class nsIChannel {}
const STATE_STOP = 0x10;
const Ci = { nsIChannel, nsIWebProgressListener: { STATE_STOP }, nsIWebNavigation: {} };
const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } }, Ci, Cr, ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);
const queue = [];
const { FrameTree } = load('content/FrameTree.js', {
  Components: { interfaces: Ci, results: Cr, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }), generateQI: () => ({}) },
  Services: { tm: { dispatchToMainThread: fn => queue.push(fn) } },
  dump: () => {},
}, ['FrameTree']);
const { FrameNavigations } = load('FrameNavigations.js', {}, ['FrameNavigations']);

test('R1 the abort an iframe\'s old process makes when the iframe goes to another process is not the navigation\'s abort', () => {
  // Measured on the engine (Windows, strategy 1): an iframe that navigates to another site gets, from the process it
  // leaves, STATE_STOP with NS_BINDING_ABORTED while its docShell is being destroyed (isBeingDestroyed() true,
  // not in the cache). The main frame's version of this is filtered (FrameTree.onStateChange, _isPageMain); the
  // child's is reported at once, the parent ends the navigation, and the document that commits in the new process
  // finds none in flight: the client sees Navigation.aborted and never the commit (page.frames keeps the old url).
  const childShell = { destroyed: true, browsingContext: { isInBFCache: false, parent: {}, id: 5 },
                       isBeingDestroyed() { return this.destroyed; } };
  const child = { _pendingNavigationId: 'nav-2', _children: new Set(), _browsingContext: childShell.browsingContext };
  const emitted = [];
  const t = {
    _mainFrame: { _browsingContext: { parent: null } },
    _isPageMain: FrameTree.prototype._isPageMain,
    frameForDocShell: shell => shell === childShell ? child : null,
    emit: (event, frame, ...args) => emitted.push([event]),
    forcePageReady() {},
  };
  const request = Object.assign(new nsIChannel(), { isDocument: true, QueryInterface() { return this; } });
  FrameTree.prototype.onStateChange.call(t, { DOMWindow: { docShell: childShell } }, request, STATE_STOP, Cr.NS_BINDING_ABORTED);
  while (queue.length) queue.shift()();
  assert.deepEqual(emitted, [], 'nothing is reported for a frame whose docShell is being destroyed by the swap');
});

test('R3 FrameNavigations remembers the navigations a newer one replaced without bound', () => {
  const nav = new FrameNavigations(() => {});
  for (let i = 1; i <= 1000; i++)
    nav.onDocumentRequest('mainframe-1', 'nav-' + i, 'http://a.test/' + i);   // each replaces the one before; none is heard from
  const replaced = nav._replaced.get('mainframe-1');
  assert.ok(replaced.size <= 8, 'replaced navigations kept: ' + replaced.size + ' (the main frame is never forgotten)');
});

function pageHandlerWithNames() {
  const scope = { Components: { classes: {}, interfaces: {}, utils: {}, results: {} }, dump: () => {}, setTimeout, clearTimeout };
  scope.ChromeUtils = { importESModule: url => url.endsWith('Helper.js') ? { Helper, EventWatcher: class {} }
      : url.endsWith('Timer.sys.mjs') ? { setTimeout, clearTimeout }
      : url.endsWith('FrameNavigations.js') ? { FrameNavigations } : new Proxy(function () {}, { get: () => class {} }) };
  const { PageHandler } = load('protocol/PageHandler.js', scope, ['PageHandler']);
  const events = [];
  const h = Object.create(PageHandler.prototype);
  h._workersNotToldYet = new Map();
  h._restoreBeingSaid = false;
  h._session = { emitEvent: (name, payload) => events.push([name, plain(payload)]) };
  h._reportedFrameIds = new Set(['subframe-7', 'm']); h._liveFrameIds = new Set(); h._parentOfFrame = new Map();
  h._frameEventsBeforeFrame = new Map(); h._contextFrame = new Map(); h._contextsOfFrame = new Map();
  h._eventsAfterName = new Map(); h._heldBackFrames = new Map();
  h._pageNetwork = null; h._frameLifecycle = { reannounceBelow() {}, framesBelow: () => [] };
  h._frameNavigations = new FrameNavigations(h._onFrameNavigationEvent.bind(h));
  return { h, events };
}

test('R4 two commits of a frame whose name is asked of its embedder: the events after the second never go before it', async () => {
  const { h, events } = pageHandlerWithNames();
  const answers = [];
  h._pageTarget = { frameIdToBrowsingContext: () => ({ id: 7 }) };
  h._contentChannel = { frameRootOf: () => 7 };                       // a root of its own: its name is the embedder's
  h._contentPage = { send: () => new Promise(resolve => answers.push(resolve)) };
  h._onNavigationCommitted({ frameId: 'subframe-7', navigationId: 'nav-a', url: 'http://b.test/a', name: '' });
  h._onNavigationCommitted({ frameId: 'subframe-7', navigationId: 'nav-b', url: 'http://b.test/b', name: '' });
  answers[0]({ name: 'f' });                                           // the first answer comes
  await new Promise(r => setTimeout(r, 0));
  h._emitFrameKeyed('Page.eventFired', { frameId: 'subframe-7', name: 'load' }, 'subframe-7');   // after commit b, before its name
  answers[1]({ name: 'f' });
  await new Promise(r => setTimeout(r, 0));
  const order = events.map(([name, p]) => name === 'Page.navigationCommitted' ? 'commit ' + p.navigationId : name);
  assert.deepEqual(order, ['commit nav-a', 'commit nav-b', 'Page.eventFired']);
});

test('R5 a frame the parent held back while its parent navigates is told when that navigation is aborted', () => {
  const { h, events } = pageHandlerWithNames();
  h._onNavigationCommitted({ frameId: 'm', navigationId: 'nav-0', url: 'http://a.test/', name: '' });
  h._frameNavigations.onDocumentRequest('m', 'nav-1', 'http://a.test/slow');            // the top navigates
  h._onFrameSeenByTheParent({ frameId: 'subframe-8', parentFrameId: 'm' });             // an iframe of the current document attaches
  events.length = 0;
  h._frameNavigations.onNavigationAborted('m', 'nav-1', 'NS_BINDING_ABORTED');          // the navigation is cut: the document stays
  assert.ok(events.some(([name, p]) => name === 'Page.frameAttached' && p.frameId === 'subframe-8'),
      'the frame of the document that stayed is never announced: only a commit releases it');
});
