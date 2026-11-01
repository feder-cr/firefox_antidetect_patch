// A window in fullscreen is the screen, its content included: retail gives
// outer == inner == screen. Juggler pins the page's <browser> to the viewport
// size the session asks for (TargetRegistry.js, updateViewportSize), and the
// pin held through fullscreen: the window filled the screen while the page
// kept the maximized window's viewport. The pin is now decided by
// pinnedViewportSize, which lets go in fullscreen, and is decided again on the
// chrome window's "fullscreen" event, on entering and on leaving.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(JUGGLER, 'TargetRegistry.js'), 'utf8');
// The code, not the comments that explain it.
const code = source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

function pinnedViewportSize() {
  const m = source.match(/function pinnedViewportSize\([^)]*\) \{[\s\S]*?\n\}/);
  assert.ok(m, 'pinnedViewportSize is gone from TargetRegistry.js');
  return new Function(`${m[0]}; return pinnedViewportSize;`)();
}

test('in fullscreen the <browser> follows the window', () => {
  const pin = pinnedViewportSize();
  const page = { width: 1280, height: 720 };
  const context = { width: 1920, height: 947 };
  assert.equal(pin(true, page, context), undefined);
  assert.equal(pin(true, undefined, context), undefined);
});

test('outside fullscreen the page viewport wins over the context default', () => {
  const pin = pinnedViewportSize();
  const page = { width: 1280, height: 720 };
  const context = { width: 1920, height: 947 };
  assert.equal(pin(false, page, context), page);
  assert.equal(pin(false, undefined, context), context);
  assert.equal(pin(false, undefined, undefined), undefined);
});

test('updateViewportSize asks pinnedViewportSize with the window state', () => {
  const body = code.match(/async updateViewportSize\(\) \{[\s\S]*?\n {2}\}/);
  assert.ok(body, 'updateViewportSize not found');
  assert.match(body[0],
      /pinnedViewportSize\(this\._window\.fullScreen, this\._viewportSize, this\._browserContext\.defaultViewportSize\)/);
  assert.doesNotMatch(body[0], /this\._viewportSize \|\| this\._browserContext\.defaultViewportSize/);
});

test('the chrome window\'s fullscreen event decides the pin again', () => {
  assert.match(code,
      /helper\.addEventListener\(this\._window, 'fullscreen', \(\) => this\.updateViewportSize\(\)/);
});
