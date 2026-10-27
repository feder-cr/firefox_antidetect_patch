// FrameTree's answer to a browsing context that went, loaded the way the
// content process loads it, with the real Helper that names frames.
//
// B292: after a few navigations in one tab, the iframes of the page being
// loaded were reported detached while the browser loaded them. With the
// back-forward cache in the parent each navigation gives the tab a new
// top-level context and a new FrameTree; a frame id names the frame, and the
// main frame's is the browser's (`mainframe-<browserId>`), so when the cache
// let an old top-level context go, every tree resolved it to its own main
// frame and detached all its children. Measured on firefox-42 with an
// instrumented copy: "discarded bc=10 ... root=14 known=true", then
// "detach subframe-10737418245", the live iframe of the current page.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

// An ES module of Juggler's, evaluated as a script: `export` dropped, the
// names it defines handed back.
function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci: {},
  ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);

const { FrameTree } = load('content/FrameTree.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  ChromeUtils: { importESModule: () => ({ Helper }) },
  dump: () => {},
}, ['FrameTree']);

// A tree as far as the answer needs it: its frames by id, and what it detaches.
function tree(frames) {
  return {
    _frameIdToFrame: new Map(frames),
    detached: [],
    frameForBrowsingContext: FrameTree.prototype.frameForBrowsingContext,
    _detachFrame(frame) { this.detached.push(frame); },
  };
}

const oldTop = { id: 10, parent: null, browserId: 9 };
const top = { id: 14, parent: null, browserId: 9 };
const iframe = { id: 10737418245, parent: top, browserId: 9 };
const main = { _browsingContext: top };
const sub = { _browsingContext: iframe };

function went(t, context) {
  FrameTree.prototype._onBrowsingContextDetached.call(t, context);
  return t.detached;
}

test('an old top-level context of the tab going detaches nothing of the current page', () => {
  const t = tree([['mainframe-9', main], ['subframe-10737418245', sub]]);
  assert.deepEqual(went(t, oldTop), []);
});

test('the tree\'s own top-level context going detaches its main frame', () => {
  const t = tree([['mainframe-9', main], ['subframe-10737418245', sub]]);
  assert.deepEqual(went(t, top), [main]);
});

test('an iframe\'s context going detaches that iframe', () => {
  const t = tree([['mainframe-9', main], ['subframe-10737418245', sub]]);
  assert.deepEqual(went(t, iframe), [sub]);
});

// B303 E3: the same class reached every process-wide observer, through the
// lookups themselves. A tree whose document was in the back-forward cache
// took the docShell of the page that replaced it for its own main frame:
// contexts created, a navigation started, all for another document. Measured
// on firefox-47 with an instrumented copy: the cached tree of page one queued
// runtimeExecutionContextCreated id-11/id-12 and pageNavigationStarted nav-27
// for page two. The identity check is now in the lookup, for every caller.
function lookups(frames) {
  return {
    _frameIdToFrame: new Map(frames),
    frameForBrowsingContext: FrameTree.prototype.frameForBrowsingContext,
    frameForDocShell: FrameTree.prototype.frameForDocShell,
  };
}

test('another top-level docShell of the tab is not this tree\'s main frame', () => {
  const t = lookups([['mainframe-9', main], ['subframe-10737418245', sub]]);
  const next = { id: 15, parent: null, browserId: 9 };
  assert.equal(t.frameForDocShell({ browsingContext: next }), null);
  assert.equal(t.frameForBrowsingContext(next), null);
});

test('the tree\'s own docShells are found', () => {
  const t = lookups([['mainframe-9', main], ['subframe-10737418245', sub]]);
  assert.equal(t.frameForDocShell({ browsingContext: top }), main);
  assert.equal(t.frameForDocShell({ browsingContext: iframe }), sub);
  assert.equal(t.frameForDocShell(null), null);
});

test('a context the tree never knew detaches nothing', () => {
  const t = tree([['mainframe-9', main]]);
  assert.deepEqual(went(t, { id: 77, parent: top, browserId: 9 }), []);
});
