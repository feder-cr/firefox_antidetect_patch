// Page.screenshot retries drawSnapshot a bounded number of times.
//
// drawSnapshot throws while a navigation replaces the document and answers on
// the next try, so the handler retries. The loop discarded every error and had
// no end: a snapshot that fails for another reason (a recording that cannot be
// replayed) left the client waiting for its timeout with no message. After the
// last try the error goes back to the client.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(JUGGLER, 'protocol/PageHandler.js'), 'utf8');
const code = source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

// The retry loop of Page.screenshot, as written in the handler.
const loop = code.slice(code.indexOf('const DRAW_SNAPSHOT_ATTEMPTS'),
                        code.indexOf('const win = browsingContext.topChromeWindow'));

test('the loop has no unbounded form', () => {
  assert.ok(loop.length > 0, 'the bounded loop is in Page.screenshot');
  assert.doesNotMatch(code, /while \(!snapshot\)/);
});

// Runs the real loop text against a fake browsingContext, with no delay.
async function run(drawSnapshot) {
  const fn = new Function('browsingContext', 'rect', 'scale', 'setTimeout',
      'return (async () => {' + loop + 'return snapshot; })();');
  return fn({ currentWindowGlobal: { drawSnapshot } }, {}, 1, (f) => f());
}

test('a snapshot that always fails ends with the error of the failure', async () => {
  let calls = 0;
  await assert.rejects(
      run(async () => { calls++; throw new Error('Failed creating UnscaledFont'); }),
      /drawSnapshot failed after 300 attempts: Failed creating UnscaledFont/);
  assert.equal(calls, 300);
});

test('a snapshot that fails during a navigation and then answers is returned', async () => {
  let calls = 0;
  const snap = { ok: true };
  const got = await run(async () => {
    if (++calls < 5) throw new Error('NS_ERROR_LOSS_OF_SIGNIFICANT_DATA');
    return snap;
  });
  assert.equal(got, snap);
  assert.equal(calls, 5);
});
