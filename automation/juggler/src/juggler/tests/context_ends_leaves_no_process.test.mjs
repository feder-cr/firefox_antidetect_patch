// A context that ends leaves no content process behind: its pages close, its processes are waited for, and the
// ones that stay are killed.
//
// A registered service worker whose script is `for (;;) {}` never answers the shutdown of its process: no shutdown
// starts for it (MOZ_LOG showed no MaybeBeginShutDown for the child), the pages are gone and the process stays for
// good, one more with every context. The context looks for the processes of its container (their remote type
// carries `^userContextId=<id>`) once its pages are gone, waits a short while for them, kills the ones that are
// left, and waits again.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(JUGGLER, 'TargetRegistry.js'), 'utf8');

// The class text of BrowserContext, evaluated with what it reads fakes.
function loadBrowserContext(scope) {
  const start = source.indexOf('class BrowserContext {');
  assert.notEqual(start, -1);
  const end = source.indexOf('\n}\n', start) + 3;
  vm.createContext(scope);
  vm.runInContext(source.slice(start, end) + '\n;globalThis.__BrowserContext = BrowserContext;', scope);
  return scope.__BrowserContext;
}

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

// A context with `pageCount` pages whose targets go away when asked to close, and processes taken from `world`.
function setup({ userContextId = 6, pageCount = 1, processes = [], exits = {} } = {}) {
  const log = [];
  const listeners = [];
  const world = { processes: processes.slice() };
  const registry = {
    _browserContextIdToBrowserContext: new Map(),
    _userContextIdToBrowserContext: new Map(),
    _updateProxiesWithSameAuthCacheAndDifferentCredentials() {},
    _syncTrrPrefs() {},
  };
  const scope = {
    ChromeUtils: { getAllDOMProcesses: () => world.processes.slice() },
    ContextualIdentityService: {
      create: () => ({ userContextId }),
      remove: id => log.push(['identity.remove', id]),
    },
    Services: { ppmm: { sharedData: { has: () => false, set() {}, delete() {}, flush() {} } } },
    Components: { utils: { reportError: error => log.push(['reportError', String(error.message || error)]) } },
    Cc: { '@mozilla.org/processtools-service;1': { getService: () => ({
      kill: pid => {
        log.push(['kill', pid]);
        if (exits.killThrows)
          throw new Error('kill refused');
        world.processes = world.processes.filter(p => p.osPid !== pid);
      },
    }) } },
    Ci: { nsIProcessToolsService: {} },
    helper: {
      on: (target, event, fn) => { listeners.push({ event, fn }); return listeners.length - 1; },
      removeListeners: () => {},
      contextCookieKey: id => 'context-' + id,
    },
    TargetRegistry: { Events: { TargetDestroyed: 'targetdestroyed' } },
    setTimeout,
    Date,
    IDENTITY_NAME: 'JUGGLER ',
    // The wait times of the file, shortened: the structure is what is tested; the real ones are checked below.
    PROCESS_EXIT_WAIT_MS: 120,
    PROCESS_KILL_WAIT_MS: 120,
    PROCESS_POLL_MS: 5,
  };
  const BrowserContext = loadBrowserContext(scope);
  const context = new BrowserContext(registry, 'ctx', false);
  context.userContextId = userContextId;
  for (let i = 0; i < pageCount; i++) {
    const page = {
      close() {
        log.push(['page.close', i]);
        setTimeout(() => {
          context.pages.delete(page);
          log.push(['page.gone', i]);
          for (const l of listeners.slice())
            if (l.event === 'targetdestroyed')
              l.fn();
        }, 5);
      },
    };
    context.pages.add(page);
  }
  return { context, log, world };
}

const names = log => log.map(entry => entry[0] + (entry.length > 1 && entry[0] !== 'identity.remove' ? ':' + entry[1] : ''));

test('the pages close, their targets are gone, and the processes are waited for', async () => {
  const { context, log, world } = setup({
    processes: [{ osPid: 10, remoteType: 'web=^userContextId=6' }],
  });
  // the page's process exits a little after the page is gone
  setTimeout(() => { world.processes = []; log.push(['process.gone']); }, 40);
  await context.destroy();
  const order = names(log);
  assert.deepEqual(order, ['identity.remove', 'page.close:0', 'page.gone:0', 'process.gone']);
});

test('every page is closed and none is waited for in vain', async () => {
  const { context, log } = setup({ pageCount: 3 });
  await context.destroy();
  assert.equal(log.filter(e => e[0] === 'page.close').length, 3);
  assert.equal(log.filter(e => e[0] === 'page.gone').length, 3);
  assert.deepEqual(log.filter(e => e[0] === 'kill'), []);
});

test('a process of the container that stays after the wait is killed, and only that one', async () => {
  const { context, log, world } = setup({
    processes: [
      { osPid: 10, remoteType: 'web=^userContextId=6' },          // outlives the wait (a worker that never stops)
      { osPid: 11, remoteType: 'web=^userContextId=60' },         // another container whose number starts the same
      { osPid: 12, remoteType: 'web=^userContextId=7' },
      { osPid: 13, remoteType: 'privilegedabout' },
      { osPid: 14, remoteType: null },                            // the parent
    ],
  });
  await context.destroy();
  assert.deepEqual(log.filter(e => e[0] === 'kill'), [['kill', 10]], 'only the process of this container is killed');
  const lastGone = log.map(e => e[0]).lastIndexOf('page.gone');
  const killed = log.findIndex(e => e[0] === 'kill');
  assert.ok(killed > lastGone, 'the kill comes after the pages are gone');
  assert.deepEqual(world.processes.map(p => p.osPid), [11, 12, 13, 14], 'the others are left alone');
});

test('a process that exits within the wait is never killed', async () => {
  const { context, log, world } = setup({ processes: [{ osPid: 10, remoteType: 'webIsolated=^userContextId=6&privateBrowsingId=0' }] });
  setTimeout(() => { world.processes = []; }, 30);
  await context.destroy();
  assert.deepEqual(log.filter(e => e[0] === 'kill'), []);
});

test('a kill that is refused is reported and the context still ends', async () => {
  const { context, log } = setup({
    processes: [{ osPid: 10, remoteType: 'web=^userContextId=6' }],
    exits: { killThrows: true },
  });
  await context.destroy();
  assert.ok(log.some(e => e[0] === 'reportError' && /kill refused/.test(e[1])));
  assert.ok(log.some(e => e[0] === 'page.gone'), 'the pages went all the same');
});

test('the default context has no identity to remove and waits for nothing', async () => {
  const { context, log } = setup({ userContextId: 0, processes: [{ osPid: 10, remoteType: 'web=^userContextId=0' }] });
  await context.destroy();
  assert.deepEqual(log, []);
});

test('the waits of the file are the ones measured: three seconds for the exit, two after the kill', () => {
  assert.match(source, /const PROCESS_EXIT_WAIT_MS = 3000;/);
  assert.match(source, /const PROCESS_KILL_WAIT_MS = 2000;/);
  assert.match(source, /const PROCESS_POLL_MS = 20;/);
});

test('the file has the timer it waits with', () => {
  assert.match(source, /const \{setTimeout\} = ChromeUtils\.importESModule\('resource:\/\/gre\/modules\/Timer\.sys\.mjs'\);/);
});
