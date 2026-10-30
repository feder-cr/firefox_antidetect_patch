// The download interceptor's completion is read with the IDL's arguments.
//
// nsIDownloadInterceptor.onDownloadComplete takes (aHandler, aErrorName)
// (uriloader/exthandler/StealthDownloadInterceptor.idl). TargetRegistry.js
// declared (handler, canceled, errorName): the error name arrived as
// `canceled`, so every failure other than a cancel was reported as a finished
// download, and success came with an undefined error.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(ROOT, rel), 'utf8');

function idlArguments() {
  const idl = read('uriloader/exthandler/StealthDownloadInterceptor.idl');
  const m = idl.match(/void\s+onDownloadComplete\s*\(([^)]*)\)/);
  assert.ok(m, 'the IDL declares onDownloadComplete');
  return m[1].split(',').map(s => s.trim()).filter(Boolean);
}

function jsParameters() {
  const js = read('juggler/TargetRegistry.js');
  const m = js.match(/^\s*onDownloadComplete\s*\(([^)]*)\)\s*\{/m);
  assert.ok(m, 'TargetRegistry.js implements onDownloadComplete');
  return m[1].split(',').map(s => s.trim()).filter(Boolean);
}

test('onDownloadComplete takes as many arguments as the IDL gives', () => {
  assert.equal(jsParameters().length, idlArguments().length);
});

test('the second argument is the error name', () => {
  assert.match(idlArguments()[1], /aErrorName$/);
  assert.match(jsParameters()[1], /error/i);
});
