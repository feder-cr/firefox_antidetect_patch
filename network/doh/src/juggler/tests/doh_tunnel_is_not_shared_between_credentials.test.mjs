// The DoH CONNECT tunnel is kept alive and reused by its connection hash key.
// For an http/https proxy that key has the proxy's type, host and port but not
// the Proxy-Authorization header (ProxyUsername/ProxyPassword stay empty: necko
// refuses credentials in the endpoint). Two browser contexts with the SAME
// proxy endpoint and DIFFERENT credentials would therefore share a tunnel, and
// one context's names would leave under another's credentials. So the proxy
// info carries a connection isolation key made of a digest of the credentials
// (never the credentials), and the TRR channel's connection info carries the
// origin attributes of the lookup's context.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const source = fs.readFileSync(path.join(TREE, 'juggler/SessionRoute.js'), 'utf8');

function load(calls) {
  const code = source.replace(/^export function /gm, 'function ') +
      '\nthis.newProxyInfoFor = newProxyInfoFor;';
  const context = {
    Math, TextEncoder, btoa, String, Uint8Array, Array,
    Ci: { nsICryptoHash: { SHA256: 3 } },
    Cc: {
      '@mozilla.org/network/protocol-proxy-service;1': { getService: () => ({
        newProxyInfoWithAuth: (...args) => { calls.push(args); return { args }; },
      }) },
      // nsICryptoHash: update(bytes, length), finish(false) -> binary string.
      '@mozilla.org/security/hash;1': { createInstance: () => {
        const h = crypto.createHash('sha256');
        return {
          init() {},
          update(bytes, length) { h.update(Buffer.from(bytes.slice(0, length))); },
          finish() { return h.digest('latin1'); },
        };
      } },
    },
  };
  vm.createContext(context);
  vm.runInContext(code, context);
  return context.newProxyInfoFor;
}

const ISOLATION_KEY = 6; // aConnectionIsolationKey in newProxyInfoWithAuth
const sha256 = s => crypto.createHash('sha256').update(s, 'utf8').digest('hex');

test('same endpoint, different credentials: different isolation keys', () => {
  const calls = [];
  const f = load(calls);
  f({ type: 'http', host: 'gw', port: 3128, username: 'a', password: 'x' });
  f({ type: 'http', host: 'gw', port: 3128, username: 'b', password: 'x' });
  f({ type: 'http', host: 'gw', port: 3128, username: 'a', password: 'y' });
  const keys = calls.map(c => c[ISOLATION_KEY]);
  assert.equal(new Set(keys).size, 3);
  for (const k of keys)
    assert.match(k, /^[0-9a-f]{64}$/);
});

test('the key is the SHA-256 of user NUL pass, so "a"+"bc" is not "ab"+"c"', () => {
  const calls = [];
  const f = load(calls);
  f({ type: 'https', host: 'gw', port: 443, username: 'a', password: 'bc' });
  f({ type: 'https', host: 'gw', port: 443, username: 'ab', password: 'c' });
  assert.equal(calls[0][ISOLATION_KEY], sha256('a\0bc'));
  assert.equal(calls[1][ISOLATION_KEY], sha256('ab\0c'));
  assert.notEqual(calls[0][ISOLATION_KEY], calls[1][ISOLATION_KEY]);
});

test('same credentials, same key (records of two contexts share a tunnel only then)', () => {
  const calls = [];
  const f = load(calls);
  f({ type: 'http', host: 'gw', port: 3128, username: 'u', password: 'p' });
  f({ type: 'http', host: 'gw', port: 3128, username: 'u', password: 'p' });
  assert.equal(calls[0][ISOLATION_KEY], calls[1][ISOLATION_KEY]);
});

test('no credentials: empty key', () => {
  const calls = [];
  const f = load(calls);
  f({ type: 'http', host: 'gw', port: 3128 });
  f({ type: 'https', host: 'gw', port: 443, username: '', password: '' });
  assert.equal(calls[0][ISOLATION_KEY], '');
  assert.equal(calls[1][ISOLATION_KEY], '');
});

test('SOCKS is unaffected: its credentials are in the endpoint and in the hash key already', () => {
  const calls = [];
  const f = load(calls);
  f({ type: 'socks', host: 'gw', port: 1080, username: 'u', password: 'p' });
  f({ type: 'socks4', host: 'gw', port: 1080, username: 'u', password: 'p' });
  for (const c of calls)
    assert.equal(c[ISOLATION_KEY], '');
});

test('the key never contains the credentials', () => {
  const calls = [];
  load(calls)({ type: 'http', host: 'gw', port: 1, username: 'abcdef', password: 'f00dcafe' });
  const key = calls[0][ISOLATION_KEY];
  assert.ok(!key.includes('abcdef') && !key.includes('f00dcafe'));
  assert.ok(!JSON.stringify(calls[0][3]).includes('f00dcafe'));
});

test('the TRR connection info carries the origin attributes of the lookup context', () => {
  const text = fs.readFileSync(path.join(TREE, 'netwerk/protocol/http/TRRServiceChannel.cpp'), 'utf8');
  const begin = text.slice(text.indexOf('nsresult TRRServiceChannel::BeginConnect()'));
  const body = begin.slice(0, begin.indexOf('\n}\n'));
  // The attributes of the lookup's context are computed once, from the load info,
  // and given to the connection info and to the alternative service mapping in
  // place of the default ones (two Mozilla lines rewritten, registered).
  assert.match(body, /OriginAttributes stealthAttrs = StealthTRRContextOfLoadInfo\(mLoadInfo\)\.valueOr\(OriginAttributes\(\)\);  \/\/ STEALTH-HOOK\(network\/doh\)\n/);
  assert.match(body, /new nsHttpConnectionInfo\(\s*host, port, ""_ns, mUsername, proxyInfo, stealthAttrs, isHttps\);  \/\/ STEALTH-HOOK\(network\/doh\)/);
  assert.match(body, /mapping->GetConnectionInfo\(getter_AddRefs\(mConnectionInfo\), proxyInfo,\s+stealthAttrs\);  \/\/ STEALTH-HOOK\(network\/doh\)/);
  // The default OriginAttributes() no longer builds the connection info.
  assert.doesNotMatch(body, /proxyInfo, OriginAttributes\(\), isHttps\)/);
});
