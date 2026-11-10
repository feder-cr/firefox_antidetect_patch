// Page.screenshot empties the canvas it draws on and closes the snapshot, on every way out.
//
// The canvas is created in the chrome document of the page's window, in the parent process,
// and holds its backing store until the garbage collector reaches it. The snapshot (an
// ImageBitmap) was closed only when drawImage did not throw. Both are released on every way out.
// (Measured on firefox-60, 30 contexts of 6 screenshots: this does not change the parent's heap
// after a forced GC; it is resource hygiene, not the cause of the growth.)
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(JUGGLER, 'protocol/PageHandler.js'), 'utf8');
// Only whole-line comments are cut: a `//` inside a string (the xhtml namespace URL) must stay.
const code = source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^[ \t]*\/\/[^\n]*$/gm, '');

// From the canvas to the end of Page.screenshot, as written in the handler: the text after the
// snapshot is taken, up to the closing brace of the method.
const from = code.indexOf('const win = browsingContext.topChromeWindow');
const to = code.lastIndexOf('}', code.indexOf("async ['Page.getContentQuads']"));
const tail = code.slice(from, to);

test('the canvas section is in Page.screenshot', () => {
  assert.ok(from > 0 && to > from, 'the section after the snapshot is found');
  assert.match(tail, /toDataURL/);
});

// Runs the real text against a fake window, canvas and snapshot.
function run({ drawImage, toDataURL, mimeType = 'image/png', quality = 80 } = {}) {
  const snapshot = { closed: 0, close() { this.closed++; } };
  const canvas = {
    width: 0,
    height: 0,
    getContext: () => ({ drawImage: drawImage || (() => {}) }),
    toDataURL: toDataURL || (() => 'data:image/png;base64,QUJD'),
  };
  const browsingContext = {
    topChromeWindow: { document: { createElementNS: () => canvas } },
  };
  const fn = new Function('browsingContext', 'snapshot', 'canvasWidth', 'canvasHeight', 'mimeType', 'quality',
      'return (async () => {' + tail + '})();');
  const result = fn(browsingContext, snapshot, 1280, 720, mimeType, quality);
  return { result, canvas, snapshot };
}

test('the answer is the base64 of the image, and the canvas is empty after it', async () => {
  const { result, canvas, snapshot } = run();
  assert.deepEqual(await result, { data: 'QUJD' });
  assert.equal(canvas.width, 0);
  assert.equal(canvas.height, 0);
  assert.ok(snapshot.closed >= 1, 'the snapshot is closed');
});

test('a canvas that is drawn at the size asked has that size while it is drawn', async () => {
  let seen;
  const { result } = run({ toDataURL() { seen = [this.width, this.height]; return 'data:image/png;base64,QUJD'; } });
  await result;
  assert.deepEqual(seen, [1280, 720]);
});

test('a drawImage that throws still closes the snapshot and empties the canvas', async () => {
  const { result, canvas, snapshot } = run({ drawImage() { throw new Error('drawImage failed'); } });
  await assert.rejects(result, /drawImage failed/);
  assert.ok(snapshot.closed >= 1, 'the snapshot is closed');
  assert.equal(canvas.width, 0);
  assert.equal(canvas.height, 0);
});

test('an encoder that throws still empties the canvas', async () => {
  const { result, canvas, snapshot } = run({ toDataURL() { throw new Error('encoder failed'); } });
  await assert.rejects(result, /encoder failed/);
  assert.ok(snapshot.closed >= 1);
  assert.equal(canvas.width, 0);
  assert.equal(canvas.height, 0);
});

test('a quality out of range is refused and the canvas is empty', async () => {
  const { result, canvas, snapshot } = run({ mimeType: 'image/jpeg', quality: 200 });
  await assert.rejects(result, /Quality must be an integer value between 0 and 100/);
  assert.ok(snapshot.closed >= 1);
  assert.equal(canvas.width, 0);
  assert.equal(canvas.height, 0);
});
