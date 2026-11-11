// No lookup of a desktop session asks for the host's system resolver on its own.
//
// With a session proxy the core declares TRR mode 3 (DoH only, through the
// proxy). A lookup that asks for the request mode "TRR disabled" (the channel
// mode, the load flag, or the DNS flag RESOLVE_TRR_DISABLED_MODE) is answered by
// getaddrinfo / DnsQueryEx of the host, whatever the global mode is. Those
// services stay on as in retail, but their lookups follow the session: DoH
// through the proxy under mode 3, the system resolver only without a proxy
// (mode 0), where dropping the request is a no-op.
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

// ---- the lookups that were asking for the system resolver -------------------

test('NetworkConnectivityService: none of its lookups or its IP check disables TRR', () => {
  const text = read('netwerk/base/NetworkConnectivityService.cpp');
  assert.doesNotMatch(text, TRR_OFF);
  // The other flags of the four lookups are kept.
  const recheck = body(text, 'NetworkConnectivityService::RecheckDNS', 'NetworkConnectivityService::Observe');
  assert.equal((recheck.match(/AsyncResolveNative\(/g) || []).length, 4);
  assert.match(recheck, /RESOLVE_DISABLE_IPV6/);
  assert.match(recheck, /RESOLVE_DISABLE_IPV4/);
  assert.match(recheck, /RESOLVE_TYPE_HTTPSSVC,\s*nsIDNSService::RESOLVE_DEFAULT_FLAGS,/);
  // The HTTP channel of the IP check keeps its address-family limits.
  const ipcheck = body(text, 'NetworkConnectivityService::SetupIPCheckChannel', 'return channel.forget();');
  assert.match(ipcheck, /SetIPv6Disabled\(\)/);
  assert.match(ipcheck, /SetIPv4Disabled\(\)/);
  assert.doesNotMatch(ipcheck, /SetTRRMode/);
});

test('captive portal detection: the XHR follows the session resolver', () => {
  const text = read('toolkit/components/captivedetect/CaptiveDetect.sys.mjs');
  assert.doesNotMatch(text, TRR_OFF);
  assert.doesNotMatch(text, /setTRRMode/);
  assert.match(text, /STEALTH-HOOK\(network\/doh\)/);
  // The flags around it stay.
  assert.match(text, /LOAD_BYPASS_CACHE/);
  assert.match(text, /LOAD_ANONYMOUS/);
  assert.match(text, /HTTPS_ONLY_EXEMPT/);
});

test('captive portal tab: opens with the load flags of any other tab', () => {
  const text = read('browser/components/tabbrowser/Tabbrowser.sys.mjs');
  assert.doesNotMatch(text, TRR_OFF);
  assert.match(text, /STEALTH-HOOK\(network\/doh\)/);
});

test('OCSP channel: asks the mode of a normal channel, mode 3 included', () => {
  const text = read('netwerk/protocol/http/HttpBaseChannel.cpp');
  const getter = body(text, 'HttpBaseChannel::GetTRRMode(', 'HttpBaseChannel::SetTRRMode(');
  assert.doesNotMatch(getter, TRR_OFF);
  assert.doesNotMatch(getter, /LoadIsOCSP/);
  assert.doesNotMatch(getter, /GetCurrentTrrMode/);
  assert.match(getter, /return GetTRRModeImpl\(aTRRMode\);/);
  assert.match(getter, /STEALTH-HOOK\(network\/doh\)/);
});

// ---- premises the "leave" verdicts rest on ---------------------------------

test('premise: under mode 3 the flag RESOLVE_DISABLE_TRR alone never reaches the native lookup', () => {
  const text = read('netwerk/dns/nsHostResolver.cpp');
  const compute = body(text, 'void nsHostResolver::ComputeEffectiveTRRMode', '// Kick-off a name resolve operation');
  // A default request under mode 3 is TRR_ONLY, whatever the flags say.
  assert.match(compute, /requestMode == nsIRequest::TRR_DEFAULT_MODE &&\s*resolverMode == nsIDNSService::MODE_TRRONLY\)[\s\S]*?mEffectiveTRRMode = nsIRequest::TRR_ONLY_MODE;/);
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
  // the global mode 5 is a user setting (about:preferences); the core declares 0 or 3
  'browser/components/preferences/config/privacy.mjs': [3, 'user setting of about:preferences'],
  'browser/components/preferences/privacy.js': [2, 'user setting of about:preferences'],
  'dom/ipc/ContentParent.cpp': [2, 'clamp of the pref value sent to content'],
  'netwerk/dns/TRRServiceBase.cpp': [2, 'clamp of the pref value'],
  'netwerk/ipc/NeckoMessageUtils.h': [1, 'IPC range of the mode'],
  'netwerk/dns/TRRService.cpp': [8, 'mode 5 checks and IsExcludedFromTRR (excluded domains, mode 5)'],
  // derivation of the effective mode: excluded domains (localhost, local, the
  // proxy name), parental control (declared off), mode 0 and 5, and a request
  // that asks for it (which nothing above does any more)
  'netwerk/dns/nsHostResolver.cpp': [21, 'derives the effective mode; native only when disabled'],
  'netwerk/dns/nsHostResolver.h': [2, 'macro that tests the global mode'],
  'netwerk/dns/nsHostRecord.cpp': [3, 'telemetry of the lookup'],
  'netwerk/dns/nsDNSService2.cpp': [4, 'main-thread sync resolve sets the DNS flag (fails closed under mode 3); skip-reason names'],
  // the flag RESOLVE_DISABLE_TRR alone never reaches the native lookup under
  // mode 3 (see the premise test); the retry paths skip TRR_ONLY
  'netwerk/base/nsSocketTransport2.cpp': [4, 'DISABLE_TRR flag set by the retry that skips TRR_ONLY'],
  'netwerk/protocol/http/DnsAndConnectSocket.cpp': [2, 'retry that skips TRR_ONLY'],
  'netwerk/protocol/http/HappyEyeballsConnectionAttempt.cpp': [1, 'retry that skips TRR_ONLY'],
  'netwerk/protocol/http/nsHttpChannel.cpp': [1, 'reads the channel mode, does not set it'],
  'toolkit/components/extensions/parent/ext-dns.js': [1, 'dns.resolve flag an extension asks for: fails closed under mode 3'],
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
