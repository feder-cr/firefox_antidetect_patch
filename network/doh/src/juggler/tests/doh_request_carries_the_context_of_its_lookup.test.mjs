// A DNS-over-HTTPS request leaves by the proxy of the browser context whose
// lookup caused it. The context (userContextId of the origin attributes of the
// lookup's record) travels TRR -> the channel's load info -> the proxy lookup
// (PProxyConfigLookup in the socket process case) -> the stand-in channel, and
// Juggler's channel filter answers per userContextId of that channel's load info.
//
// Source guards: the C++ cannot run here. The behaviour is in
// netwerk/test/gtest/TestStealthTRRContext.cpp.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');
const HOOK = 'STEALTH-HOOK(network/doh)';

test('the filter answers per userContextId of the load info of the channel', () => {
  const text = read('juggler/TargetRegistry.js');
  assert.match(text, /const originAttributes = channel\.loadInfo && channel\.loadInfo\.originAttributes;/);
  assert.match(text, /originAttributes \? originAttributes\.userContextId : undefined/);
});

test('the lookup message carries the origin attributes', () => {
  assert.match(read('netwerk/ipc/PSocketProcess.ipdl'),
      /LoadInfoArgs\? aLoadInfo,[^\n]*\n\s+OriginAttributes\? aOriginAttributes\);  \/\/ STEALTH-HOOK\(network\/doh\)/);
});

test('every layer of the lookup passes them on', () => {
  // [file, minimum, pattern]: SocketProcessParent.cpp has older lines about
  // aOriginAttributes (the DNS resolve calls), so its own lines are told apart
  // by the optional type our change adds.
  for (const [file, minimum, pattern] of [
    ['netwerk/ipc/ProxyConfigLookup.h', 3],
    ['netwerk/ipc/ProxyConfigLookup.cpp', 6],
    ['netwerk/ipc/ProxyConfigLookupChild.h', 1],
    ['netwerk/ipc/ProxyConfigLookupChild.cpp', 2],
    ['netwerk/ipc/ProxyConfigLookupParent.h', 2],
    ['netwerk/ipc/ProxyConfigLookupParent.cpp', 3],
    ['netwerk/ipc/SocketProcessParent.h', 2],
    ['netwerk/ipc/SocketProcessParent.cpp', 2, /Maybe<OriginAttributes>/],
  ]) {
    const lines = read(file).split(/\r?\n/).filter(l => (pattern || /aOriginAttributes|mOriginAttributes/).test(l));
    assert.ok(lines.length >= minimum, `${file}: ${lines.length} lines carry the attributes`);
  }
});

test('the stand-in channel of the lookup is given the context before the proxy is asked', () => {
  const text = read('netwerk/ipc/ProxyConfigLookup.cpp');
  // The hook sits between the statement that makes the stand-in and upstream's
  // check of its status, which stays as Mozilla wrote it.
  const apply = text.indexOf('rv = StealthApplyTRRContextToChannel(channel, mOriginAttributes);  // STEALTH-HOOK(network/doh)\n  NS_ENSURE_SUCCESS(rv, rv);  // STEALTH-HOOK(network/doh)');
  const ask = text.indexOf('pps2->AsyncResolve2(channel');
  assert.ok(apply > 0, 'the stand-in does not get the context');
  assert.ok(ask > apply, 'the proxy is asked before the stand-in has its context');
});

test('the TRR request takes the context of its lookup, right after its channel is made', () => {
  const text = read('netwerk/dns/TRR.cpp');
  const made = text.indexOf('rv = DNSUtils::CreateChannelHelper(dnsURI, getter_AddRefs(channel));');
  // The hook sits between the call that makes the channel and upstream's check of it.
  const apply = text.indexOf('rv = StealthApplyTRRContextToChannel(channel, StealthTRRContextOfSuffix(mOriginSuffix));  // STEALTH-HOOK(network/doh)\n  NS_ENSURE_SUCCESS(rv, rv);  // STEALTH-HOOK(network/doh)');
  const open = text.indexOf('rv = httpChannel->AsyncOpen(this);');
  assert.ok(made > 0 && apply > made, 'the context is not applied after the channel is made');
  assert.ok(open > apply, 'the channel is opened before it has its context');
});

test('the cached connection info of the default context never serves another context', () => {
  const text = read('netwerk/dns/TRR.cpp');
  assert.match(text, /  if \(UseDefaultServer\(\) && !useOHTTP &&\n      StaticPrefs::network_trr_async_connInfo\(\) && StealthTRRContextOfSuffix\(mOriginSuffix\)\.isNothing\(\)\) \{  \/\/ STEALTH-HOOK\(network\/doh\)/);
});

test('the TRR channel asks the proxy lookup with the context of its load info', () => {
  const text = read('netwerk/protocol/http/TRRServiceChannel.cpp');
  const resolve = text.slice(text.indexOf('nsresult TRRServiceChannel::ResolveProxy()'));
  assert.match(resolve.slice(0, 1200),
      /getter_AddRefs\(proxyRequest\), Nothing\(\), StealthTRRContextOfLoadInfo\(mLoadInfo\)\);  \/\/ STEALTH-HOOK\(network\/doh\)/);
});

test('a TRR load info keeps the context, and its clone (a redirect) too', () => {
  const text = read('netwerk/base/TRRLoadInfo.cpp');
  const set = text.slice(text.indexOf('nsresult TRRLoadInfo::SetOriginAttributes('));
  // The setter stores the attributes; the line of Mozilla's that refused is gone
  // (a rewritten line, registered: no way to keep it without a dead `return`).
  assert.match(set.slice(0, set.indexOf('\n}\n') + 3),
      /\{\n  mOriginAttributes = aOriginAttributes;  \/\/ STEALTH-HOOK\(network\/doh\)\n  return NS_OK;  \/\/ STEALTH-HOOK\(network\/doh\)\n\}\n/);
  const clone = text.slice(text.indexOf('TRRLoadInfo::Clone()'), text.indexOf('TRRLoadInfo::GetLoadingPrincipal'));
  assert.match(clone, /->mOriginAttributes = mOriginAttributes;  \/\/ STEALTH-HOOK\(network\/doh\)/);
});

test('our file and its gtest are registered', () => {
  assert.match(read('netwerk/ipc/moz.build'), /"StealthTRRContext\.h",  # STEALTH-HOOK\(network\/doh\)/);
  assert.match(read('netwerk/ipc/moz.build'), /"StealthTRRContext\.cpp",  # STEALTH-HOOK\(network\/doh\)/);
  // The gtests of the network line are listed in stealth.build, which Mozilla's moz.build includes with one line.
  assert.match(read('netwerk/test/gtest/moz.build'), /include\("stealth\.build"\)  # STEALTH-HOOK\(network\/socks\)/);
  assert.match(read('netwerk/test/gtest/stealth.build'), /SOURCES \+= \[[^\]]*"TestStealthTRRContext\.cpp",/);
});
