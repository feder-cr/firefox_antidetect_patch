// The crash of an iframe's process does not close the tab.
//
// With a process per site (Fission), an out-of-process iframe's BrowserParent has the TAB's <browser> as its owner
// element (BrowserBridgeParent::Init, SetOwnerElement), so `oop-frameloader-crashed` is notified with the tab's
// frameloader for the crash of an iframe too. The observer used to answer every notification the same way: emit
// Page.crashed, dispose the target, close the tab. A page whose only broken part was one iframe was closed, and the
// client was told the whole page crashed.
//
// The engine now passes the crashed BrowsingContext's id as the notification's data (nsFrameLoader::MaybeNotifyCrashed);
// the observer takes the iframe branch for a context that has a parent: the page stays, the client is told the frame is
// gone (Page.frameDetached) and never Page.crashed.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const TREE = path.join(JUGGLER, '..');
const strip = text => text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
const code = rel => strip(fs.readFileSync(path.join(JUGGLER, rel), 'utf8'));

function observer() {
  const registry = code('TargetRegistry.js');
  const end = registry.indexOf("'oop-frameloader-crashed'");
  const start = registry.lastIndexOf('observe: (subject, topic, data) => {', end);
  assert.ok(start > 0 && end > start, 'the crash observer is where this test looks for it');
  return registry.slice(start, end);
}

test('a crashed iframe takes a branch of its own, before the tab branch', () => {
  const body = observer();
  const iframe = body.indexOf('crashed.parent');
  const tab = body.indexOf('target.emit(PageTarget.Events.Crashed');
  assert.ok(iframe > 0, 'the observer looks at the crashed context\'s parent');
  assert.ok(tab > iframe, 'the iframe branch comes first');
  // the context comes from the data the engine passes, and only when there is one
  assert.match(body, /data \? BrowsingContext\.get\(data\) : null/);
});

test('the iframe branch neither emits Crashed, nor disposes the target, nor closes the tab', () => {
  const body = observer();
  const branch = body.slice(body.indexOf('if (crashed && crashed.parent)'), body.indexOf('target.emit(PageTarget.Events.Crashed'));
  assert.match(branch, /target\.frameCrashed\(crashed\);\s*return;/);
  assert.doesNotMatch(branch, /Events\.Crashed|dispose\(|closeCrashedTab/);
});

test('the tab branch is the one the pinned test reads, unchanged and last', () => {
  const body = observer();
  const tail = body.slice(body.lastIndexOf('target.emit(PageTarget.Events.Crashed'));
  assert.match(tail, /^target\.emit\(PageTarget\.Events\.Crashed\);\s*target\.dispose\(\);\s*target\.closeCrashedTab\(\);/);
});

test('the target names the crashed frame the way the content side does, and tells the client it is detached, not crashed', () => {
  const registry = code('TargetRegistry.js');
  const method = registry.slice(registry.indexOf('frameCrashed(browsingContext) {'));
  const frameCrashed = method.slice(0, method.indexOf('\n  }\n') + 4);
  assert.match(frameCrashed, /helper\.browsingContextToFrameId\(browsingContext\)/);
  assert.match(frameCrashed, /Events\.FrameCrashed/);
  assert.doesNotMatch(frameCrashed, /Events\.Crashed|closeCrashedTab|dispose\(/);

  // the page handler forgets the frame and the frames below it as it forgets a frame the lifecycle detached, which is
  // what tells the client (`Page.frameDetached`) for those it knows
  const handler = code('protocol/PageHandler.js');
  const wiring = handler.slice(handler.indexOf('PageTarget.Events.FrameCrashed'));
  assert.match(wiring.slice(0, 500), /framesBelow\(frameId\)[\s\S]*this\._onFrameDetached\(id\)/);
  assert.doesNotMatch(wiring.slice(0, 500), /emitEvent\('Page\.frameDetached'/);
  // and the mux ends the roots of the crashed frame before the client is told
  assert.match(frameCrashed, /this\._contentMux\.disposeRootsOf\(browsingContext\);\s*this\.emit\(/);
});

test('the engine passes the crashed context\'s id as the notification data, and nothing when it does not know it', () => {
  // The logic lives in a file of ours; nsFrameLoader.cpp keeps an include and a hook of a few lines.
  const source = strip(fs.readFileSync(path.join(TREE, 'dom/base/nsFrameLoader.cpp'), 'utf8'));
  const fn = source.slice(source.indexOf('void nsFrameLoader::MaybeNotifyCrashed('));
  const notify = fn.slice(0, 1500);
  assert.match(notify, /nsAutoString crashedContext;/);
  assert.match(notify, /"oop-frameloader-crashed",\s*StealthCrashedContextData\(aBrowsingContext, crashedContext\)\)/);
  assert.match(source, /#include "StealthFrameCrash\.h"/);

  const header = strip(fs.readFileSync(path.join(TREE, 'dom/base/StealthFrameCrash.h'), 'utf8'));
  const helper = header.slice(header.indexOf('StealthCrashedContextData('));
  // nothing when the caller does not say which context crashed; the id as the data otherwise
  assert.match(helper, /if \(!aBrowsingContext\) \{\s*return nullptr;\s*\}/);
  assert.match(helper, /aBuffer\.AppendInt\(aBrowsingContext->Id\(\)\)/);
  assert.match(helper, /return aBuffer\.get\(\);/);
});
