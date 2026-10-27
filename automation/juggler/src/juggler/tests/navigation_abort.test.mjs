// FrameTree's answer to the main frame's NS_BINDING_ABORTED, loaded the way the
// content process loads it, with the real Helper that names the status.
//
// B302: a goto issued while the page being left rewrote itself with
// document.open() answered "committed" on the OLD document, or waited out its
// timeout. document.open() stops the browsing context's ongoing navigation
// (HTML spec), and the main frame's NS_BINDING_ABORTED was dropped whole,
// because a Fission process swap fires a spurious one; the navigation stayed
// pending, and the document.write() that follows inserts a root element into
// the old document, which onWindowEvent took for the pending navigation's
// commit. Measured on firefox-45 with the wrapper: 7 gotos of 40 answered OK in
// 0.03 s with the page still at the previous URL.
//
// The swap's abort is told from a real one by what the browser does to the old
// document in the same task: BrowsingContext::DeactivateDocuments disconnects
// the old channel and then marks the context in the BFCache, and a document
// that cannot enter it has its channel cancelled while its docShell is
// destroyed. A real abort (document.open, window.stop) leaves the document
// where it is.
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

const Cr = { NS_BINDING_ABORTED: 0x804b0002, NS_ERROR_CONNECTION_REFUSED: 0x804b000d };
class nsIChannel {}
const STATE_STOP = 0x10;
const Ci = { nsIChannel, nsIWebProgressListener: { STATE_STOP }, nsIWebNavigation: {} };

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci, Cr,
  ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);

// The task queue the deferred decision goes to, run by hand.
const queue = [];
const { FrameTree } = load('content/FrameTree.js', {
  Components: { interfaces: Ci, results: Cr, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }) },
  Services: { tm: { dispatchToMainThread: fn => queue.push(fn) } },
  dump: () => {},
}, ['FrameTree']);

function runQueue() {
  while (queue.length)
    queue.shift()();
}

// A tree as far as the answer needs it: one main frame and one child frame on
// their docShells, and what it emits.
function setup() {
  const mainShell = { destroyed: false, browsingContext: { isInBFCache: false },
                      isBeingDestroyed() { return this.destroyed; },
                      currentURI: { spec: 'http://x/old' }, QueryInterface() { return this; } };
  const childShell = { destroyed: false, browsingContext: { isInBFCache: false },
                       isBeingDestroyed() { return this.destroyed; } };
  const main = { _pendingNavigationId: 'nav-1', _children: new Set() };
  const child = { _pendingNavigationId: 'nav-2', _children: new Set() };
  const emitted = [];
  const t = {
    _mainFrame: main,
    frameForDocShell: shell => shell === mainShell ? main : shell === childShell ? child : null,
    emit: (event, frame, ...args) => emitted.push([event, frame === main ? 'main' : 'child', ...args]),
    forcePageReady: () => emitted.push(['pageReady']),
    _detachFrame() {},
    _installMainFrameInputListeners() {},
  };
  return { t, main, child, mainShell, childShell, emitted };
}

function stop(t, shell, status) {
  const request = Object.assign(new nsIChannel(), { isDocument: true, QueryInterface() { return this; } });
  FrameTree.prototype.onStateChange.call(t, { DOMWindow: { docShell: shell } }, request, STATE_STOP, status);
}

function docElementInserted(t, shell) {
  FrameTree.prototype.onWindowEvent.call(t, { type: 'DOMDocElementInserted', target: { documentGlobal: { docShell: shell } } });
}

const ABORTED = FrameTree.Events.NavigationAborted;
const COMMITTED = FrameTree.Events.NavigationCommitted;

test('a real abort of the main frame (document.open, window.stop) is reported', () => {
  const { t, mainShell, emitted } = setup();
  stop(t, mainShell, Cr.NS_BINDING_ABORTED);
  runQueue();
  assert.deepEqual(emitted, [[ABORTED, 'main', 'nav-1', 'NS_BINDING_ABORTED']]);
});

test('the document.write after the abort is not taken for the navigation\'s commit', () => {
  const { t, main, mainShell, emitted } = setup();
  stop(t, mainShell, Cr.NS_BINDING_ABORTED);
  docElementInserted(t, mainShell);
  runQueue();
  assert.equal(main._pendingNavigationId, null);
  assert.equal(emitted.filter(e => e[0] === COMMITTED).length, 0);
  assert.deepEqual(emitted, [[ABORTED, 'main', 'nav-1', 'NS_BINDING_ABORTED']]);
});

test('the process swap\'s abort, the old document entering the BFCache in the same task, is not reported', () => {
  const { t, mainShell, emitted } = setup();
  stop(t, mainShell, Cr.NS_BINDING_ABORTED);
  mainShell.browsingContext.isInBFCache = true;   // DeactivateDocuments, after the disconnect
  runQueue();
  assert.deepEqual(emitted, []);
});

test('the process swap\'s abort while the old docShell is destroyed is not reported', () => {
  const { t, mainShell, emitted } = setup();
  mainShell.destroyed = true;
  stop(t, mainShell, Cr.NS_BINDING_ABORTED);
  runQueue();
  assert.deepEqual(emitted, []);
});

test('a network failure of the main frame is reported at once, and readies the page', () => {
  const { t, mainShell, emitted } = setup();
  stop(t, mainShell, Cr.NS_ERROR_CONNECTION_REFUSED);
  assert.deepEqual(emitted, [[ABORTED, 'main', 'nav-1', 'NS_ERROR_CONNECTION_REFUSED'], ['pageReady']]);
});

test('a child frame\'s abort is reported at once', () => {
  const { t, childShell, emitted } = setup();
  stop(t, childShell, Cr.NS_BINDING_ABORTED);
  assert.deepEqual(emitted, [[ABORTED, 'child', 'nav-2', 'NS_BINDING_ABORTED']]);
});
