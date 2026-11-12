// The native function of a binding takes a MESSAGE, or a FUNCTION: the page-side end of the binding.
//
// A client that answers a call of an exposed function has to run something in the page's context. Through a name on the page's
// window that is a tell (the page lists its globals), so the script of the binding keeps everything in a closure and, once, calls
// the native function with the function that answers. A function cannot travel in an event (it is not a message): the context
// keeps it as a remote object and the event carries the id, which the client uses with Runtime.callFunction. Everything else is
// the message, unchanged.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = file => fs.readFileSync(path.join(ROOT, file), 'utf8');

function context() {
  const events = [];
  const scope = {
    Debugger: function Debugger() {},
    Cu: {
      exportFunction: (fn, target, { defineAs }) => { target[defineAs] = fn; },
      waiveXrays: x => x,
    },
    dump: () => {},
  };
  vm.createContext(scope);
  vm.runInContext(read('content/Runtime.js'), scope);
  const ExecutionContext = vm.runInContext('ExecutionContext', scope);
  const ctx = Object.create(ExecutionContext.prototype);
  const evaluated = [];
  ctx._id = 'ctx-7';
  const listenersSymbol = vm.runInContext('listenersSymbol', scope);
  ctx._runtime = { events: { onBindingCalled: { [listenersSymbol]: new Set([params => events.push(JSON.parse(JSON.stringify(params)))]) } } };
  ctx._contextGlobal = {};
  ctx.evaluateScriptSafely = script => evaluated.push(script);
  ctx.rawValueToRemoteObject = value => ({ objectId: 'obj-' + (typeof value) });
  return { ctx, events, evaluated };
}

test('a message is carried as it is', () => {
  const { ctx, events } = context();
  ctx.addBinding('send', 'script');
  ctx._contextGlobal.send('{"a":1}');
  assert.deepEqual(events, [{ executionContextId: 'ctx-7', name: 'send', payload: '{"a":1}' }]);
});

test('a function is kept as an object of the context and the event carries its id', () => {
  const { ctx, events } = context();
  ctx.addBinding('send', 'script');
  ctx._contextGlobal.send(() => 1);
  assert.deepEqual(events, [{ executionContextId: 'ctx-7', name: 'send', payload: { bindingEnd: 'obj-function' } }]);
});

test('the script of the binding runs after the native function exists', () => {
  const { ctx, evaluated } = context();
  ctx.addBinding('send', 'the script');
  assert.equal(typeof ctx._contextGlobal.send, 'function');
  assert.deepEqual(evaluated, ['the script']);
});

test('a page can have its own binding: Page.addBinding is declared and answered by the page handler', () => {
  const protocol = read('protocol/Protocol.js');
  assert.match(protocol, /'addBinding': \{\s*params: \{\s*worldName: t\.Optional\(t\.String\),\s*name: t\.String,\s*script: t\.String,/);
  const handler = read('protocol/PageHandler.js');
  assert.match(handler, /async \['Page\.addBinding'\]\(\{ worldName, name, script \}\) \{\s*return await this\._pageTarget\.addBinding\(worldName, name, script\);/);
});
