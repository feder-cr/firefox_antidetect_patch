// FrameTree's answer to a navigation Firefox refuses before any channel
// reports progress, loaded the way the content process loads it, with the
// real Helper that names the status.
//
// A goto to a restricted port (http://127.0.0.1:9/) was announced with
// Page.navigationStarted and then never ended: DocumentChannelChild::AsyncOpen
// refuses the port, DoURILoad fails, nsDocShell::InternalLoad shows the error
// page, and no channel ever reached onStateChange, so no navigationAborted was
// sent. The error page is a navigation of its own, with its own load
// identifier, and committed at the refused address; the reason was only in
// that document's documentURI (about:neterror?e=deniedPortAccess). Recorded on
// firefox-55 (invisible_tetra fixtures/juggler/unsafe_port.json).
//
// The docshell now reports the failure with the load identifier and the status
// (StealthNavigationLoad::Failed, 'juggler-navigation-failed-renderer'), and the
// refused navigation ends there, with the NS_ERROR_* name.
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

const Cr = { NS_ERROR_PORT_ACCESS_NOT_ALLOWED: 0x804b0013, NS_ERROR_UNKNOWN_PROTOCOL: 0x804b0012 };
const Ci = { nsIWebProgressListener: {}, nsIWebNavigation: {} };

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci, Cr,
  ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);

const { FrameTree } = load('content/FrameTree.js', {
  Components: { interfaces: Ci, results: Cr, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }) },
  dump: () => {},
}, ['FrameTree']);

function setup(pending) {
  const mainShell = {};
  const childShell = {};
  const main = { _pendingNavigationId: pending, _children: new Set() };
  const child = { _pendingNavigationId: 'nav-7', _children: new Set() };
  const emitted = [];
  const t = {
    _mainFrame: main,
    frameForDocShell: shell => shell === mainShell ? main : shell === childShell ? child : null,
    emit: (event, frame, ...args) => emitted.push([event, frame === main ? 'main' : 'child', ...args]),
    forcePageReady: () => emitted.push(['pageReady']),
  };
  const failed = (shell, payload) => FrameTree.prototype._onNavigationFailed.call(t, shell, payload);
  return { t, main, child, mainShell, childShell, emitted, failed };
}

test('a navigation refused before a channel ends, with its reason', () => {
  const s = setup('nav-88');
  s.failed(s.mainShell, `88 ${Cr.NS_ERROR_PORT_ACCESS_NOT_ALLOWED}`);
  assert.deepEqual(s.emitted, [
    [FrameTree.Events.NavigationAborted, 'main', 'nav-88', 'NS_ERROR_PORT_ACCESS_NOT_ALLOWED'],
    ['pageReady'],
  ]);
  assert.equal(s.main._pendingNavigationId, null);
});

test('a refused navigation of a child frame ends without the page being ready', () => {
  const s = setup('nav-88');
  s.failed(s.childShell, `7 ${Cr.NS_ERROR_UNKNOWN_PROTOCOL}`);
  assert.deepEqual(s.emitted, [
    [FrameTree.Events.NavigationAborted, 'child', 'nav-7', 'NS_ERROR_UNKNOWN_PROTOCOL'],
  ]);
});

test('a failure of a navigation that is no longer pending says nothing', () => {
  // The error page's own navigation, or a newer one, has taken the frame.
  const s = setup('nav-30064771073');
  s.failed(s.mainShell, `88 ${Cr.NS_ERROR_PORT_ACCESS_NOT_ALLOWED}`);
  assert.deepEqual(s.emitted, []);
  assert.equal(s.main._pendingNavigationId, 'nav-30064771073');
});

test('the docshell sends the topic FrameTree listens on, before the error page', () => {
  const tree = fs.readFileSync(path.join(ROOT, 'content/FrameTree.js'), 'utf8');
  const hook = fs.readFileSync(path.join(ROOT, '../docshell/base/StealthNavigation.cpp'), 'utf8');
  const docShell = fs.readFileSync(path.join(ROOT, '../docshell/base/nsDocShell.cpp'), 'utf8');
  assert.match(tree, /'juggler-navigation-failed-renderer'/);
  assert.match(hook, /"juggler-navigation-failed-renderer"/);
  const failed = docShell.indexOf('stealthLoad.Failed(rv);');
  const errorPage = docShell.indexOf('DisplayLoadError(rv, uri, nullptr, chan)');
  assert.ok(failed > 0 && errorPage > failed, 'the failure is reported before DisplayLoadError');
});
