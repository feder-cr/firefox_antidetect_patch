// Page.screenshot tries drawSnapshot again, and says why when the time is up (firefox-62, Fission, step 8.4).
//
// drawSnapshot throws while the document is in navigation (NS_ERROR_LOSS_OF_SIGNIFICANT_DATA) and the call is made
// again after 50 ms. The loop had no end: an error that is not a navigation's (a context that crashed, a page that
// closed) kept the command waiting for ever. Every error is still tried again, because that is what the loop did and
// what a navigation needs, but the real one is thrown after SNAPSHOT_RETRY_MS. And the browsing context is read at
// each attempt: a navigation to another group gives the tab a new top, and the one read before the first attempt is
// the page that left.
//
// The text of the loop is run as it is in PageHandler.js, against a fake clock and a fake browser.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(JUGGLER, 'protocol/PageHandler.js'), 'utf8');
const code = source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^[ \t]*\/\/[^\n]*$/gm, '');

const from = code.indexOf('let snapshot;');
const to = code.indexOf('const win = browsingContext.topChromeWindow');
const loop = code.slice(from, to);
const retryMs = Number(/const SNAPSHOT_RETRY_MS = (\d+);/.exec(code)[1]);

test('the loop and its limit are in Page.screenshot', () => {
  assert.ok(from > 0 && to > from);
  assert.match(loop, /drawSnapshot/);
  assert.equal(retryMs, 5000);
});

// Runs the loop with `drawSnapshot(browsingContext, attempt)` as the browser's answer. Time is a number that the
// 50 ms waits advance.
async function run(drawSnapshot, browsingContexts) {
  let now = 1000;
  const clock = { now: () => now };
  const wait = callback => { now += 50; Promise.resolve().then(callback); };
  const reads = [];
  let attempt = 0;
  const pageTarget = {
    linkedBrowser: () => {
      const context = browsingContexts[Math.min(reads.length, browsingContexts.length - 1)];
      reads.push(context.name);
      return { browsingContext: context };
    },
  };
  for (const context of browsingContexts)
    context.currentWindowGlobal = { drawSnapshot: async () => drawSnapshot(context, ++attempt) };
  const fn = new Function('Date', 'setTimeout', 'SNAPSHOT_RETRY_MS', 'rect', 'scale', 'pageTarget',
      'let browsingContext;' +
      'return (async () => {' + loop.replace(/this\._pageTarget/g, 'pageTarget') + ' return snapshot; })();');
  try {
    return { snapshot: await fn(clock, wait, retryMs, {}, 1, pageTarget), reads, attempts: attempt };
  } catch (e) {
    return { error: e, reads, attempts: attempt, elapsed: now - 1000 };
  }
}

test('an error of a navigation is tried again until the snapshot comes', async () => {
  const result = await run((context, attempt) => {
    if (attempt < 4)
      throw new Error('NS_ERROR_LOSS_OF_SIGNIFICANT_DATA');
    return 'snapshot';
  }, [{ name: 'top' }]);
  assert.equal(result.snapshot, 'snapshot');
  assert.equal(result.attempts, 4);
});

test('an error that does not go away is thrown, the real one, when the time is up', async () => {
  const result = await run((context, attempt) => { throw new Error('the real error ' + attempt); }, [{ name: 'top' }]);
  assert.ok(result.error, 'it ended');
  assert.match(result.error.message, /^the real error \d+$/);
  // 50 ms a wait: the last attempt is the first one past the limit
  assert.ok(result.elapsed > retryMs && result.elapsed <= retryMs + 100, 'elapsed ' + result.elapsed);
});

test('the browsing context is read at each attempt, and the snapshot is of the one that answers', async () => {
  const oldTop = { name: 'old top' };
  const newTop = { name: 'new top' };
  const result = await run((context) => {
    if (context.name === 'old top')
      throw new Error('the old page left');
    return 'snapshot of ' + context.name;
  }, [oldTop, newTop]);
  assert.equal(result.snapshot, 'snapshot of new top');
  assert.deepEqual(Array.from(result.reads), ['old top', 'new top']);
});
