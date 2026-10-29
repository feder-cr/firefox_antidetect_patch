// The road of the pingsender background task (B266) lives in our own file,
// toolkit/components/telemetry/app/StealthPingsender.sys.mjs, and
// TelemetrySend.sys.mjs only calls it from marked one-line hooks.
//
// Two halves: a source guard that fails if the logic comes back into the
// Mozilla file, and a behaviour check of the moved functions on stubs, so the
// move cannot change what the child gets (the program and its arguments, the
// environment variable around the start of the one child, the proxy record).
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const APP = path.join(TREE, 'toolkit/components/telemetry/app');
const HOOK = 'STEALTH-HOOK(stillborn/telemetry)';

test('TelemetrySend.sys.mjs holds only hooks of the pingsender road', () => {
  const text = fs.readFileSync(path.join(APP, 'TelemetrySend.sys.mjs'), 'utf8');
  assert.doesNotMatch(text, /Services\.env/);
  assert.doesNotMatch(text, /TargetRegistry/);
  assert.doesNotMatch(text, /--backgroundtask/);
  assert.doesNotMatch(text, /\.notificationCallbacks\s*=[^=]/);
  assert.doesNotMatch(text, /function sessionRoute/);
  const named = text.split('\n').filter(l => l.includes('StealthPingsender'));
  // Near-miss guard: the hooks exist, so an empty list is a broken test.
  assert.ok(named.length >= 5, `only ${named.length} lines name StealthPingsender`);
  for (const line of named)
    assert.ok(line.includes(HOOK), `a hook without its marker: ${line}`);
});

// Load the own file on stubs: the one import is replaced by a stub object and
// `export` is dropped.
function load(platform, globals) {
  let src = fs.readFileSync(path.join(APP, 'StealthPingsender.sys.mjs'), 'utf8');
  src = src.replace(/^import \{ AppConstants \}.*$/m, 'const AppConstants = __AppConstants;');
  src = src.replace(/^export /gm, '');
  src += '\nmodule.exports = { PINGSENDER_ROUTE_ENV, attachCallbacks, invocation, runAsync };\n';
  const sandbox = {
    __AppConstants: { MOZ_APP_NAME: 'firefox', platform },
    module: { exports: {} },
    Ci: { nsIFile: 'nsIFile' },
    ...globals,
  };
  vm.runInNewContext(src, sandbox);
  return sandbox.module.exports;
}

function stubs({ loaded = false, registry = null, log = [] } = {}) {
  return {
    log,
    globals: {
      Services: {
        env: { set: (k, v) => log.push(['env', k, v]), get: () => '' },
        dirsvc: { get: () => ({ append() {}, path: 'P/prefs.js' }) },
      },
      Cu: { isESModuleLoaded: () => loaded },
      ChromeUtils: { importESModule: () => ({ TargetRegistry: { instance: () => registry } }) },
    },
  };
}

test('the program and its arguments', () => {
  const win = load('win', stubs().globals);
  const a = win.invocation();
  assert.equal(a.exeName, 'firefox.exe');
  assert.equal(JSON.stringify(a.params), '["--backgroundtask","pingsender"]');
  assert.notEqual(win.invocation().params, a.params, 'params must be a fresh array');
  assert.equal(load('linux', stubs().globals).invocation().exeName, 'firefox');
  assert.equal(win.PINGSENDER_ROUTE_ENV, 'STEALTHFOX_PINGSENDER_ROUTE');
});

test('the callbacks reach the channel only when given', () => {
  const m = load('win', stubs().globals);
  const bare = { channel: {} };
  m.attachCallbacks(bare, null);
  assert.equal('notificationCallbacks' in bare.channel, false);
  const cb = {};
  const req = { channel: {} };
  m.attachCallbacks(req, cb);
  assert.equal(req.channel.notificationCallbacks, cb);
});

test('the road is in the environment only around the start of the child', () => {
  const s = stubs();
  const m = load('win', s.globals);
  const observer = {};
  const proc = {
    runAsync(params, n, obs) {
      s.log.push(['run', JSON.stringify(params), n, obs === observer]);
    },
  };
  m.runAsync(proc, ['a'], observer);
  assert.equal(s.log.length, 3);
  assert.equal(s.log[0][0], 'env');
  assert.equal(s.log[0][1], 'STEALTHFOX_PINGSENDER_ROUTE');
  assert.equal(s.log[0][2], JSON.stringify({ proxy: null, prefs: 'P/prefs.js' }));
  assert.deepEqual(s.log[1], ['run', '["a"]', 1, true]);
  assert.deepEqual(s.log[2], ['env', 'STEALTHFOX_PINGSENDER_ROUTE', '']);
});

test('the environment is reset when the child cannot be started', () => {
  const s = stubs();
  const m = load('win', s.globals);
  const proc = { runAsync() { throw new Error('no start'); } };
  assert.throws(() => m.runAsync(proc, [], {}), /no start/);
  assert.deepEqual(s.log.at(-1), ['env', 'STEALTHFOX_PINGSENDER_ROUTE', '']);
});

test('the proxy record comes from the target registry when Juggler loaded it', () => {
  const run = registry => {
    const s = stubs({ loaded: true, registry });
    load('win', s.globals).runAsync({ runAsync() {} }, [], {});
    return JSON.parse(s.log[0][2]).proxy;
  };
  assert.deepEqual(run({ proxyRecordFor: id => ({ id }) }), { id: 0 });
  assert.equal(run(null), null);
});
