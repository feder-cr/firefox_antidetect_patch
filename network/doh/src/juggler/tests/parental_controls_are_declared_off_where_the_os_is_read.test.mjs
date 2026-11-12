// Parental controls are declared off, and the declaration lives where the OS is
// read: the constructor of every implementation of nsIParentalControlsService
// that our builds compile, Windows and the default one (Linux)
// (toolkit/components/parentalcontrols/). The Android and macOS implementations
// are not compiled by any build of ours and stay byte for byte as Mozilla wrote
// them (last test). Every reader then sees the same value
// without a hook of its own: nsHttpHandler (TRR skip, "Prefer: safe"),
// CanonicalBrowsingContext::ShouldEnforceParentalControls, DownloadIntegration
// and the DoH heuristics all go through the service. nsHttpHandler is upstream's.
// The service, created with do_CreateInstance, reads the OS at construction, so
// the hook is before the first OS call.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');
const DIR = 'toolkit/components/parentalcontrols';
const HOOK = 'if (StealthParentalControlsDeclared(&mEnabled)) {';

// The implementations our builds compile: Windows x86_64 and Linux x86_64.
const implementations = fs.readdirSync(path.join(TREE, DIR))
    .filter(f => /^nsParentalControlsService(Win|Default)\.cpp$/.test(f));
const OUT_OF_PERIMETER = ['nsParentalControlsServiceAndroid.cpp', 'nsParentalControlsServiceCocoa.mm'];

function constructorBody(text) {
  const start = text.indexOf('nsParentalControlsService::nsParentalControlsService()');
  assert.ok(start >= 0, 'no constructor');
  const open = text.indexOf('{', start);
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    if (text[i] === '{') depth++;
    if (text[i] === '}' && --depth === 0)
      return text.slice(open + 1, i);
  }
  throw new Error('unterminated constructor');
}

// What reads the host, per implementation.
const OS_CALLS = [
  'GetUserSid(', 'windows-registry-key', 'regKey', 'OpenProcessToken',
  'CFPreferences', 'CFSTR(', 'GetWebSettings', 'GetUserSettings',
];

test('the implementations our builds compile are known to this test', () => {
  assert.deepEqual(implementations.sort(),
      ['nsParentalControlsServiceDefault.cpp', 'nsParentalControlsServiceWin.cpp']);
});

test('the declared value is set in the constructor of every implementation', () => {
  for (const file of implementations) {
    const body = constructorBody(read(`${DIR}/${file}`));
    assert.ok(body.includes(HOOK), `${file}: no declaration in the constructor`);
    assert.match(body, /STEALTH-HOOK\(network\/doh\)/, file);
  }
});

test('the declaration comes before the first OS call', () => {
  for (const file of implementations) {
    const body = constructorBody(read(`${DIR}/${file}`));
    const hook = body.indexOf(HOOK);
    assert.ok(hook >= 0, file);
    assert.match(body.slice(hook, hook + 120), /\{\s*return;\s*\}/, `${file}: the hook does not return`);
    for (const call of OS_CALLS) {
      const at = body.indexOf(call);
      if (at >= 0)
        assert.ok(at > hook, `${file}: ${call} is read before the declaration`);
    }
  }
});

test('the Windows implementation reads the OS only in its constructor, behind the hook', () => {
  const text = read(`${DIR}/nsParentalControlsServiceWin.cpp`);
  // mPC (the old COM API) is only ever created as null, so the COM API is never called.
  assert.doesNotMatch(text, /CoCreateInstance|mPC\s*=/);
  assert.doesNotMatch(text.replace(constructorBody(text), ''), /windows-registry-key|\(GetUserSid\(\)\)/);
});

test('the declared value is false, in our header', () => {
  const header = read(`${DIR}/StealthParentalControls.h`);
  assert.match(header, /inline bool StealthParentalControlsDeclared\(bool\* aEnabled\) \{\s*\*aEnabled = false;\s*return true;\s*\}/);
  for (const file of implementations)
    assert.match(read(`${DIR}/${file}`), /#include "StealthParentalControls\.h"  \/\/ STEALTH-HOOK\(network\/doh\)/, file);
});

test('nsHttpHandler is upstream: it has no parental controls hook of ours', () => {
  const text = read('netwerk/protocol/http/nsHttpHandler.cpp');
  assert.doesNotMatch(text, /Stealth/);
  const start = text.indexOf('void nsHttpHandler::UpdateParentalControlsEnabled(bool waitForCompletion) {');
  const os = text.indexOf('do_CreateInstance("@mozilla.org/parental-controls-service;1")', start);
  const child = text.indexOf('if (!XRE_IsParentProcess()) {', start);
  assert.ok(start > 0 && child > start && os > child);
  // Nothing sits between the child-process return and the task that asks the service.
  assert.equal(text.slice(child, os),
      'if (!XRE_IsParentProcess()) {\n    return;\n  }\n\n' +
      '  auto getParentalControlsTask = []() {\n' +
      '    nsCOMPtr<nsIParentalControlsService> pc =\n        ');
});

test('the former resolver-side declaration is gone', () => {
  for (const f of ['netwerk/dns/StealthParentalControls.h', 'netwerk/dns/StealthParentalControls.cpp'])
    assert.equal(fs.existsSync(path.join(TREE, f)), false, f);
  assert.doesNotMatch(read('netwerk/dns/moz.build'), /StealthParentalControls/);
});

test('the other readers take the value from the service', () => {
  assert.match(read('netwerk/dns/TRRService.cpp'),
      /return nsHttpHandler::GetParentalControlsEnabled\(\);/);
  for (const file of ['netwerk/dns/TRRService.cpp', 'netwerk/dns/TRRServiceParent.cpp',
    'netwerk/dns/nsHostResolver.cpp', 'netwerk/dns/nsDNSService2.cpp'])
    assert.doesNotMatch(read(file), /do_CreateInstance\("@mozilla\.org\/parental-controls-service;1"\)/, file);
  assert.match(read('toolkit/components/build/components.conf'),
      /'contract_ids': \['@mozilla\.org\/parental-controls-service;1'\],\n\s+'type': 'nsParentalControlsService',/);
});

test('the implementations no build of ours compiles (Android, macOS) are as Mozilla wrote them', () => {
  for (const file of OUT_OF_PERIMETER) {
    const text = read(`${DIR}/${file}`);
    assert.doesNotMatch(text, /Stealth/, file);
    let diff = null;
    try {
      diff = execFileSync('git', ['diff', 'FIREFOX_159_0_RELEASE', '--', `${DIR}/${file}`],
        { cwd: TREE, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
    } catch (e) { /* no tag in this checkout */ }
    if (diff !== null) assert.equal(diff, '', `${file} differs from the release`);
  }
});
