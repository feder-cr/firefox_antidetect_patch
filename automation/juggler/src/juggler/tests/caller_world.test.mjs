// B322: the kinds of world a client declares, and the global each one gets.
//
// The caller's code (page.evaluate and the like) ran in the page's own realm,
// so a call to a standard API went through whatever the page had put in its
// place: a bot-challenge page wraps Document.prototype.querySelector in a
// Proxy, and one lookup of its token field from an evaluate got the clearance
// refused, 11 pairs out of 11. The client now declares a 'caller' world, a
// sandbox with the page's principal and hybrid Xrays (the C++ is in
// js/xpconnect/wrappers/XrayWrapper.h). These tests pin what FrameTree builds
// for each declaration; what the hybrid view resolves is measured in a browser.
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

const sandboxes = [];
const { FrameTree, Frame } = load('content/FrameTree.js', {
  Components: {
    interfaces: {},
    results: {},
    utils: {
      Sandbox(principal, options) {
        const sandbox = { principal, options };
        sandboxes.push(sandbox);
        return sandbox;
      },
    },
  },
  ChromeUtils: { importESModule: () => ({ Helper: class {} }) },
  dump: () => {},
}, ['FrameTree', 'Frame']);

const dw = { name: 'the page window' };

function globalFor(kind) {
  sandboxes.length = 0;
  return Frame.prototype._createGlobal.call({ domWindow: () => dw }, kind);
}

test('the caller\'s world is a sandbox with the page\'s principal and hybrid Xrays', () => {
  const global = globalFor('caller');
  assert.equal(sandboxes.length, 1);
  // the page's principal itself, not the extended [dw]: the page must read
  // what the caller writes and passes, as it did in its own realm
  assert.equal(global.principal, dw);
  assert.equal(global.options.sandboxPrototype, dw);
  assert.equal(global.options.hybridXrays, true);
  // hybridXrays replaces wantXrays: the engine refuses the two together
  assert.equal(global.options.wantXrays, false);
  assert.equal(global.options.wantComponents, false);
  assert.equal(global.options.wantExportHelpers, false);
});

test('the isolated world keeps the extended principal and plain Xrays', () => {
  const global = globalFor('isolated');
  // an array from the module's realm: compared by its elements
  assert.equal(global.principal.length, 1);
  assert.equal(global.principal[0], dw);
  assert.equal(global.options.wantXrays, true);
  assert.equal(global.options.hybridXrays, undefined);
});

test('the main world is the page\'s own window, and no sandbox', () => {
  assert.equal(globalFor('main'), dw);
  assert.equal(sandboxes.length, 0);
});

function worlds() {
  return {
    _isolatedWorlds: new Map(),
    frames: () => [],
    _ensureWorld: FrameTree.prototype._ensureWorld,
    setInitScripts: FrameTree.prototype.setInitScripts,
  };
}

test('a declared kind is the world\'s kind, and the main world has none', () => {
  const t = worlds();
  t.setInitScripts([
    { script: '', worldName: '__ctx_aux__' },
    { script: '', worldName: '__ctx_caller__', worldKind: 'caller' },
    { script: 'window.x = 1' },
  ]);
  assert.equal(t._isolatedWorlds.get('__ctx_aux__')._kind, 'isolated');
  assert.equal(t._isolatedWorlds.get('__ctx_caller__')._kind, 'caller');
  assert.equal(t._isolatedWorlds.get('')._kind, null);
});

test('a world declared again keeps its kind, and another kind is refused', () => {
  const t = worlds();
  t.setInitScripts([{ script: '', worldName: 'w', worldKind: 'caller' }]);
  // the whole list again, as every client sends it: same kind, accepted
  t.setInitScripts([{ script: '', worldName: 'w', worldKind: 'caller' }]);
  // a binding names the world without a kind: it stays what it was
  assert.equal(t._ensureWorld('w')._kind, 'caller');
  assert.throws(() => t._ensureWorld('w', 'isolated'), /already a caller world/);
});

test('a kind for the main world, or a kind nobody defined, is refused', () => {
  const t = worlds();
  assert.throws(() => t._ensureWorld('', 'caller'), /needs a worldName/);
  assert.throws(() => t._ensureWorld('w', 'page'), /one of: isolated, caller/);
});

test('a frame builds each world as its kind was declared', () => {
  const declared = new Map([['w', { _kind: 'caller' }], ['u', { _kind: 'isolated' }]]);
  const frame = {
    _frameTree: { _isolatedWorlds: declared },
    _createWorldContext: (name, kind) => [name, kind],
  };
  assert.deepEqual(Frame.prototype._createIsolatedContext.call(frame, 'w'), ['w', 'caller']);
  assert.deepEqual(Frame.prototype._createIsolatedContext.call(frame, 'u'), ['u', 'isolated']);
});
