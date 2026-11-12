// A lookup of a proxied context is a per-request TRR_ONLY request while the DoH
// service stays in global mode 0 (the resolver hook, StealthSessionResolver.cpp).
// TRRService::GetRequestTimeout picks the long TRR-only timeout
// (network.trr.request_timeout_mode_trronly_ms, 30 s) only when the GLOBAL mode is
// 3; in mode 0 it falls through to network.trr.request_timeout_ms (1500 ms in
// Mozilla's file). A DoH request through a SOCKS or HTTP proxy adds the proxy's
// handshakes to the DoH server's own, so it timed out at 1.5 s and, being
// fail-closed, ended as NS_ERROR_UNKNOWN_HOST: a name that does not resolve on a
// slow proxy road.
//
// The smallest source-level fix is the value of that one pref, on Mozilla's own
// line, with the hook on the line: no session without a proxy ever sends a TRR
// request, so the value is invisible there, and the only reader of the pref is
// GetRequestTimeout.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

// The block of one pref in StaticPrefList.yaml: from its "- name:" line to the
// blank line that ends it.
function declaration(name) {
  const text = read('modules/libpref/init/StaticPrefList.yaml');
  const start = text.indexOf(`\n- name: ${name}\n`);
  assert.ok(start >= 0, `${name} is declared`);
  const end = text.indexOf('\n\n', start + 1);
  return text.slice(start + 1, end);
}

function valueLine(name) {
  const lines = declaration(name).split('\n').filter(l => /^  value:/.test(l));
  assert.equal(lines.length, 1, `${name} has one value line`);
  return lines[0];
}

function valueOf(name) {
  const m = valueLine(name).match(/^  value: (\d+)\b/);
  assert.ok(m, `${name} has a plain number as its value`);
  return Number(m[1]);
}

test('premise: the long TRR-only timeout is chosen by the GLOBAL mode only', () => {
  const text = read('netwerk/dns/TRRService.cpp');
  const start = text.indexOf('uint32_t TRRService::GetRequestTimeout() {');
  const end = text.indexOf('\n}\n', start);
  const body = text.slice(start, end);
  assert.match(body, /if \(mMode == nsIDNSService::MODE_TRRONLY\) \{\n    return StaticPrefs::network_trr_request_timeout_mode_trronly_ms\(\);/);
  assert.match(body, /\n  return StaticPrefs::network_trr_request_timeout_ms\(\);\n?$/);
  // Neither the strict fallback nor the Android rollout branch is reached with
  // the defaults of this build.
  assert.match(declaration('network.trr.strict_native_fallback'), /value: false/);
});

test('premise: GetRequestTimeout is the only reader of network.trr.request_timeout_ms', () => {
  const dir = path.join(TREE, 'netwerk', 'dns');
  const readers = fs.readdirSync(dir)
    .filter(f => /\.(cpp|h)$/.test(f))
    .filter(f => fs.readFileSync(path.join(dir, f), 'utf8').includes('network_trr_request_timeout_ms()'));
  assert.deepEqual(readers, ['TRRService.cpp']);
});

test('the single request timeout of a proxied lookup is the TRR-only one', () => {
  assert.equal(valueOf('network.trr.request_timeout_ms'),
    valueOf('network.trr.request_timeout_mode_trronly_ms'));
  // And a DoH answer through a slow proxy road has seconds, not 1.5 s.
  assert.ok(valueOf('network.trr.request_timeout_ms') >= 30000);
});

test('the hook is on the value line of Mozilla\'s pref, which is declared nowhere else', () => {
  assert.match(valueLine('network.trr.request_timeout_ms'), /# STEALTH-HOOK\(network\/doh\): /);
  const pref = /network\.trr\.request_timeout_ms/;
  for (const file of ['modules/libpref/init/StealthPrefs.js', 'modules/libpref/init/StealthPrefList.yaml'])
    assert.doesNotMatch(read(file), pref, file);
  for (const file of ['modules/libpref/init/all.js', 'browser/app/profile/firefox.js'])
    assert.doesNotMatch(read(file), pref, file);
});
