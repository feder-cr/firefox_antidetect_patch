// Page.scrollIntoViewIfNeeded, loaded the way the content process loads
// PageAgent.js, on a node as far as the command reads it.
//
// The command, declared in Protocol.js, failed on every node with "Node does
// not have a layout object": the handler called
// Element.scrollRectIntoViewIfNeeded, a method upstream adds to Element's
// binding and this tree does not (B184). Measured on the shipped binary at four
// positions, including an element already in view. The clients rebuilt the
// scroll on their side, in a script injected into the page
// (invisible_core InjectedScript.scroll_into_view), and dropped `rect`.
//
// The engine now answers it with the page's own scrollIntoView, reached through
// the Xray: only when the node (or the rect asked for, in the node's box) is
// not in full view in its frame and every frame above, centred, as upstream.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

class Helper {}
const { PageAgent } = load('content/PageAgent.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  Cc: new Proxy({}, { get: () => ({ getService: () => ({}) }) }),
  ChromeUtils: { importESModule: () => ({ Helper, NetUtil: {}, setTimeout }) },
  dump: () => {},
}, ['PageAgent']);

// A window 800x600 whose page scrolls, and one element in it at `box`.
function page(box, { rendered = true } = {}) {
  const scrolls = [];
  const win = {
    innerWidth: 815, innerHeight: 600,   // a 15px vertical scrollbar
    document: { documentElement: { clientWidth: 800, clientHeight: 600 } },
    scrollBy(opts) {
      scrolls.push(['scrollBy', opts]);
      box = { ...box, top: box.top - opts.top, bottom: box.bottom - opts.top,
              left: box.left - opts.left, right: box.right - opts.left };
    },
  };
  win.top = win;
  const el = {
    nodeType: 1, isConnected: true, ownerDocument: { defaultView: win },
    getClientRects: () => rendered ? [box] : [],
    getBoundingClientRect: () => box,
    scrollIntoView(opts) {
      scrolls.push(['scrollIntoView', opts]);
      const h = box.bottom - box.top, w = box.right - box.left;
      const top = 300 - h / 2, left = 400 - w / 2;
      box = { top, bottom: top + h, left, right: left + w };
    },
  };
  const agent = Object.create(PageAgent.prototype);
  agent._frameTree = { frame: () => ({ unsafeObject: () => el }) };
  const scroll = rect => agent._scrollIntoViewIfNeeded({ objectId: 'o', frameId: 'f', rect });
  return { scroll, scrolls, box: () => box };
}

test('an element in full view is left where it is', async () => {
  const p = page({ top: 100, bottom: 140, left: 10, right: 200 });
  await p.scroll();
  assert.deepEqual(p.scrolls, []);
});

test('an element below the fold is brought to the centre with the native method', async () => {
  const p = page({ top: 3000, bottom: 3040, left: 10, right: 200 });
  await p.scroll();
  // Compared as JSON: the options object comes from the module's own realm.
  assert.equal(JSON.stringify(p.scrolls),
               JSON.stringify([['scrollIntoView', { block: 'center', inline: 'center', behavior: 'instant' }]]));
  assert.equal(p.box().top, 280);
});

test('an element half under the scrollbar is not in full view', async () => {
  const p = page({ top: 100, bottom: 140, left: 700, right: 810 });
  await p.scroll();
  assert.equal(p.scrolls.length, 1);
});

test('a rect in view inside an element that is not is left where it is', async () => {
  const p = page({ top: -500, bottom: 1500, left: 0, right: 400 });
  await p.scroll({ x: 10, y: 600, width: 0, height: 0 });
  assert.deepEqual(p.scrolls, []);
});

test('a rect out of view in an element taller than the viewport is centred', async () => {
  const p = page({ top: 3000, bottom: 6000, left: 0, right: 400 });
  await p.scroll({ x: 10, y: 2900, width: 0, height: 0 });
  assert.equal(p.scrolls[0][0], 'scrollIntoView');
  assert.equal(p.scrolls[1][0], 'scrollBy');
  const b = p.box();
  assert.equal(b.top + 2900, 300);
});

test('a node with no layout says so', async () => {
  const p = page({ top: 0, bottom: 0, left: 0, right: 0 }, { rendered: false });
  await assert.rejects(p.scroll(), /Node does not have a layout object/);
});
