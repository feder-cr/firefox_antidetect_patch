// The host resolver chooses DoH or the native resolver PER LOOKUP, by the user
// context of the lookup, and never by the global mode.
//
// The service stays in mode 0 (native only). Every lookup enters the host
// resolver through the hook StealthFlagsFollowSession (netwerk/dns/
// StealthSessionResolver.cpp), which sets the TRR mode bits of its flags:
//  - the context of the lookup has a proxy: TRR_ONLY (DoH only, fail closed);
//  - it has none: TRR disabled (the native resolver, as retail).
// "Has a proxy" is read from the transport pref network.trr.stealth.contexts,
// which Juggler writes: "*" (the browser-wide proxy: every context without its
// own is proxied) and/or decimal user context ids. A global mode of 3 would
// couple the contexts: a context without a proxy would resolve through DoH.
//
// This file pins: the two call sites pass the attributes of the lookup, the
// pref is declared ONCE and read thread-safely, the rule has no dependence on
// network.trr.mode, and the exemption of the DoH server's own name.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

const PREF = 'network.trr.stealth.contexts';

function body(text, startMarker, endMarker) {
  const start = text.indexOf(startMarker);
  assert.ok(start >= 0, startMarker);
  const end = text.indexOf(endMarker, start + startMarker.length);
  assert.ok(end > start, endMarker);
  return text.slice(start, end);
}

// The code of a C++ file without its comments, so that the words of a comment
// never satisfy or break an assertion about the code.
const withoutComments = text => text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

test('both call sites pass the attributes of the lookup', () => {
  const text = read('netwerk/dns/nsHostResolver.cpp');
  const resolve = body(text, 'nsresult nsHostResolver::ResolveHost(', 'nsAutoCString host(aHost);');
  assert.match(resolve, /nsResolveHostCallback\* aCallback\) \{\n  \/\/ STEALTH-HOOK\(network\/doh\): [^\n]*\n  flags = StealthFlagsFollowSession\(aHost, aOriginAttributes, flags\);\n  $/);
  const detach = body(text, 'void nsHostResolver::DetachCallback(', 'RefPtr<nsHostRecord> rec;');
  assert.match(detach, /nsresult status\) \{\n  flags = StealthFlagsFollowSession\(host, aOriginAttributes,\n\s+flags\);  \/\/ STEALTH-HOOK\(network\/doh\)\n  $/);
  // The parameters those names refer to are the attributes of the lookup.
  assert.match(resolve, /const OriginAttributes& aOriginAttributes,/);
  assert.match(detach, /const OriginAttributes& aOriginAttributes,/);
  assert.equal((text.match(/StealthFlagsFollowSession\(/g) || []).length, 2);
});

test('a lookup and its cancellation resolve with the same flags', () => {
  // nsDNSAsyncRequest keeps the flags and the attributes it was created with
  // and hands both back to DetachCallback; the function is a pure function of
  // (host, attributes, flags, pref), and applying it twice changes nothing.
  const dns = read('netwerk/dns/nsDNSService2.cpp');
  assert.match(dns, /mResolver->DetachCallback\(mHost, mTrrServer, mType, mOriginAttributes, mFlags,/);
  assert.match(dns, /new nsDNSAsyncRequest\(res, hostname, DNSAdditionalInfo::URL\(aInfo\), type,\s*aOriginAttributes, listener, flags, af\)/);
});

test(`${PREF} is declared once, as a thread-safe static string`, () => {
  const yaml = read('modules/libpref/init/StealthPrefList.yaml');
  assert.match(yaml, new RegExp(`- name: ${PREF.replace(/\./g, '\\.')}\\n  type: DataMutexString\\n  value: ""\\n  mirror: always\\n`));
  for (const file of [
    'modules/libpref/init/StealthPrefs.js',
    'modules/libpref/init/StaticPrefList.yaml',
    'modules/libpref/init/all.js',
    'browser/app/profile/firefox.js',
  ]) {
    assert.ok(!read(file).includes(PREF), `${file} declares ${PREF} a second time`);
  }
  assert.equal(yaml.split(`name: ${PREF}`).length - 1, 1);
});

test('the pref is read through the static pref API, on any thread', () => {
  const cpp = read('netwerk/dns/StealthSessionResolver.cpp');
  assert.match(cpp, /#include "mozilla\/StaticPrefs_network\.h"/);
  assert.match(cpp, /StaticPrefs::network_trr_stealth_contexts\(\)/);
  // Not through the Preferences service, which is main-thread only.
  assert.doesNotMatch(cpp, /Preferences::Get|nsIPrefBranch|Services\.prefs/);
});

test('the rule does not depend on the global mode', () => {
  const code = withoutComments(read('netwerk/dns/StealthSessionResolver.cpp'));
  assert.doesNotMatch(code, /Mode\(\)/);
  assert.doesNotMatch(code, /MODE_TRR|MODE_NATIVEONLY|network_trr_mode|ResolverMode/);
  const header = withoutComments(read('netwerk/dns/StealthSessionResolver.h'));
  assert.doesNotMatch(header, /ResolverMode|MODE_TRR/);
});

test('the pure functions: list parsing, flags, and the signature of the hook', () => {
  const h = read('netwerk/dns/StealthSessionResolver.h');
  assert.match(h, /bool StealthContextHasProxy\(uint32_t aUserContextId,\s*const nsACString& aList\);/);
  assert.match(h, /nsIDNSService::DNSFlags StealthResolverFlags\(nsIDNSService::DNSFlags aFlags,\s*bool aContextHasProxy,\s*bool aHostIsTRRServer\);/);
  assert.match(h, /nsIDNSService::DNSFlags StealthFlagsFollowSession\(\s*const nsACString& aHost, const OriginAttributes& aAttrs,\s*nsIDNSService::DNSFlags aFlags\);/);
  const cpp = read('netwerk/dns/StealthSessionResolver.cpp');
  const flags = body(cpp, 'nsIDNSService::DNSFlags StealthResolverFlags(', '// The host name of the DoH server');
  // The DoH server's own request keeps the flags it asked for.
  assert.match(flags, /if \(aHostIsTRRServer\) \{\s*return aFlags;/);
  // Only the two mode bits are replaced: DoH only with a proxy, native without.
  assert.match(flags, /aFlags & ~nsIDNSService::RESOLVE_TRR_MODE_MASK/);
  assert.match(flags, /GetFlagsFromTRRMode\(nsIRequest::TRR_ONLY_MODE\)/);
  assert.match(flags, /nsIDNSService::RESOLVE_TRR_DISABLED_MODE/);
  assert.doesNotMatch(flags, /RESOLVE_DISABLE_TRR/);
  // The context id comes from the attributes of the lookup.
  const hook = body(cpp, 'nsIDNSService::DNSFlags StealthFlagsFollowSession(', '}  // namespace mozilla::net');
  assert.match(hook, /aAttrs\.mUserContextId/);
});

test('the gtest covers the list, the contexts, the flags and the DoH server', () => {
  const gtest = read('netwerk/test/gtest/TestStealthSessionResolver.cpp');
  for (const name of ['ListParsing', 'ContextAgainstList', 'ProxiedLookupIsDoHOnly',
    'UnproxiedLookupIsNative', 'OtherFlagsStay', 'TheDoHServerKeepsItsRequest'])
    assert.match(gtest, new RegExp(`TEST\\(TestStealthSessionResolver,\\s*${name}\\)`));
  assert.doesNotMatch(gtest, /MODE_TRRONLY|ResolverMode|NeedsHost/);
});
