// A page agent says again only the execution contexts of the frames it holds.
//
// An iframe starts in the process of its embedder and may leave it for a process of its own (strategy 1). The contexts
// it made in the first process stay in that process' runtime, of a window that is not the frame's any more. At the
// restore of the page from the back-forward cache the agents of the top say every context of their runtime again, the
// stale ones too, and the mux takes the last root that announces a context for a frame as the one that holds the frame:
// the contexts of the frame in its own process were ended, and the client evaluated in the frame through the stale
// window of the embedder (measured on the engine: `Frame.evaluate` after `go_back()` of a page with an iframe of another
// site: "Permission denied to access property on cross-origin object").
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

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } }, Ci: {}, ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);

function agentOver(contexts, framesHeld) {
  const scope = { Components: { interfaces: {}, results: {}, utils: {} }, dump: () => {},
    Cc: new Proxy({}, { get: () => ({ getService: () => ({ addObserver() {}, removeObserver() {} }) }) }) };
  scope.ChromeUtils = { importESModule: url => url.endsWith('Helper.js') ? { Helper: class extends Helper { addObserver() { return () => {}; } } }
      : { NetUtil: {}, setTimeout } };
  const { PageAgent } = load('content/PageAgent.js', scope, ['PageAgent']);
  const said = [];
  const channel = { connect: ns => ({ emit: (method, params) => said.push([ns, method, plain(params)]), dispose() {} }), register: () => () => {} };
  const main = { id: () => 'mainframe-1', docShell: () => ({}), url: () => 'about:blank', name: () => '', pendingNavigationId: () => null,
    lastCommittedNavigationId: () => null, domWindow: () => null };
  const runtime = new Proxy({}, { get: (_, name) => name === 'executionContexts' ? () => contexts
    : name === 'events' ? new Proxy({}, { get: () => () => () => {} }) : function () {} });
  const tree = { runtime: () => runtime, mainFrame: () => main, frames: () => [], isPageReady: () => false, workers: () => [],
    frame: id => (framesHeld.includes(id) ? { id: () => id } : null), _isPageMain: () => true, enterCache() {}, leaveCache() {},
    on() {}, off() {} };
  const agent = new PageAgent(channel, tree);
  return { agent, said };
}
const context = (id, frameId, name = '') => ({ id: () => id, auxData: () => ({ frameId, name }) });
const created = said => said.filter(([, m]) => m === 'runtimeExecutionContextCreated').map(([, , p]) => p.executionContextId);

test('the contexts of a frame that left this process are not said again', () => {
  const { said } = agentOver([context('c1', 'mainframe-1'), context('c2', 'subframe-7'), context('c3', 'subframe-7', '__ctx_aux__')],
      ['mainframe-1']);
  assert.deepEqual(created(said), ['c1']);
});

test('the contexts of the frames this process holds are said, in order, after the clearing', () => {
  const { agent, said } = agentOver([context('c1', 'mainframe-1'), context('c2', 'subframe-3'), context('c3', 'subframe-7')],
      ['mainframe-1', 'subframe-3']);
  said.length = 0;
  agent._reportExecutionContexts();
  assert.deepEqual(said.map(([, m]) => m), ['runtimeExecutionContextsCleared', 'runtimeExecutionContextCreated', 'runtimeExecutionContextCreated']);
  assert.deepEqual(created(said), ['c1', 'c2']);
});

test('a context with no frame in its auxiliary data is said', () => {
  const { said } = agentOver([{ id: () => 'c9', auxData: () => ({}) }], []);
  assert.deepEqual(created(said), ['c9']);
});
