// A request's body reaches the client whether or not anything intercepts it.
//
// Since 2026-08-24 (B170) the body of a POST was not read, to save a copy of
// every upload; on 2026-10-03 it came back only while interception was on
// (invisible_core#90). So `request.post_data` answered None for every POST of a
// page with no route - a login, a search - as if the form had been empty, with
// no error: measured on firefox-47, a fetch POST with a body read as None with
// no route and as the body with a route that only continues (B303 E15). The
// owner chose upstream's behaviour on 2026-10-16: the body is always read, with
// upstream's reader, which leaves the upload intact and skips bodies over 10 MB.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const code = fs.readFileSync(path.join(JUGGLER, 'NetworkObserver.js'), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

test('the request event carries the body with no condition on interception', () => {
  const emit = code.slice(code.indexOf('pageNetwork.emit(PageNetwork.Events.Request'));
  const event = emit.slice(0, emit.indexOf('}, this._frameId)'));
  assert.match(event, /postData:\s*readRequestPostData\(this\.httpChannel\)\s*,/);
  assert.doesNotMatch(event, /_shouldIntercept\(\)/);
});

test('the reader keeps the upload intact and caps what it copies', () => {
  const reader = code.slice(code.indexOf('function readRequestPostData('));
  const body = reader.slice(0, reader.indexOf('\n}\n'));
  // Cloned, or rewound where it was, so the network still sends every byte.
  assert.match(body, /iStream\.clone\(\)/);
  assert.match(body, /NS_SEEK_SET, 0\)/);
  assert.match(body, /10 \* 1024 \* 1024/);
});
