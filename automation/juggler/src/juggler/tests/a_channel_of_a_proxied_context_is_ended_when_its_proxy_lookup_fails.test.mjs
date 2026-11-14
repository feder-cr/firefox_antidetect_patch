// A channel of a context that has a proxy never goes out DIRECT because the lookup of its proxy failed. nsHttpChannel fails over
// to DIRECT when proxy resolution fails ("we just failover to DIRECT", nsHttpChannel::OnProxyAvailable) and when the lookup cannot
// start; only a DoH request was exempt (StealthTRRFailClosed, network/doh). The same two tagged lines now ask
// StealthProxyIsMandatory, which is true for a DoH request (as before) and for a channel whose user context is in the declared
// list of contexts with a proxy (network.trr.stealth.contexts, the list the resolver follows for names). A context without a proxy
// keeps upstream's failover. This covers the CONNECT-only channel of the HTTP-proxy road of a TCP socket of a PeerConnection
// (WebrtcTCPSocket::OpenWithHttpProxy), which does its own second lookup.
//
// No Mozilla line is added: only the third argument of the two existing tagged calls changes. Behaviour of the rule:
// netwerk/test/gtest/TestStealthSessionResolver.cpp.
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
const CHANNEL = 'netwerk/protocol/http/nsHttpChannel.cpp';
const ASK = 'StealthProxyIsMandatory(LoadIsTRRServiceChannel(), mLoadInfo.get())';

test('both fail-closed calls of nsHttpChannel ask whether the proxy is mandatory, not only whether the channel is a DoH one', () => {
  const text = read(CHANNEL);
  const calls = text.split('\n').filter(l => l.includes('StealthTRRFailClosed('));
  assert.equal(calls.length, 2);
  for (const line of calls) {
    assert.ok(line.includes(ASK), `a fail-closed call that ignores the context: ${line.trim()}`);
    assert.match(line, /  \/\/ STEALTH-HOOK\(network\/doh\)$/);
  }
  assert.doesNotMatch(text, /StealthTRRFailClosed\([^\n]*, LoadIsTRRServiceChannel\(\), mCanceled\)/);
});

test('the lookup that cannot start and the failed status are both covered, before the DIRECT road is taken', () => {
  const text = read(CHANNEL);
  const start = text.slice(text.indexOf('void nsHttpChannel::MaybeResolveProxyAndBeginConnect()'));
  assert.ok(start.indexOf('NS_SUCCEEDED(proxyRv = ResolveProxy())') < start.indexOf(ASK));
  assert.ok(start.indexOf(ASK) < start.indexOf('rv = BeginConnect();'));
  const proxy = text.slice(text.indexOf('nsHttpChannel::OnProxyAvailable('));
  assert.ok(proxy.indexOf(ASK) > 0 && proxy.indexOf(ASK) < proxy.indexOf('if (NS_SUCCEEDED(status)) {\n    mProxyInfo = pi;'),
    'the status is used before the rule');
});

test('no Mozilla line is added to nsHttpChannel.cpp: it keeps its five tagged lines', () => {
  const text = read(CHANNEL);
  assert.ok(text.split('\n').filter(l => /STEALTH-HOOK\(/.test(l)).length <= 5);
  assert.equal(text.match(/\n#include [^\n]*STEALTH-HOOK/g).length, 1, 'a new include');
  assert.doesNotMatch(text, /StealthSessionResolver\.h/);
  let diff = null;
  try {
    diff = execFileSync('git', ['diff', '-U0', 'FIREFOX_159_0_RELEASE', '--', CHANNEL],
      { cwd: TREE, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
  } catch (e) {
    diff = null;
  }
  if (diff) {
    const added = diff.split('\n').filter(l => l.startsWith('+') && !l.startsWith('+++')).length;
    assert.ok(added <= 5, `${added} lines of ours in nsHttpChannel.cpp, the ceiling is 5`);
  }
});

test('the rule is declared in the header the channel already includes and defined next to the parser of the list', () => {
  const header = read('netwerk/ipc/StealthTRRContext.h');
  assert.match(header, /bool StealthProxyIsMandatory\(bool aIsTRRServiceChannel, uint32_t aUserContextId,\s+const nsACString& aContextsWithProxy\);/);
  assert.match(header, /bool StealthProxyIsMandatory\(bool aIsTRRServiceChannel, nsILoadInfo\* aLoadInfo\);/);
  const code = read('netwerk/dns/StealthSessionResolver.cpp');
  assert.match(code, /bool StealthProxyIsMandatory\(bool aIsTRRServiceChannel, uint32_t aUserContextId,\s+const nsACString& aContextsWithProxy\) \{\n  return aIsTRRServiceChannel \|\|\n\s+StealthContextHasProxy\(aUserContextId, aContextsWithProxy\);\n\}/);
  const fn = code.match(/bool StealthProxyIsMandatory\(bool aIsTRRServiceChannel, nsILoadInfo\* aLoadInfo\) \{[\s\S]*?\n}\n/)[0];
  assert.match(fn, /StaticPrefs::network_trr_stealth_contexts\(\)/);
  assert.match(fn, /aLoadInfo \? aLoadInfo->GetOriginAttributes\(\)\.mUserContextId : 0/);
  assert.match(code, /#include "mozilla\/net\/StealthTRRContext\.h"/);
});

test('the rule is true for a DoH channel whatever the list says, so what was covered stays covered', () => {
  const code = read('netwerk/dns/StealthSessionResolver.cpp');
  const fn = code.match(/bool StealthProxyIsMandatory\(bool aIsTRRServiceChannel, nsILoadInfo\* aLoadInfo\) \{[\s\S]*?\n}\n/)[0];
  assert.match(fn, /if \(aIsTRRServiceChannel\) \{\n    return true;\n  \}/);
  const test = read('netwerk/test/gtest/TestStealthSessionResolver.cpp');
  assert.match(test, /StealthProxyIsMandatory\(true, 0, ""_ns\)/);
  assert.match(test, /StealthProxyIsMandatory\(false, 7, "6"_ns\)\)/);
});
