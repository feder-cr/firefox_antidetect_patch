// bypassCSP and the file chooser interception are flags of a docShell, and they are written to every docShell of
// the page that is in the process, not to the root's alone.
//
// `CSP_IsBypassedFor` (dom/security/StealthBypassCSP.cpp) and the file chooser of an <input> (HTMLInputElement.cpp)
// read the docShell of the document they run for. content/main.js wrote the flag to the docShell it was given at
// `initialize`, the root's, so a document in an iframe of the same process (and, with sites isolated, in the root of
// another process) was not covered: the CSP of an iframe held with bypass_csp on, and the file chooser of an <input>
// in it was the browser's own. The iframe's docShell does not exist when its browsing context is attached
// (nsFrameLoader attaches it first), so the settings are written when its document's window is created.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';
import { Helper, initializeRoot } from './_content_main.mjs';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

const helper = new Helper();
const { FrameTree } = load('content/FrameTree.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }) },
  dump: () => {},
}, ['FrameTree']);

// A docShell that records what was written to it.
const docShellOf = browsingContext => {
  const docShell = { browsingContext, bypassCSPEnabled: false, fileInputInterceptionEnabled: false };
  browsingContext.docShell = docShell;
  return docShell;
};
const bc = (id, parent) => ({ id, parent: parent || null, browserId: 7, docShell: null });
const frameOf = browsingContext => ({
  _browsingContext: browsingContext,
  docShell: () => browsingContext.docShell,
  _onGlobalObjectCleared() { this.cleared = true; },
});

// A tree with a main frame, a local iframe, and a frame of another process (no docShell).
function treeWith() {
  const top = bc(1, null), local = bc(2, top), remote = bc(3, top);
  const topShell = docShellOf(top), localShell = docShellOf(local);
  const frames = new Map([top, local, remote].map(b => [helper.browsingContextToFrameId(b), frameOf(b)]));
  const tree = Object.create(FrameTree.prototype);
  tree._frameIdToFrame = frames;
  tree._docShellSettings = {};
  tree._announceIfLocal = () => false;
  tree.frames = () => [...frames.values()];
  return { tree, top, topShell, localShell };
}

test('Helper.applyDocShellSettings writes what it is told and nothing else', () => {
  const shell = { bypassCSPEnabled: false, fileInputInterceptionEnabled: true };
  helper.applyDocShellSettings(shell, { bypassCSP: 1 });
  assert.deepEqual([shell.bypassCSPEnabled, shell.fileInputInterceptionEnabled], [true, true]);
  helper.applyDocShellSettings(shell, { interceptFileChooser: false });
  assert.deepEqual([shell.bypassCSPEnabled, shell.fileInputInterceptionEnabled], [true, false]);
  assert.doesNotThrow(() => helper.applyDocShellSettings(null, { bypassCSP: true }));
});

test('a setting reaches the root and the local iframe at once, and a frame of another process is skipped', () => {
  const { tree, topShell, localShell } = treeWith();
  tree.setDocShellSettings({ bypassCSP: true });
  assert.equal(topShell.bypassCSPEnabled, true);
  assert.equal(localShell.bypassCSPEnabled, true);
  assert.equal(topShell.fileInputInterceptionEnabled, false);
});

test('an iframe whose window is created after the setting gets it when its window is created', () => {
  const { tree, top } = treeWith();
  tree.setDocShellSettings({ bypassCSP: true, interceptFileChooser: true });
  const late = bc(9, top);
  const frame = frameOf(late);
  tree._frameIdToFrame.set(helper.browsingContextToFrameId(late), frame);
  const shell = docShellOf(late);
  assert.equal(shell.bypassCSPEnabled, false, 'no flag before its window is there');
  tree._onDOMWindowCreated({ docShell: shell });
  assert.deepEqual([shell.bypassCSPEnabled, shell.fileInputInterceptionEnabled], [true, true]);
  assert.equal(frame.cleared, true, 'the frame still goes on to its contexts');
});

test('a docShell of another page is left alone', () => {
  const { tree } = treeWith();
  tree.setDocShellSettings({ bypassCSP: true });
  const stranger = docShellOf(bc(50, bc(49, null)));
  tree._onDOMWindowCreated({ docShell: stranger });
  assert.equal(stranger.bypassCSPEnabled, false);
});

test('content/main.js hands both settings to the tree and writes nothing to the root docShell itself', () => {
  const writes = [];
  const docShell = new Proxy({}, { set(target, key, value) { writes.push(key); return true; } });
  const top = { id: 1, parent: null, browserId: 7, originAttributes: { userContextId: 0 } };
  top.top = top;
  const { tree, handlers } = initializeRoot({
    browsingContext: top, docShell, settings: { bypassCSP: true }, page: { interceptFileChooserDialog: true },
  });
  assert.deepEqual(tree.docShellSettings, [{ bypassCSP: true }, { interceptFileChooser: true }]);
  handlers.setInterceptFileChooserDialog(false);
  handlers.applyContextSetting({ name: 'bypassCSP', value: false });
  assert.deepEqual(tree.docShellSettings.slice(2), [{ interceptFileChooser: false }, { bypassCSP: false }]);
  assert.deepEqual(writes, []);
});

test('an iframe announced with its first window has the settings too: they are written before the announcement', () => {
  const { tree, top } = treeWith();
  tree.setDocShellSettings({ bypassCSP: true });
  const late = bc(11, top);
  tree._frameIdToFrame.set(helper.browsingContextToFrameId(late), frameOf(late));
  const shell = docShellOf(late);
  let announced = 0;
  tree._announceIfLocal = () => { announced++; return true; };   // the announcement ends the call
  tree._onDOMWindowCreated({ docShell: shell });
  assert.equal(announced, 1);
  assert.equal(shell.bypassCSPEnabled, true);
});
