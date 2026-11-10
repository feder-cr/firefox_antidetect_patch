// A content process may hold only a part of a page: an out-of-process iframe is
// the top of its process, but not the top of its browsing context tree. Three
// things must follow the "fragment root" (the browsing context with no parent,
// or whose parent has no docShell in this process), not `.top`:
//   - Helper.fragmentRootOf / isFragmentRoot, the one definition of the root;
//   - JugglerFrameChild: the agents are created once per fragment root and
//     `_agents()` finds them from a frame nested under that root;
//   - content/main.js: the geolocation setter must not call
//     setGeolocationServiceOverride on a browsing context that is not the top
//     (BrowsingContext::SetGeolocationServiceOverride asserts IsTop()).
// All three are executed here, loaded the way the content process loads them.
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

const helper = new Helper();

// A fake browsing context. `local` says whether its docShell lives in this
// process; `parent` is the fake of the parent browsing context.
function bc(name, parent, { local = true } = {}) {
  const b = { name, parent: parent || null, docShell: local ? { name } : null };
  b.top = parent ? parent.top : b;
  return b;
}

test('a browsing context with no parent is a fragment root, and its own root', () => {
  const top = bc('top', null);
  assert.equal(helper.isFragmentRoot(top), true);
  assert.equal(helper.fragmentRootOf(top), top);
});

test('a child whose parent has a docShell here is not a fragment root; the root is the topmost local ancestor', () => {
  const top = bc('top', null);
  const a = bc('a', top);
  const b = bc('b', a);
  assert.equal(helper.isFragmentRoot(a), false);
  assert.equal(helper.isFragmentRoot(b), false);
  assert.equal(helper.fragmentRootOf(a), top);
  assert.equal(helper.fragmentRootOf(b), top);
});

test('a child whose parent has no docShell here (another process) is a fragment root', () => {
  const top = bc('top', null, { local: false });
  const oop = bc('oop', top);
  const nested = bc('nested', oop);
  const deeper = bc('deeper', nested);
  assert.equal(helper.isFragmentRoot(oop), true);
  assert.equal(helper.fragmentRootOf(oop), oop);
  assert.equal(helper.isFragmentRoot(nested), false);
  // The climb stops at the topmost ancestor whose own parent is not here.
  assert.equal(helper.fragmentRootOf(nested), oop);
  assert.equal(helper.fragmentRootOf(deeper), oop);
  // The old `.top` would have named the other process's browsing context.
  assert.notEqual(deeper.top, helper.fragmentRootOf(deeper));
});

// ---- JugglerFrameChild --------------------------------------------------------

// The module under test, with the actor base class and `initialize` stubbed.
// `initialize` counts its calls per browsing context and hands back fake agents.
function loadFrameChild() {
  const initialized = [];
  const scope = {
    Components: { interfaces: {} },
    dump: () => {},
    JSWindowActorChild: class {},
    ChromeUtils: {
      importESModule: (url) => {
        if (url.endsWith('/Helper.js'))
          return { Helper };
        return {
          initialize: (browsingContext, docShell) => {
            const agents = {
              browsingContext,
              docShell,
              actor: undefined,
              bound: [],
              channel: {
                bindToActor(actor) { agents.bound.push(actor); },
                resetTransport() {},
              },
              pageAgent: { dispose() {}, onWindowEvent() {} },
              frameTree: { dispose() {}, onWindowEvent() {} },
            };
            initialized.push(agents);
            return agents;
          },
        };
      },
    },
  };
  const { JugglerFrameChild } = load('content/JugglerFrameChild.sys.mjs', scope, ['JugglerFrameChild']);
  return { JugglerFrameChild, initialized };
}

function actorFor(JugglerFrameChild, browsingContext) {
  const actor = new JugglerFrameChild();
  actor.browsingContext = browsingContext;
  actor.docShell = browsingContext.docShell;
  actor.contentWindow = { addEventListener() {}, removeEventListener() {} };
  actor.document = { documentURI: 'https://example.test/' };
  return actor;
}

test('an out-of-process iframe is a fragment root: its actor creates agents keyed by it, and a nested local frame finds them', () => {
  const { JugglerFrameChild, initialized } = loadFrameChild();
  const top = bc('top', null, { local: false });   // the parent is in another process
  const oop = bc('oop', top);
  const nested = bc('nested', oop);

  const rootActor = actorFor(JugglerFrameChild, oop);
  rootActor.actorCreated();
  assert.equal(initialized.length, 1);
  assert.equal(initialized[0].browsingContext, oop);
  assert.equal(initialized[0].actor, rootActor);
  assert.deepEqual(initialized[0].bound, [rootActor]);

  // The nested frame's actor creates nothing: it is not a fragment root.
  const nestedActor = actorFor(JugglerFrameChild, nested);
  nestedActor.actorCreated();
  assert.equal(initialized.length, 1);

  // Both find the agents of the root; `.top` would have found nothing.
  assert.equal(rootActor._agents(), initialized[0]);
  assert.equal(nestedActor._agents(), initialized[0]);
});

test('a second actor of the same fragment root reuses its agents; destroying the root actor disposes them', () => {
  const { JugglerFrameChild, initialized } = loadFrameChild();
  const top = bc('top', null, { local: false });
  const oop = bc('oop', top);

  const first = actorFor(JugglerFrameChild, oop);
  first.actorCreated();
  const second = actorFor(JugglerFrameChild, oop);
  second.actorCreated();
  assert.equal(initialized.length, 1);
  assert.equal(initialized[0].actor, second);

  // The older actor no longer owns the agents: its destruction leaves them.
  first.didDestroy();
  assert.equal(first._agents(), initialized[0]);

  let disposed = 0;
  initialized[0].pageAgent.dispose = () => { disposed++; };
  second.didDestroy();
  assert.equal(disposed, 1);
  assert.equal(second._agents(), undefined);
});

test('a nested local frame\'s actor neither creates nor disposes agents', () => {
  const { JugglerFrameChild, initialized } = loadFrameChild();
  const top = bc('top', null);
  const child = bc('child', top);

  const topActor = actorFor(JugglerFrameChild, top);
  topActor.actorCreated();
  const childActor = actorFor(JugglerFrameChild, child);
  childActor.actorCreated();
  assert.equal(initialized.length, 1);

  let disposed = 0;
  initialized[0].pageAgent.dispose = () => { disposed++; };
  childActor.didDestroy();
  assert.equal(disposed, 0);
  assert.equal(topActor._agents(), initialized[0]);
});

// ---- content/main.js geolocation ----------------------------------------------

// main.js with the platform stubbed: `initialize(browsingContext, docShell)` is
// run for real; the tree, the channel and the page agent are inert fakes.
// Returns the channel's registered handlers, to reach applyContextSetting.
function runInitialize(browsingContext, settings) {
  let handlers;
  class SimpleChannel {
    constructor() {}
    register(_ns, h) { handlers = h; }
    onReattached() {}
  }
  const cookies = new Map([
    [helper.contextCookieKey(0), { initScripts: [], bindings: [], settings }],
  ]);
  const scope = {
    Services: {
      scriptloader: { loadSubScript() {} },
      cpmm: { sharedData: { get: (key) => cookies.get(key) } },
      appinfo: { processID: 1 },
    },
    ChromeUtils: {
      importESModule: (url) => {
        if (url.endsWith('/Helper.js'))
          return { Helper };
        if (url.endsWith('/FrameTree.js'))
          return { FrameTree: class { applyCrossProcessState() {} } };
        if (url.endsWith('/PageAgent.js'))
          return { PageAgent: class {} };
        throw new Error('unexpected import ' + url);
      },
    },
    SimpleChannel,
  };
  const { initialize } = load('content/main.js', scope, ['initialize']);
  const docShell = {};
  initialize(browsingContext, docShell);
  return handlers;
}

function geoBrowsingContext(isTop) {
  const calls = [];
  const b = {
    browserId: 7,
    originAttributes: { userContextId: 0 },
    setGeolocationServiceOverride(...args) { calls.push(args); },
  };
  b.top = isTop ? b : { other: true };
  return { b, calls };
}

const GEO = { latitude: 45.1, longitude: 9.2, accuracy: 10 };

test('geolocation from the shared settings is applied on the top browsing context', () => {
  const { b, calls } = geoBrowsingContext(true);
  runInitialize(b, { geolocation: GEO });
  assert.equal(calls.length, 1);
  assert.equal(calls[0][0].coords.latitude, 45.1);
  assert.equal(calls[0][0].coords.longitude, 9.2);
});

test('geolocation from the shared settings is not applied on a fragment root that is not the top', () => {
  const { b, calls } = geoBrowsingContext(false);
  runInitialize(b, { geolocation: GEO });
  assert.equal(calls.length, 0);
});

test('applyContextSetting geolocation (set and cleared) does nothing on a browsing context that is not the top', () => {
  const { b, calls } = geoBrowsingContext(false);
  const handlers = runInitialize(b, {});
  handlers.applyContextSetting({ name: 'geolocation', value: GEO });
  handlers.applyContextSetting({ name: 'geolocation', value: undefined });
  assert.equal(calls.length, 0);
});

test('applyContextSetting geolocation still sets and clears the override on the top', () => {
  const { b, calls } = geoBrowsingContext(true);
  const handlers = runInitialize(b, {});
  handlers.applyContextSetting({ name: 'geolocation', value: GEO });
  handlers.applyContextSetting({ name: 'geolocation', value: undefined });
  assert.equal(calls.length, 2);
  assert.equal(calls[0][0].coords.accuracy, 10);
  assert.equal(calls[1].length, 0);
});
