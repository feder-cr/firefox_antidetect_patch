// The record of where a pointer event landed is kept by every root of the page, not only by the page's main frame.
//
// Fission, strategy 1: a click on an element of an <iframe> of another site is handled by the process that owns it,
// and the engine tells the client where it landed from that process (PageAgent._landingOn reads the record of
// FrameTree.pointerLanding, which the privileged listeners on the chrome event handler write). Those listeners were
// installed only for the frame with no parent browsing context, the page's main frame; the root of a fragment in
// another process has a parent, so it never had them, its record stayed empty, and the click, which had been
// delivered, was answered "the action went out but did not reach the element: mousedown landed on no mousedown has
// reached the page". 21 e2e of test_nested_frame_actions.py and test_click_in_oop_iframe_by_selector.
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

const Ci = { nsIChannel: class {}, nsIWebProgressListener: { STATE_STOP: 0x10 }, nsIWebNavigation: {} };
const Cr = { NS_BINDING_ABORTED: 0x804b0002 };

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci, Cr,
  ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);

const { FrameTree } = load('content/FrameTree.js', {
  Components: { interfaces: Ci, results: Cr, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }) },
  Services: { tm: { dispatchToMainThread: fn => fn() } },
  dump: () => {},
}, ['FrameTree']);

// A tree as far as the answer needs it: its root frame, and a frame below it in the same process. `rootHasParent`:
// the root is an <iframe> of a document that lives in another process.
function setup({ rootHasParent }) {
  const rootShell = { browsingContext: { isInBFCache: false }, currentURI: { spec: 'http://x/' }, QueryInterface() { return this; } };
  const childShell = { browsingContext: { isInBFCache: false }, currentURI: { spec: 'http://x/child' }, QueryInterface() { return this; } };
  const root = { _pendingNavigationId: null, _children: new Set(), _browsingContext: { parent: rootHasParent ? {} : null } };
  const child = { _pendingNavigationId: null, _children: new Set(), _browsingContext: { parent: {} } };
  const installed = [];
  const t = {
    _mainFrame: root,
    _isPageMain: FrameTree.prototype._isPageMain,
    frameForDocShell: shell => shell === rootShell ? root : shell === childShell ? child : null,
    emit: () => {},
    forcePageReady: () => {},
    _detachFrame() {},
    _installMainFrameInputListeners: shell => installed.push(shell === rootShell ? 'root' : 'child'),
  };
  const inserted = shell => FrameTree.prototype.onWindowEvent.call(t, {
    type: 'DOMDocElementInserted', target: { documentGlobal: { docShell: shell } },
  });
  return { inserted, rootShell, childShell, installed };
}

test('the root of the page records where a pointer event landed', () => {
  const { inserted, rootShell, installed } = setup({ rootHasParent: false });
  inserted(rootShell);
  assert.deepEqual(installed, ['root']);
});

test('the root of a fragment in another process records it too: it is the one a click on its <iframe> lands in', () => {
  const { inserted, rootShell, installed } = setup({ rootHasParent: true });
  inserted(rootShell);
  assert.deepEqual(installed, ['root']);
});

test('a frame below the root of its process does not install them again: the root listens for the whole fragment', () => {
  const { inserted, childShell, installed } = setup({ rootHasParent: true });
  inserted(childShell);
  assert.deepEqual(installed, []);
});
