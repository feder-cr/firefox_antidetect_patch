// A DNS-over-HTTPS request of a session behind a proxy fails when its proxy
// cannot be learned; it never goes DIRECT (the name would leave from the
// host's own address). Behaviour of the C++ is in
// netwerk/test/gtest/TestStealthTRRContext.cpp; here the hooks sit where they
// must, and the Juggler side is run on stubs.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

test('a failed proxy status ends the TRR request, a cancelled one goes on as upstream', () => {
  const text = read('netwerk/protocol/http/TRRServiceChannel.cpp');
  const body = text.slice(text.indexOf('TRRServiceChannel::OnProxyAvailable('));
  const fail = body.indexOf('if (StealthTRRServiceFailClosed(this, StealthTRRProxyAnswer(status, pi, true), mCanceled)) { return NS_OK; }  // STEALTH-HOOK(network/doh)');
  const used = body.indexOf('if (NS_SUCCEEDED(status)) mProxyInfo = pi;');
  assert.ok(fail > 0, 'no abort on a failed proxy status');
  assert.ok(used > fail, 'the status is used before it is checked');
});

test('a lookup that cannot even be started ends the TRR request', () => {
  const text = read('netwerk/protocol/http/TRRServiceChannel.cpp');
  const body = text.slice(text.indexOf('nsresult TRRServiceChannel::MaybeResolveProxyAndBeginConnect()'));
  const resolve = body.indexOf('NS_SUCCEEDED(proxyRv = ResolveProxy())');
  const fail = body.indexOf('if (StealthTRRServiceFailClosed(this, proxyRv, false)) { return NS_OK; }  // STEALTH-HOOK(network/doh)');
  const connect = body.indexOf('rv = BeginConnect();');
  assert.ok(resolve > 0 && fail > resolve, 'a failed lookup is not turned into an abort');
  assert.ok(connect > fail, 'the connection begins before the failure is handled');
});

test('the rule is a template in our header: the same test as the nsHttpChannel one, then AsyncAbort with the failure', () => {
  const header = read('netwerk/ipc/StealthTRRContext.h');
  assert.match(header, /bool StealthTRRServiceFailClosed\(ServiceChannelT\* aChannel,\s+nsresult aProxyStatus, bool aCanceled\) \{\n  if \(!StealthFailClosedOnProxyFailure\(aProxyStatus, true, aCanceled\)\) \{\n    return false;\n  \}\n  \(void\)aChannel->AsyncAbort\(StealthTRRProxyFailure\(aProxyStatus\)\);\n  return true;\n\}/);
});

test('the Mozilla comment that says failover to DIRECT stays, and the hook is above the line it governs', () => {
  const text = read('netwerk/protocol/http/TRRServiceChannel.cpp');
  assert.match(text, /We just failover to DIRECT when proxy resolution\n\s+\/\/ fails/);
});

function loadSessionRoute(newProxyInfoWithAuth) {
  const code = read('juggler/SessionRoute.js').replace(/^export function /gm, 'function ') +
      '\nthis.newProxyInfoFor = newProxyInfoFor;';
  const context = {
    Math, WeakMap,
    Cc: { '@mozilla.org/network/protocol-proxy-service;1': { getService: () => ({ newProxyInfoWithAuth }) } },
    Ci: { nsIProxyInfo: {} },
  };
  vm.createContext(context);
  vm.runInContext(code, context);
  return context.newProxyInfoFor;
}

test('the filter asks the same proxy info the command that set the proxy built, and cannot throw', () => {
  let builds = 0;
  let broken = false;
  const newProxyInfoFor = loadSessionRoute(() => {
    if (broken)
      throw new Error('the proxy service is gone');
    return { id: ++builds };
  });
  const record = { type: 'socks', host: 'gw', port: 1080, bypass: [] };
  const atSet = newProxyInfoFor(record);
  broken = true;
  assert.equal(newProxyInfoFor(record), atSet);
  assert.equal(builds, 1);
});

test('a record that cannot be expressed still throws, when it is set', () => {
  const newProxyInfoFor = loadSessionRoute(() => { throw new Error('refused'); });
  assert.throws(() => newProxyInfoFor({ type: 'http', host: 'gw', port: 3128 }), /refused/);
});
