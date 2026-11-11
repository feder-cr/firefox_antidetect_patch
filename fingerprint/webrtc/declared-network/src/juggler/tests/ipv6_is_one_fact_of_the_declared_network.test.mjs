// IPv6 is ONE fact for the whole browser, and it is the declared network's.
//
// The core declares the host network in STEALTHFOX_WEBRTC_NETWORK
// (v4=<ip>;v6=<0|1>;p6=<prefix>). ICE already answered from it, but HTTP and
// DNS asked the host: a page could load over IPv6 while WebRTC said "no IPv6".
// Now the parent derives network.dns.disableIPv6 from the SAME parse, once, in
// nsIOService::Init (a default value, so nothing reaches the profile); the
// static pref reaches the socket and content processes by the pref sync.
//
// Source guards (the C++ is compiled and run by the candidate's gtest filter,
// suite StealthDeclaredNetwork): nothing else writes the pref, the hook is the
// one line in the parent, and the accessor is the parser's own answer.
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
const NETWORK = 'dom/media/webrtc/transport/nr_stealth_network';

test('nothing but the declared network writes network.dns.disableIPv6 (juggler, prefs files)', () => {
  const files = [
    'modules/libpref/init/StealthPrefs.js',
    'modules/libpref/init/StealthPrefList.yaml',
    'modules/libpref/init/all.js',
    'modules/libpref/greprefs.js',
    'browser/app/profile/firefox.js',
  ];
  for (const rel of files)
    assert.doesNotMatch(read(rel), /disableIPv6/, rel);
  const juggler = [];
  const walk = dir => {
    for (const entry of fs.readdirSync(path.join(TREE, dir), { withFileTypes: true })) {
      if (entry.name === 'tests') continue;
      const rel = dir + '/' + entry.name;
      if (entry.isDirectory()) walk(rel);
      else if (/\.(js|mjs|jsm|cpp|h)$/.test(entry.name)) juggler.push(rel);
    }
  };
  walk('juggler');
  for (const rel of juggler)
    assert.doesNotMatch(read(rel), /disableIPv6/, rel);
});

test('in the C++ sources outside tests the pref is written by one function only', () => {
  const out = execFileSync('git', ['grep', '-lF', 'network.dns.disableIPv6', '--',
    '*.cpp', '*.h', '*.mm', '*.js', '*.mjs', '*.sys.mjs', '*.yaml', ':!*/test/*', ':!*/tests/*', ':!*test*.js'],
  { cwd: TREE, encoding: 'utf8' });
  // The declaration of the pref, the list of prefs the socket process follows,
  // the one writer and the comment of its header.
  assert.deepEqual(out.trim().split('\n').sort(), [
    'dom/media/webrtc/transport/nr_stealth_network.cpp',
    'dom/media/webrtc/transport/nr_stealth_network.h',
    'modules/libpref/init/StaticPrefList.yaml',
    'netwerk/base/nsIOService.cpp',
  ]);
  const writers = out.trim().split('\n').filter(f => /SetBool\(\s*kPref|SetBool\(\s*"network\.dns\.disableIPv6"/.test(read(f)));
  assert.deepEqual(writers, ['dom/media/webrtc/transport/nr_stealth_network.cpp']);
});

test('the accessor is the parser\'s own answer, with no second reading of the string', () => {
  const text = read(NETWORK + '.cpp');
  // One parser: the interface list and the IPv6 fact call it, the fact quietly.
  assert.match(text, /static int ParseNetwork\(/);
  assert.match(text, /return ParseNetwork\(aDecl, aIid, aAddrs, aMaxAddrs, aCount, true\);/);
  assert.match(text, /ParseNetwork\(aDecl, kIid, addrs, kMaxDeclared, &count, false\)/);
  // The "v6" key is read in one place.
  assert.equal(text.match(/"v6"/g).length, 1);
  // IPv6 is declared when the parser returns the second (IPv6) entry.
  assert.match(text, /\*aHas = count == 2;/);
  assert.match(read(NETWORK + '.h'), /int nr_stealth_declared_has_ipv6\(const char\* decl, int\* has\);/);
});

test('the application sets a default value, clears the user one, and does nothing without a parsed declaration', () => {
  const text = read(NETWORK + '.cpp');
  const from = text.indexOf('bool StealthApplyDeclaredIPFamily(const char* aDecl) {');
  assert.ok(from > 0);
  const body = text.slice(from, text.indexOf('bool StealthApplyDeclaredIPFamilyFromEnvironment'));
  assert.match(body, /if \(nr_stealth_declared_has_ipv6\(aDecl, &has\)\) \{\s+return false;\s+\}/);
  const early = body.indexOf('return false;');
  const set = body.indexOf('Preferences::SetBool(');
  assert.ok(early > 0 && set > early, 'nothing is set before the early return');
  assert.match(body, /Preferences::SetBool\(kPref, !has, PrefValueKind::Default\);/);
  assert.match(body, /Preferences::ClearUser\(kPref\);/);
  assert.match(body, /kPref\[\] = "network\.dns\.disableIPv6"/);
  assert.match(text, /getenv\("STEALTHFOX_WEBRTC_NETWORK"\)/);
});

test('the hook is one call at the top of nsIOService::Init, tagged; the parent-only rule lives in our file', () => {
  const text = read('netwerk/base/nsIOService.cpp');
  const init = text.slice(text.indexOf('nsresult nsIOService::Init() {'));
  const head = init.slice(0, init.indexOf('SSLTokensCache::Init();'));
  assert.match(head, /^nsresult nsIOService::Init\(\) \{\n  StealthApplyDeclaredNetwork\(\);  \/\/ STEALTH-HOOK\(fingerprint\/webrtc\/declared-network\)\n\n  $/);
  assert.match(text, /#include \"StealthIPv6Literal\.h\"  \/\/ STEALTH-HOOK\(fingerprint\/webrtc\/declared-network\)\n/);
  // Two lines of ours in the Mozilla file, no conditional compilation and no
  // mention of the WebRTC header or of the parent process.
  assert.equal((text.match(/STEALTH-HOOK/g) || []).length, 2);
  assert.equal(text.match(/StealthApplyDeclaredNetwork/g).length, 1);
  assert.doesNotMatch(text, /StealthApplyDeclaredIPFamily|nr_stealth_network/);
  // Before the callbacks that read the pref are registered.
  assert.ok(init.indexOf('RegisterPrefixCallbacks') > head.length);
  // The pref is still followed by the socket process.
  assert.match(text, /"network\.dns\.disableIPv6",/);
  // The function: parent process only, only where WebRTC is built, and it calls
  // the one writer.
  const ours = read('netwerk/base/StealthIPv6Literal.cpp');
  const fn = ours.slice(ours.indexOf('void StealthApplyDeclaredNetwork() {'));
  assert.match(fn, /^void StealthApplyDeclaredNetwork\(\) \{\n#ifdef MOZ_WEBRTC\n  if \(XRE_IsParentProcess\(\)\) \{\n    StealthApplyDeclaredIPFamilyFromEnvironment\(\);\n  \}\n#endif\n\}/);
  assert.match(ours, /#ifdef MOZ_WEBRTC\n#  include \"transport\/nr_stealth_network\.h\"\n#endif/);
  assert.match(read('netwerk/base/StealthIPv6Literal.h'), /void StealthApplyDeclaredNetwork\(\);/);
});

test('the gtest runs one table through the parser, the accessor and the application', () => {
  const text = read('dom/media/webrtc/transport/test/gtest/TestStealthDeclaredNetwork.cpp');
  assert.match(text, /TEST\(StealthDeclaredNetwork, TheIPv6AccessorIsTheParsersOwnAnswer\)/);
  assert.match(text, /TEST\(StealthDeclaredNetwork, NoIPv6InTheDeclarationIsNoIPv6NameLookup\)/);
  assert.match(text, /TEST\(StealthDeclaredNetwork, TheDeclaredIPFamilyIsNotWrittenToTheProfile\)/);
  assert.equal(text.match(/for \(const DeclRow& row : kDecls\)/g).length, 2);
  for (const fragment of ['v6=0;p6=', 'v6=1;p6=2606', 'v6=1;p6="', 'nullptr, false', 'v6=2'])
    assert.ok(text.includes(fragment), fragment);
  // The filter of the candidate runs the suite.
  assert.match(read('.github/workflows/release.yml'), /StealthDeclaredNetwork\.\*/);
});
