// Heap.minimizeMemory: the memory manager's MinimizeMemoryUsage (what about:memory's "Minimize memory usage"
// runs in the parent), as a Juggler command.
//
// Heap.collectGarbage forces a GC and a cycle collection in the parent and, through child-gc-request and
// child-cc-request, in the children. It leaves what a GC does not own: decoded images, caches, the free pages of
// the allocator, the parent's own heap. Measured on five heavy pages (cnbc, scmp, hismileteeth, nytimes, politico)
// in one reused tab, after about:blank and ten seconds later, the biggest content process held on average
// 254 MB with nothing done, 199 MB after collectGarbage and 127 MB after minimizeMemory; the parent lost 80 MB
// to the new command and 11 MB to collectGarbage; a content report showed 13 MB explicit against 36-39, with the
// decoded images (16-20 MB) gone. The wait on the parent was 0.08 s against 0.02 s.
//
// The memory manager's MinimizeMemoryUsage fires three heap-minimize notifications in this process, which
// ContentParent forwards to every child, then frees the allocator's dirty pages, then calls back. about:memory
// also sends child-mmu-request before it, so that every child minimizes itself in full: measured on the same five
// pages it changes nothing (content 127 MB and parent -79 MB with it, 127 and -80 without), so the command does not.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const MANAGER = '@mozilla.org/memory-reporter-manager;1';

function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

const anything = new Proxy(function () {}, { get: () => class {} });

// A PageHandler loaded with a fake memory manager, a fake clock and the notifications the real one would fire.
function handler({ answers = true } = {}) {
  const log = { minimized: 0, timers: [], cleared: [], notifications: [] };
  const manager = {
    minimizeMemoryUsage(callback) {
      log.minimized++;
      log.callback = callback;
      if (answers)
        callback();
    },
  };
  const Services = { obs: { notifyObservers: (...args) => log.notifications.push(args) } };
  const Cu = { forceGC: () => log.notifications.push(['forceGC']), forceCC: () => log.notifications.push(['forceCC']) };
  const { PageHandler } = load('protocol/PageHandler.js', {
    Components: { classes: { [MANAGER]: { getService: () => manager } },
                  interfaces: { nsIMemoryReporterManager: {} }, utils: Cu, results: {} },
    ChromeUtils: {
      importESModule: url => /Timer\.sys\.mjs/.test(url)
        ? { setTimeout: (fn, ms) => { log.timers.push({ fn, ms }); return log.timers.length; },
            clearTimeout: id => log.cleared.push(id) }
        : anything,
    },
    Services,
    dump: () => {},
  }, ['PageHandler']);
  return { pageHandler: Object.create(PageHandler.prototype), log };
}

test('Heap.minimizeMemory asks the memory manager to minimize, once', async () => {
  const { pageHandler, log } = handler();
  await pageHandler['Heap.minimizeMemory']();
  assert.equal(log.minimized, 1);
});

test('it answers when the manager has called back, not before', async () => {
  const { pageHandler, log } = handler({ answers: false });
  let done = false;
  const pending = pageHandler['Heap.minimizeMemory']().then(() => { done = true; });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(done, false, 'the callback has not come: the command is still waiting');
  log.callback();
  await pending;
  assert.equal(done, true);
  assert.equal(log.cleared.length, 1, 'the timer that bounds the wait is cleared once the callback came');
});

test('a callback that never comes does not hold the command for good', async () => {
  const { pageHandler, log } = handler({ answers: false });
  const pending = pageHandler['Heap.minimizeMemory']();
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(log.timers.length, 1);
  assert.equal(log.timers[0].ms, 30000);
  log.timers[0].fn();
  await pending;
});

test('it is not a garbage collection: collectGarbage keeps doing its own work and minimize does not do it', async () => {
  const { pageHandler, log } = handler();
  await pageHandler['Heap.minimizeMemory']();
  assert.deepEqual(log.notifications, [], 'minimizeMemory forces no GC or CC of its own and notifies no child itself');
  await pageHandler['Heap.collectGarbage']();
  assert.deepEqual(log.notifications.map(n => n[0] === 'forceGC' || n[0] === 'forceCC' ? n[0] : n[1]),
                   ['child-gc-request', 'forceGC', 'child-cc-request', 'forceCC']);
  assert.equal(log.minimized, 1, 'collectGarbage does not minimize');
});

test('the protocol declares the command on the page target, with no parameters', () => {
  const source = fs.readFileSync(path.join(ROOT, 'protocol/Protocol.js'), 'utf8');
  const heap = source.slice(source.indexOf('const Heap = {'));
  const block = heap.slice(0, heap.indexOf('\n};\n'));
  assert.match(block, /targets: \['page'\]/);
  assert.match(block, /'collectGarbage': \{\s*params: \{\},\s*\},\s*'minimizeMemory': \{\s*params: \{\},\s*\}/);
});
