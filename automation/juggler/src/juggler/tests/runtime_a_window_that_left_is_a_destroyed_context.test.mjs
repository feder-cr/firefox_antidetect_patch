// An evaluation that raised because its window had left is "Execution context was destroyed!", not an exception of the page.
//
// The injected script of the client is built in a world with `new InjectedScript(globalThis, ...)`, and the first thing it
// reads of its window is `document`. When the iframe is removed (or leaves for the process of its site) while that
// command is on its way, the engine's read finds a cross-origin proxy and raises "Permission denied to access property
// "document" on cross-origin object"; the reply carried it as an exception of the page, and the client gave it to the
// caller as the error of a click: 1 to 3 calls in 300 on the iframe-made-and-removed-50-times stress test of Fission
// strategy 1, besides the ones that waited 10 s for a world of the frame that had left. Every other command aimed at a
// context that went is answered "Failed to find execution context" or "Execution context was destroyed!", which the
// client reads as the context destroyed (and, the frame being detached, as "Frame was detached"). Now an exception
// raised in a context whose window can no longer be read is answered the second way.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const SOURCE = fs.readFileSync(
  path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'content', 'Runtime.js'), 'utf8');

function load({ remote = () => false, dead = () => false } = {}) {
  const scope = {
    Debugger: function Debugger() {},
    Cu: { isRemoteProxy: remote, isDeadWrapper: dead },
  };
  vm.createContext(scope);
  vm.runInContext(SOURCE, scope);
  return scope;
}

// A runtime holding one context whose evaluation raised (it returns null and fills exceptionDetails, as the real one does).
function runtimeWith(scope, domWindow) {
  const ExecutionContext = vm.runInContext('ExecutionContext', scope);
  const context = Object.create(ExecutionContext.prototype);
  context._id = 'id-x-1';
  context._domWindow = domWindow;
  const raised = async (...args) => { args[args.length - 1].text = 'the page raised'; return null; };
  context.evaluateScript = raised;
  context.evaluateFunction = raised;
  const runtime = Object.create(scope.Runtime.prototype);
  runtime._executionContexts = new Map([[context._id, context]]);
  return runtime;
}

const crossOrigin = () => ({
  get closed() { return false; },
  get document() { throw new Error('Permission denied to access property "document" on cross-origin object'); },
});
const alive = () => ({ closed: false, document: {} });

const COMMANDS = [
  ['evaluate', { executionContextId: 'id-x-1', expression: '1', returnByValue: false }],
  ['callFunction', { executionContextId: 'id-x-1', functionDeclaration: '() => 1', args: [], returnByValue: false }],
];

for (const [command, params] of COMMANDS) {
  test(`${command}: an exception in a context whose window is a cross-origin proxy is the context destroyed`, async () => {
    const runtime = runtimeWith(load(), crossOrigin());
    await assert.rejects(runtime[command](params), /Execution context was destroyed!/);
  });

  test(`${command}: a window the engine knows is remote, or dead, or closed, is gone`, async () => {
    await assert.rejects(runtimeWith(load({ remote: () => true }), alive())[command](params), /Execution context was destroyed!/);
    await assert.rejects(runtimeWith(load({ dead: () => true }), alive())[command](params), /Execution context was destroyed!/);
    await assert.rejects(runtimeWith(load(), { closed: true, document: {} })[command](params), /Execution context was destroyed!/);
  });

  test(`${command}: an exception of a page that is still there stays the page's`, async () => {
    const runtime = runtimeWith(load(), alive());
    assert.deepEqual(JSON.parse(JSON.stringify(await runtime[command](params))), { exceptionDetails: { text: 'the page raised' } });
  });

  test(`${command}: a worker has no window, and its exception stays the worker's`, async () => {
    const runtime = runtimeWith(load(), null);
    assert.deepEqual(JSON.parse(JSON.stringify(await runtime[command](params))), { exceptionDetails: { text: 'the page raised' } });
  });
}

test('a successful evaluation reads nothing of the window', async () => {
  const scope = load();
  const runtime = runtimeWith(scope, new Proxy({}, { get() { throw new Error('the window was read'); } }));
  const context = runtime._executionContexts.get('id-x-1');
  context.evaluateScript = async () => ({ type: 'number', value: 1 });
  const answer = await runtime.evaluate({ executionContextId: 'id-x-1', expression: '1', returnByValue: false });
  assert.deepEqual(JSON.parse(JSON.stringify(answer)), { result: { type: 'number', value: 1 } });
});
