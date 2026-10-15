// The input chain waits only on what is guaranteed to come.
//
// Every input that brings a tab forward runs through activateAndRun's chain
// (TargetRegistry.js), so one wait that never ends stops the input of every tab
// of the browser. Two did not end in every case:
//
// - `await helper.awaitTopic('apz-repaints-flushed')` after
//   `windowUtils.flushApzRepaints()`, on the mouse and the wheel. The call
//   answers true once it has SENT the request, and the compositor drops it in
//   silence when its bridge is not initialized or the layer tree is gone
//   (WebRenderBridgeParent::RecvFlushApzRepaints, test-only on desktop). And it
//   protected nothing here: it followed a scroll of the <browser> into the
//   window (scrollRectIntoViewIfNeeded) that no binding of Element declares in
//   this tree, so the scroll never ran (B184).
// - `watcher.ensureEventsAndDispose(['dragover'])` after `dispatchDragEvent`:
//   jugglerSendDragEvent dispatches synchronously in the content process, so
//   its reply already says the dragover was dispatched; the reported event
//   never came when the dragover reached no frame.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = rel => fs.readFileSync(path.join(JUGGLER, rel), 'utf8');
// The code, not the comments that explain why it is gone.
const code = rel => read(rel).replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

test('no input path waits for an APZ flush', () => {
  for (const rel of ['protocol/PageHandler.js', 'TargetRegistry.js', 'content/PageAgent.js']) {
    assert.doesNotMatch(code(rel), /flushApzRepaints|apz-repaints-flushed/, rel);
  }
});

test('the parent does not scroll the <browser> before an input', () => {
  assert.doesNotMatch(code('protocol/PageHandler.js'), /scrollRectIntoViewIfNeeded/);
});

test('a drag event is acknowledged by its reply, not by a second channel', () => {
  const handler = code('protocol/PageHandler.js');
  assert.doesNotMatch(handler, /ensureEventsAndDispose\(\s*\[\s*'dragover'/);
  assert.match(handler, /dispatchDragEvent', \{type: ?'dragover'/);
  // Nothing reads a reported dragover any more, so nothing reports it.
  assert.doesNotMatch(code('content/FrameTree.js'), /addEventListener\(chromeEventHandler, 'dragover'/);
});

test('the helper that waited on an observer topic without a bound is gone', () => {
  assert.doesNotMatch(code('Helper.js'), /awaitTopic\s*\(/);
});
