// A context or object id names one thing for the whole session.
//
// Runtime.js is loaded once per content process and once per worker, and its
// ids were a counter: every process named its worlds id-1, id-2... in the
// same order. Measured 2026-10-24 on firefox-52: a page served with
// Cross-Origin-Opener-Policy and Cross-Origin-Embedder-Policy runs in a
// process of its own, the page after it does not, and the utility world was
// id-8 in both - a client holding the first one's id-8 and an object of it
// (id-10) was answered by the second one's id-8, "Cannot find object with
// id = id-10", on every locator for good.
//
// Each load below is one process: its own global, its own copy of the file.
//
// Run: node --test juggler/tests/

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const SOURCE = fs.readFileSync(
  path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'content', 'Runtime.js'), 'utf8');

function loadIntoProcess() {
  // A worker's global already has a Debugger, so the file skips the import
  // that needs ChromeUtils.
  const scope = { Debugger: function Debugger() {} };
  vm.createContext(scope);
  vm.runInContext(SOURCE, scope);
  return scope;
}

test('two processes never hand out the same id', () => {
  const first = loadIntoProcess();
  const second = loadIntoProcess();
  const a = Array.from({ length: 200 }, () => first.generateId());
  const b = Array.from({ length: 200 }, () => second.generateId());
  assert.equal(new Set(a).size, a.length);
  assert.equal(new Set(b).size, b.length);
  assert.deepEqual(a.filter(id => b.includes(id)), []);
});

test('the nth id of a new process is not the nth id of the old one', () => {
  // The defect exactly: same position, same name.
  const ids = Array.from({ length: 20 }, () => {
    const process = loadIntoProcess();
    for (let i = 0; i < 7; i++)
      process.generateId();
    return process.generateId();
  });
  assert.equal(new Set(ids).size, ids.length);
});

test('ids stay ordinary protocol strings', () => {
  const id = loadIntoProcess().generateId();
  assert.match(id, /^id-[0-9a-z]+-1$/);
});
