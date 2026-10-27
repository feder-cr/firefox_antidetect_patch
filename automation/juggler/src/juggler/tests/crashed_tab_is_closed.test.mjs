// A tab whose content process crashed is closed once the client is told.
//
// Until firefox-48 the crash observer (oop-frameloader-crashed, TargetRegistry.js)
// emitted Crashed and disposed the target, and nothing removed the tab: it
// stayed in the parent until the browser exited, 15-60 MB a crash, and no
// command reached it any more. Measured on a headed firefox-47, crashing with
// about:crashcontent and counting the process's top-level windows: the crashed
// page's window stayed open in every arm; with this remedy it closes, except
// as the only window of a persistent context, where closing it would quit the
// browser.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const code = rel => fs.readFileSync(path.join(JUGGLER, rel), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

test('the crash observer closes the tab after disposing its target', () => {
  const registry = code('TargetRegistry.js');
  const observer = registry.slice(0, registry.indexOf("'oop-frameloader-crashed'"));
  const tail = observer.slice(observer.lastIndexOf('target.emit(PageTarget.Events.Crashed'));
  // dispose() first: it takes the target out of _browserToTarget, so the
  // TabClose that removeTab fires finds nothing to dispose a second time.
  assert.match(tail, /^target\.emit\(PageTarget\.Events\.Crashed\);\s*target\.dispose\(\);\s*target\.closeCrashedTab\(\);/);
});

test('only a browser that does not survive its last window keeps the tab', () => {
  const registry = code('TargetRegistry.js');
  const body = registry.slice(registry.indexOf('closeCrashedTab() {'));
  const method = body.slice(0, body.indexOf('\n  }\n') + 4);
  assert.match(method, /!this\._registry\._survivesLastWindow/);
  assert.match(method, /removeTab\(this\._tab, \{ skipPermitUnload: true \}\)/);
  // The flag is Juggler's -silent, which holds the browser in the
  // last-window-closing survival area.
  assert.match(code('components/Juggler.js'), /new TargetRegistry\(\{ survivesLastWindow: this\._silent \}\)/);
});
