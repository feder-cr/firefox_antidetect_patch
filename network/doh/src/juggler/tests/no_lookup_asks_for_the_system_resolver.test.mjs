// No lookup of a desktop session asks for the host's system resolver on its own.
//
// The global mode stays 0; a lookup of a context with a proxy is DoH only
// (request mode TRR_ONLY), a lookup of a context without one is native. A lookup
// that asks for the request mode "TRR disabled" (the channel mode, the load flag,
// or the DNS flag RESOLVE_TRR_DISABLED_MODE) is answered by getaddrinfo /
// DnsQueryEx of the host. Those services stay on as in retail, but their lookups
// follow the context: DoH through its proxy when it has one, the system resolver
// when it has none.
//
// That is done ONCE, in our file netwerk/dns/StealthSessionResolver.cpp, called
// from the two places where the host resolver builds the key of a record from
// the caller's flags (the rule itself is pinned in
// the_resolver_chooses_by_context.test.mjs). The Mozilla files that ask for the
// request mode (the connectivity service, the captive portal XHR and tab, the
// OCSP channel) are as Mozilla wrote them. The first half pins that, the hook,
// and the one request that must keep its mode (the DoH request itself).
//
// The second half is an allowlist: every remaining occurrence of the words that
// disable TRR in non-test source has a verdict. A new one fails the test and
// forces a review.
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
const TRR_OFF = /TRR_DISABLED|DISABLE_TRR|MODE_TRROFF|TRR_OFF_MODE/;

function body(text, startMarker, endMarker) {
  const start = text.indexOf(startMarker);
  assert.ok(start >= 0, startMarker);
  const end = text.indexOf(endMarker, start + startMarker.length);
  assert.ok(end > start, endMarker);
  return text.slice(start, end);
}

// ---- the one place that turns "TRR disabled" into the session's mode ---------

// The request mode "TRR disabled" is resolved ONCE, where every lookup enters
// the host resolver. The services that ask for it (the four lookups and the IP
// check channel of NetworkConnectivityService, the captive portal XHR and its
// tab, the OCSP channels) are Mozilla's files as Mozilla wrote them.
const SERVICES = {
  'netwerk/base/NetworkConnectivityService.cpp': [
    // the four lookups and the IP check channel, as upstream
    /nsIDNSService::RESOLVE_DISABLE_IPV6 \|\s*nsIDNSService::RESOLVE_TRR_DISABLED_MODE,/,
    /nsIDNSService::RESOLVE_DISABLE_IPV4 \|\s*nsIDNSService::RESOLVE_TRR_DISABLED_MODE,/,
    /nsIDNSService::RESOLVE_TYPE_HTTPSSVC,\s*nsIDNSService::RESOLVE_TRR_DISABLED_MODE,/,
    /(RESOLVE_TRR_DISABLED_MODE[\s\S]*){4}/,
    /channel->SetTRRMode\(nsIRequest::TRR_DISABLED_MODE\);/,
  ],
  'toolkit/components/captivedetect/CaptiveDetect.sys.mjs': [
    /xhr\.channel\.setTRRMode\(Ci\.nsIRequest\.TRR_DISABLED_MODE\);/,
  ],
  'browser/components/tabbrowser/Tabbrowser.sys.mjs': [
    /LOAD_FLAGS_DISABLE_TRR,\n\} = Ci\.nsIWebNavigation;/,
    /if \(isCaptivePortalTab\) \{\n\s+loadFlags \|= LOAD_FLAGS_DISABLE_TRR;\n\s+\}/,
  ],
  'netwerk/protocol/http/HttpBaseChannel.cpp': [
    // the OCSP special case, whole
    /if \(!LoadIsOCSP\(\)\) \{\n\s+return GetTRRModeImpl\(aTRRMode\);\n\s+\}[\s\S]*?trrMode == nsIDNSService::MODE_TRRONLY\) \{\n\s+\*aTRRMode = nsIRequest::TRR_DISABLED_MODE;\n\s+return NS_OK;\n\s+\}\n\n\s+return GetTRRModeImpl\(aTRRMode\);/,
  ],
};

for (const [file, upstream] of Object.entries(SERVICES)) {
  test(`${file}: as Mozilla wrote it, with no hook of ours for the resolver`, () => {
    const text = read(file);
    for (const re of upstream) assert.match(text, re);
    assert.doesNotMatch(text, /STEALTH-HOOK\(network\/doh\)/);
    // Where the tag of the release exists, the file carries no line of this
    // feature: nothing of the TRR words was added or taken out of it.
    let diff = null;
    try {
      diff = execFileSync('git', ['diff', '-U0', 'FIREFOX_159_0_RELEASE', '--', file],
        { cwd: TREE, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
    } catch (e) { /* no tag in this checkout */ }
    if (diff !== null) assert.doesNotMatch(diff, /^[-+].*(TRR|Trr|trr)/m);
  });
}

test('the resolver hook: two calls at the top of the functions that build the record key from the flags', () => {
  const text = read('netwerk/dns/nsHostResolver.cpp');
  // 3 lines of ours: the include and the two calls.
  assert.equal((text.match(/STEALTH-HOOK\(network\/doh\)/g) || []).length, 3);
  assert.match(text, /#include "PLDHashTable\.h"\n#include "StealthSessionResolver\.h"  \/\/ STEALTH-HOOK\(network\/doh\)\n#include "TRR\.h"\n/);
  const resolve = body(text, 'nsresult nsHostResolver::ResolveHost(', 'nsAutoCString host(aHost);');
  assert.match(resolve, /nsResolveHostCallback\* aCallback\) \{\n  \/\/ STEALTH-HOOK\(network\/doh\): [^\n]*\n  flags = StealthFlagsFollowSession\(aHost, aOriginAttributes, flags\);\n  $/);
  const detach = body(text, 'void nsHostResolver::DetachCallback(', 'RefPtr<nsHostRecord> rec;');
  assert.match(detach, /nsresult status\) \{\n  flags = StealthFlagsFollowSession\(host, aOriginAttributes,\n\s+flags\);  \/\/ STEALTH-HOOK\(network\/doh\)\n  $/);
  // The Mozilla function that computes the effective mode is untouched.
  const compute = body(text, 'void nsHostResolver::ComputeEffectiveTRRMode', '// Kick-off a name resolve operation');
  assert.doesNotMatch(compute, /Stealth/);
  assert.match(compute, /if \(requestMode == nsIRequest::TRR_DISABLED_MODE\) \{\n\s+aRec->RecordReason\(TRRSkippedReason::TRR_REQ_MODE_DISABLED\);/);
});

test('the resolver hook: only the two mode bits, per context, never the name of the DoH server', () => {
  const cpp = read('netwerk/dns/StealthSessionResolver.cpp');
  const h = read('netwerk/dns/StealthSessionResolver.h');
  // Pure core: the other flags (RESOLVE_DISABLE_TRR included) stay.
  assert.match(cpp, /\(aFlags & ~nsIDNSService::RESOLVE_TRR_MODE_MASK\) \| mode/);
  assert.match(cpp, /if \(aHostIsTRRServer\) \{\s*return aFlags;/);
  assert.doesNotMatch(cpp, /RESOLVE_DISABLE_TRR/);
  // The DoH server's name is read from the TRR service only for a request for
  // "TRR disabled", the one the DoH request itself makes, and compared
  // case-insensitively with the host of its URI.
  const wrapper = body(cpp, 'nsIDNSService::DNSFlags StealthFlagsFollowSession(', '}  // namespace mozilla::net');
  assert.match(wrapper, /TRRService::Get\(\)/);
  assert.match(wrapper, /\(aFlags & nsIDNSService::RESOLVE_TRR_MODE_MASK\) ==\s*nsIDNSService::RESOLVE_TRR_DISABLED_MODE &&\s*HostIsTRRServer\(service, aHost\)/);
  assert.match(cpp, /GetAsciiHost\(serverHost\)/);
  assert.match(cpp, /nsCaseInsensitiveCStringComparator/);
  assert.match(h, /StealthFlagsFollowSession\(\s*const nsACString& aHost, const OriginAttributes& aAttrs,\s*nsIDNSService::DNSFlags aFlags\)/);
  // Built in the DNS library, and with its gtest.
  assert.match(read('netwerk/dns/moz.build'), /\]\nUNIFIED_SOURCES \+= \["StealthSessionResolver\.cpp"\]  # STEALTH-HOOK\(network\/doh\)\n/);
  assert.match(read('netwerk/test/gtest/moz.build'), /include\("stealth\.build"\)  # STEALTH-HOOK\(network\/socks\)/);
  assert.match(read('netwerk/test/gtest/stealth.build'), /SOURCES \+= \[[^\]]*"TestStealthSessionResolver\.cpp",/);
  const gtest = read('netwerk/test/gtest/TestStealthSessionResolver.cpp');
  for (const name of ['ListParsing', 'ContextAgainstList', 'ProxiedLookupIsDoHOnly', 'UnproxiedLookupIsNative',
    'OtherFlagsStay', 'TheResultIsStableUnderASecondCall', 'TheDoHServerKeepsItsRequest'])
    assert.match(gtest, new RegExp(`TEST\\(TestStealthSessionResolver,\\s*${name}\\)`));
});

test('the DoH request is the one "TRR disabled" request that must stay one', () => {
  // TRR.cpp asks for it on purpose (resolving the DoH server through DoH would
  // wait for itself); its server's name is the only name the hook leaves alone,
  // the proxy's names being excluded by the pref the engine derives.
  const trr = read('netwerk/dns/TRR.cpp');
  assert.match(trr, /httpChannel->SetTRRMode\(nsIRequest::TRR_DISABLED_MODE\)/);
  const dns = read('netwerk/dns/nsDNSService2.cpp');
  // With a bootstrap address the name never reaches the resolver at all.
  assert.match(dns, /mTrrService->MaybeBootstrap\(aInput, aACE\)/);
});

test('the records and the callbacks agree: a lookup and its cancellation build the same key', () => {
  // nsDNSAsyncRequest keeps the flags it was given and hands them to
  // DetachCallback; both entries of the resolver apply the same function.
  const dns = read('netwerk/dns/nsDNSService2.cpp');
  assert.match(dns, /mResolver->DetachCallback\(mHost, mTrrServer, mType, mOriginAttributes, mFlags,/);
  const text = read('netwerk/dns/nsHostResolver.cpp');
  assert.equal((text.match(/StealthFlagsFollowSession\(/g) || []).length, 2);
});

// ---- premises the "leave" verdicts rest on ---------------------------------

test('premise: for a proxied lookup the flag RESOLVE_DISABLE_TRR alone never reaches the native lookup', () => {
  const text = read('netwerk/dns/nsHostResolver.cpp');
  const compute = body(text, 'void nsHostResolver::ComputeEffectiveTRRMode', '// Kick-off a name resolve operation');
  // A request that asks for TRR_ONLY (what the hook sets for a proxied
  // context, with the global mode at 0) keeps that effective mode, whatever the
  // other flags say: the last statement of the function.
  assert.match(compute, /\n  aRec->mEffectiveTRRMode = requestMode;\n\}\n+$/);
  // The names the TRR service excludes (the proxies' own, localhost, local) are
  // the one way out of it: they fall to the native lookup, checked with the
  // request mode, which for TRR_ONLY applies the excluded list.
  assert.match(compute, /IsExcludedFromTRR\(aRec->host, requestMode\)\) \{\n\s+aRec->RecordReason\(TRRSkippedReason::TRR_EXCLUDED\);\n\s+aRec->mEffectiveTRRMode = nsIRequest::TRR_DISABLED_MODE;/);
  assert.match(read('netwerk/dns/TRRService.cpp'),
    /const bool trrOnly = aRequestMode == nsIRequest::TRR_ONLY_MODE \|\|/);
  // The native lookup runs only for the effective mode "disabled", or for
  // TRR-first when TRR was skipped or failed.
  const lookup = body(text, 'nsresult nsHostResolver::NameLookup', 'nsresult nsHostResolver::ConditionallyRefreshRecord');
  assert.match(lookup,
    /if \(rec->mEffectiveTRRMode == nsIRequest::TRR_DISABLED_MODE \|\|\s*\(rec->mEffectiveTRRMode == nsIRequest::TRR_FIRST_MODE &&/);
  assert.equal((lookup.match(/NativeLookup\(rec\)/g) || []).length, 1);
});

test('premise: a failed connection retries without TRR only outside TRR_ONLY', () => {
  assert.match(read('netwerk/protocol/http/DnsAndConnectSocket.cpp'),
    /if \(trrMode != nsIRequest::TRR_ONLY_MODE\) \{/);
  assert.match(read('netwerk/base/nsSocketTransport2.cpp'),
    /if \(trrMode != nsIRequest::TRR_ONLY_MODE\) \{/);
  assert.match(read('netwerk/protocol/http/HappyEyeballsConnectionAttempt.cpp'),
    /mDnsMetadata\.mEffectiveTRRMode == nsIRequest::TRR_ONLY_MODE/);
});

test('premise: the DoH rollout heuristics do not run once network.trr.mode has a user value', () => {
  const text = read('toolkit/components/doh/DoHController.sys.mjs');
  const gate = text.indexOf('Services.prefs.prefHasUserValue(NETWORK_TRR_MODE_PREF)');
  const state = text.indexOf('await this.setState("manuallyDisabled");', gate);
  const run = text.indexOf('this.runHeuristicsThrottled("startup");', gate);
  assert.ok(gate > 0 && state > gate && run > state);
});

test('premise: TRR over OHTTP, the only user of the OHTTP config fetch, is off', () => {
  const yaml = read('modules/libpref/init/StaticPrefList.yaml');
  assert.match(yaml, /- name: network\.trr\.use_ohttp\n  type: RelaxedAtomicBool\n  value: false\n/);
});

// ---- the allowlist -------------------------------------------------------------

// file -> [number of occurrences, verdict]. A comment that names one counts.
const ALLOWED = {
  // the global mode 5 is a user setting (about:preferences); the core leaves it at 0
  'browser/components/preferences/config/privacy.mjs': [3, 'user setting of about:preferences'],
  'browser/components/preferences/privacy.js': [2, 'user setting of about:preferences'],
  'dom/ipc/ContentParent.cpp': [2, 'clamp of the pref value sent to content'],
  'netwerk/dns/TRRServiceBase.cpp': [2, 'clamp of the pref value'],
  'netwerk/ipc/NeckoMessageUtils.h': [1, 'IPC range of the mode'],
  'netwerk/dns/TRRService.cpp': [8, 'mode 5 checks and IsExcludedFromTRR (excluded domains, mode 5)'],
  // derivation of the effective mode: excluded domains (localhost, local, the
  // proxy name), parental control (declared off), mode 0 and 5, and a request
  // that asks for it (turned into TRR_ONLY or native by the hook below, by the
  // context of the lookup, for every name but the DoH server's)
  'netwerk/dns/nsHostResolver.cpp': [21, 'derives the effective mode; native only when disabled'],
  'netwerk/dns/StealthSessionResolver.cpp': [3, 'the hook: the TRR mode bits of a lookup become DoH only (a context with a proxy) or "TRR disabled" (none)'],
  'netwerk/dns/StealthSessionResolver.h': [1, 'the comment of that hook'],
  // asked for as upstream does, answered by the hook above
  'netwerk/base/NetworkConnectivityService.cpp': [5, 'four lookups and the IP check channel; follow the session through the hook'],
  'toolkit/components/captivedetect/CaptiveDetect.sys.mjs': [1, 'captive portal XHR; follows the session through the hook'],
  'browser/components/tabbrowser/Tabbrowser.sys.mjs': [2, 'captive portal tab flag; follows the session through the hook'],
  'netwerk/protocol/http/HttpBaseChannel.cpp': [2, 'OCSP channels; follow the session through the hook'],
  'netwerk/dns/nsHostResolver.h': [2, 'macro that tests the global mode'],
  'netwerk/dns/nsHostRecord.cpp': [3, 'telemetry of the lookup'],
  'netwerk/dns/nsDNSService2.cpp': [4, 'main-thread sync resolve sets the DNS flag (fails closed for a proxied lookup); skip-reason names'],
  // the flag RESOLVE_DISABLE_TRR alone never reaches the native lookup for
  // a proxied lookup (see the premise test); the retry paths skip TRR_ONLY
  'netwerk/base/nsSocketTransport2.cpp': [4, 'DISABLE_TRR flag set by the retry that skips TRR_ONLY'],
  'netwerk/protocol/http/DnsAndConnectSocket.cpp': [2, 'retry that skips TRR_ONLY'],
  'netwerk/protocol/http/HappyEyeballsConnectionAttempt.cpp': [1, 'retry that skips TRR_ONLY'],
  'netwerk/protocol/http/nsHttpChannel.cpp': [1, 'reads the channel mode, does not set it'],
  'toolkit/components/extensions/parent/ext-dns.js': [1, 'dns.resolve flag an extension asks for: fails closed for a proxied lookup'],
  // DoH itself
  'netwerk/dns/TRR.cpp': [1, 'the DoH request is not itself resolved through TRR; its proxy is the session proxy'],
  'netwerk/protocol/http/ObliviousHttpService.cpp': [1, 'TRR over OHTTP only: network.trr.use_ohttp is false'],
  // PAC only: network.proxy.type 2 or a PAC from the system settings
  'netwerk/base/ProxyAutoConfig.cpp': [1, 'dnsResolve() of a PAC script'],
  'netwerk/base/nsPACMan.cpp': [1, 'load of the PAC file'],
  // heuristics of the DoH rollout: skipped once network.trr.mode has a user value
  'toolkit/components/doh/DoHHeuristics.sys.mjs': [1, 'canary lookups of the DoH rollout heuristics'],
  // user-driven: the flag LOAD_FLAGS_DISABLE_TRR is set by no caller now
  'docshell/base/BrowsingContext.cpp': [2, 'handles LOAD_FLAGS_DISABLE_TRR; no caller sets it'],
  'docshell/base/nsDocShell.cpp': [2, 'handles LOAD_FLAGS_DISABLE_TRR; no caller sets it'],
  'toolkit/actors/NetErrorChild.sys.mjs': [1, 'RPM API of about:neterror, no caller in the tree, not reachable from a web page'],
};

function occurrences() {
  const out = execFileSync('git', [
    'grep', '-n', '-I', '-E', TRR_OFF.source, '--', '.',
    ':!*.idl', ':!*.ipdl', ':!*.md', ':!*.rs', ':!tools/@types', ':!third_party', ':!testing', ':!juggler/tests',
  ], { cwd: TREE, encoding: 'utf8', maxBuffer: 1 << 26 });
  const isTest = f => /(^|\/)(test|tests|gtest|xpcshell|browser-captivePortal)(\/|$)|(^|\/)(test_|browser_|head_)[^/]*$|Test[A-Za-z]*\.cpp$/.test(f);
  const counts = new Map();
  for (const line of out.split('\n')) {
    if (!line) continue;
    const file = line.slice(0, line.indexOf(':'));
    if (isTest(file)) continue;
    counts.set(file, (counts.get(file) || 0) + 1);
  }
  return counts;
}

test('allowlist: the files that still name a TRR-disabling request are exactly the reviewed ones', () => {
  const found = occurrences();
  const expected = new Map(Object.entries(ALLOWED).map(([f, [n]]) => [f, n]));
  const unreviewed = [...found].filter(([f, n]) => expected.get(f) !== n).map(([f, n]) => `${f}: ${n} (allowlist ${expected.get(f) ?? 'none'})`);
  const stale = [...expected].filter(([f]) => !found.has(f)).map(([f]) => `${f}: allowlisted but no longer present`);
  assert.deepEqual([...unreviewed, ...stale], [],
    'a new occurrence of a TRR-disabling request must be reviewed (does it run in a desktop session while a proxy is active?) and added to ALLOWED with its verdict');
});

test('allowlist: every entry has a verdict', () => {
  for (const [file, [n, why]] of Object.entries(ALLOWED)) {
    assert.ok(Number.isInteger(n) && n > 0, file);
    assert.ok(why && why.length > 10, file);
  }
});
