// A TCP socket of a PeerConnection (TURN over TCP) is opened plain (DIRECT) only when its context has no proxy. Upstream's
// WebrtcTCPSocket::OnProxyAvailable opens it plain at the end of the function for every other outcome too: a lookup that failed (a PAC
// thread cancelled or shut down answers a failure status, which skips the proxy filters), a null info with a failed status, or an info of
// a type it does not know. The socket then leaves from the host's own address while the context has a proxy. The UDP twin
// (PcUdpProxy::KindFor) already reads a failed lookup as "proxy not known: nothing goes out".
//
// The hook is one tagged line before that end, calling StealthTcpDirectRefused (own file StealthIceResolverContext.cpp, declared in
// nr_stealth_network.h which the TCP socket already includes). WebrtcTCPSocket.cpp stays at its ceiling of added lines: the two hooks of
// OpenWithoutHttpProxy that sat before the bind (the context of the name lookup, the bind address) became one call, StealthTcpSocketBorn.
// Behaviour of the functions: dom/media/webrtc/transport/test/gtest/TestStealthTcpProxyRoad.cpp.
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
const T = 'dom/media/webrtc/transport/';
const TCP = T + 'ipc/WebrtcTCPSocket.cpp';

const HOOK = '  if (mozilla::StealthTcpDirectRefused(mProxyConfig.isSome(), NS_FAILED(aResult), aProxyinfo)) { CloseWithReason(NS_ERROR_FAILURE); return NS_OK; }  // STEALTH-HOOK(network/proxy-udp)\n';

function onProxyAvailable() {
  const text = read(TCP);
  const start = text.indexOf('NS_IMETHODIMP WebrtcTCPSocket::OnProxyAvailable(');
  assert.ok(start > 0);
  return text.slice(start, text.indexOf('\n}\n', start) + 3);
}

test('the end of OnProxyAvailable refuses a socket whose lookup failed or answered an unknown proxy, before it can open it plain', () => {
  const body = onProxyAvailable();
  assert.ok(body.includes(HOOK), 'no refusal before the plain open');
  const plain = body.indexOf('OpenWithoutHttpProxy(nullptr);');
  assert.ok(plain > 0);
  assert.ok(body.indexOf(HOOK) < plain, 'the refusal comes after the plain open');
  assert.equal(body.match(/OpenWithoutHttpProxy\(nullptr\)/g).length, 1, 'a second plain open that the refusal does not guard');
  // The known types are decided before: the refusal sees only what is left.
  assert.ok(body.indexOf('OpenWithHttpProxy()') < body.indexOf(HOOK));
  assert.ok(body.indexOf('OpenWithoutHttpProxy(aProxyinfo);') < body.indexOf(HOOK));
});

test('the rule is the one of UDP: only "no proxy" (lookup succeeded, no info) opens the socket plain', () => {
  const own = read(T + 'StealthIceResolverContext.cpp');
  const fn = own.match(/bool StealthTcpDirectRefused\([\s\S]*?\n}\n/)[0];
  assert.match(fn, /return aHasProxyConfig && \(aLookupFailed \|\| aInfo != nullptr\);/);
  assert.match(read('netwerk/socket/StealthPcUdpProxy.cpp'), /Kind kind = NS_SUCCEEDED\(aStatus\) \? Kind::Direct : Kind::Blocked;/);
});

test('the function is declared in the header the TCP socket already includes: no new include in its file', () => {
  assert.match(read(T + 'nr_stealth_network.h'), /\nbool StealthTcpDirectRefused\(bool aHasProxyConfig, bool aLookupFailed, nsIProxyInfo\* aInfo\);\n/);
  assert.match(read(T + 'nr_stealth_network.h'), /\nclass nsIProxyInfo;\n/);
  const text = read(TCP);
  assert.match(text, /\n#include "transport\/nr_stealth_network\.h"/);
  assert.doesNotMatch(text, /StealthIceResolverContext\.h/);
  assert.equal(text.match(/\n#include [^\n]*STEALTH-HOOK/g).length, 2, 'a third tagged include');
});

test('the two hooks before the bind are one line: the file keeps its tagged lines and its added lines', () => {
  const text = read(TCP);
  const tagged = text.split('\n').filter(l => /STEALTH-HOOK\(/.test(l));
  assert.ok(tagged.length <= 6, `${tagged.length} tagged lines, the file had 6 before the refusal: the merge is not done`);
  assert.doesNotMatch(text, /StealthTcpNameContext\(/);
  assert.doesNotMatch(text, /StealthTcpBindAddress\(/);
  assert.equal(text.match(/StealthTcpSocketBorn\(/g).length, 1);
  assert.match(read(T + 'nr_stealth_network.h'),
    /\nbool StealthTcpSocketBorn\(nsISocketTransport\* aTransport, const net::WebrtcProxyConfig\* aConfig, const char\* aDeclaredAddress,\n\s+uint16_t aNicerPort, union PRNetAddr\* aOut\);\n/);
  // The ceiling of the series (tools/hook-budget.json): 17 lines added to the release. Skipped where the release tag is not in the checkout.
  let diff = null;
  try {
    diff = execFileSync('git', ['diff', '-U0', 'FIREFOX_159_0_RELEASE', '--', TCP],
      { cwd: TREE, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
  } catch (e) {
    diff = null;
  }
  if (diff) {
    const added = diff.split('\n').filter(l => l.startsWith('+') && !l.startsWith('+++')).length;
    assert.ok(added <= 17, `${added} lines of ours in WebrtcTCPSocket.cpp, the ceiling is 17`);
  }
});

test('the gtest of the rule is built', () => {
  assert.match(read(T + 'test/gtest/moz.build'), /\nSOURCES \+= \["TestStealthTcpProxyRoad\.cpp"\]\n/);
  const test = read(T + 'test/gtest/TestStealthTcpProxyRoad.cpp');
  assert.match(test, /StealthTcpDirectRefused\(true, true, nullptr\)/);
  assert.match(test, /StealthTcpDirectRefused\(true, false, nullptr\)/);
});
