// The warnings that explain a hang (a driver that never decides the proxy, a
// proxy answer that failed, an early hold that could not be registered) were
// written with a bare dump(). In an official build dump() prints nothing:
// nsJSUtils::DumpEnabled() falls back on browser.dom.window.dump.enabled, whose
// default is false there. The hang was silent, which is the thing the line
// exists to prevent.
//
// They now go through say() (juggler/Say.js), which writes to stderr with
// Cu.printStderr: not gated by that pref, no pref touched, nothing for a page
// to read. The one deliberate dump() left is the 'Juggler listening to the
// pipe' line, which the client waits for on stdout and which Juggler.js
// enables the pref around.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = rel => fs.readFileSync(path.join(ROOT, rel), 'utf8');

function load(file, scope, names) {
  const source = read(file).replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

const bareDumps = text => text.split('\n').filter(l => /(^|[^.\w])dump\(/.test(l) && !l.trim().startsWith('//'));

test('source: no bare dump() writes these warnings any more', () => {
  assert.deepEqual(bareDumps(read('EarlyProxyHold.js')), []);
  assert.deepEqual(bareDumps(read('NetworkObserver.js')), []);
  // Juggler.js keeps exactly one dump(): the readiness line the client waits for.
  const left = bareDumps(read('components/Juggler.js'));
  assert.equal(left.length, 1);
  assert.match(left[0], /Juggler listening to the pipe/);
  for (const file of ['EarlyProxyHold.js', 'NetworkObserver.js', 'components/Juggler.js'])
    assert.match(read(file), /\bsay\(/, file);
});

test('source: the helper is one small module, listed in moz.build, with no pref in it', () => {
  assert.match(read('moz.build'), /"Say\.js",/);
  const text = read('Say.js');
  assert.match(text, /printStderr/);
  assert.doesNotMatch(text, /dump\(|Services\.prefs|logStringMessage/);
});

test('say() writes one line to the non-gated sink and never to dump or prefs', () => {
  const written = [];
  const dumped = [];
  const prefs = [];
  const { say } = load('Say.js', {
    Components: { utils: { printStderr: text => written.push(text) } },
    dump: text => dumped.push(text),
    Services: { prefs: new Proxy({}, { get: (_, name) => () => prefs.push(name) }) },
  }, ['say']);
  say('juggler: something');
  say('already ended\n');
  assert.deepEqual(written, ['juggler: something\n', 'already ended\n']);
  assert.deepEqual(dumped, []);
  assert.deepEqual(prefs, []);
});

test('say() never throws, whatever the sink does', () => {
  const { say } = load('Say.js', {
    Components: { utils: { printStderr: () => { throw new Error('closed'); } } },
  }, ['say']);
  say('x');
  const { say: bare } = load('Say.js', { Components: { utils: {} } }, ['say']);
  bare('x');
});

test('the warnings of the hold and of the observer arrive at the sink, with dump stubbed to a no-op', () => {
  const written = [];
  const say = text => written.push(text);
  const components = {
    classes: { '@mozilla.org/network/protocol-proxy-service;1': { getService: () => ({ registerChannelFilter() {} }) } },
    interfaces: { nsIProtocolProxyChannelFilter: {} }, utils: {}, results: {},
  };
  const { EarlyProxyHold } = load('EarlyProxyHold.js', {
    Components: components,
    ChromeUtils: { generateQI: () => () => {}, importESModule: () => ({ say }) },
    dump: () => { throw new Error('bare dump used'); },
  }, ['EarlyProxyHold']);
  EarlyProxyHold.noteHeld();
  EarlyProxyHold.noteHeld();
  assert.equal(written.length, 1);
  assert.match(written[0], /waiting for Browser\.setBrowserProxy or Browser\.setNoBrowserProxy/);
});
