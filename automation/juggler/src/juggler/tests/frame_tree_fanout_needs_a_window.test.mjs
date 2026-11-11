// A world or a binding declared to a tree reaches the frames that have a window, and only those.
//
// FrameTree.setInitScripts and addBinding fan out over frames(). A frame can be in that list with no window: a
// frame whose browsing context lives in another process (with sites isolated, a cross-site iframe), and an iframe
// of this process whose docShell does not exist yet when it is attached (nsFrameLoader attaches the browsing
// context before it creates the docShell). Creating a world or adding a binding there is a Cu.Sandbox over
// `undefined`, and the exception ends the whole declaration, so no other frame of the page gets it either. The
// frame with no window loses nothing: Frame._onGlobalObjectCleared creates the contexts, the bindings and the
// scripts of every world declared when its window is born.
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

// A frame as far as the fan-out needs it; `window` is what domWindow() answers.
function frame(name, window) {
  return {
    name,
    calls: [],
    domWindow: () => window,
    _createIsolatedContext(world) { this.calls.push(['world', world]); },
    _addBinding(world, binding, script) { this.calls.push(['binding', world, binding, script]); },
  };
}

function tree(frames) {
  const t = Object.create(FrameTree.prototype);
  t._isolatedWorlds = new Map();
  t.frames = () => frames;
  return t;
}

test('a named world is created in the frames with a window, not in the ones without', () => {
  const here = frame('here', {});
  const remote = frame('remote', undefined);
  const notYet = frame('not-yet', null);
  tree([here, remote, notYet]).setInitScripts([{ worldName: 'utility', script: 'x' }]);
  assert.deepEqual(here.calls, [['world', 'utility']]);
  assert.deepEqual(remote.calls, []);
  assert.deepEqual(notYet.calls, []);
});

test('a binding is added in the frames with a window, not in the ones without', () => {
  const here = frame('here', {});
  const remote = frame('remote', undefined);
  const t = tree([here, remote]);
  t.addBinding('utility', 'send', 'script');
  assert.deepEqual(here.calls, [['binding', 'utility', 'send', 'script']]);
  assert.deepEqual(remote.calls, []);
});

test('the declaration is kept for the frame that gets its window later', () => {
  const t = tree([frame('remote', undefined)]);
  t.setInitScripts([{ worldName: 'utility', script: 'x' }]);
  t.addBinding('utility', 'send', 'script');
  const world = t._isolatedWorlds.get('utility');
  assert.deepEqual([...world._scriptsToEvaluateOnNewDocument], ['x']);
  assert.deepEqual([...world._bindings.keys()], ['send']);
});
