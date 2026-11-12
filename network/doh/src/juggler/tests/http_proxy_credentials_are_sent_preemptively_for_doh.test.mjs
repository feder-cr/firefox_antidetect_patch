// TRRServiceChannel has no auth provider, so the 407 on the DoH CONNECT is never
// answered; it sends the Proxy-Authorization of the nsIProxyInfo instead
// (TRRServiceChannel.cpp). So an http/https proxy with credentials carries a
// `Basic` header in its proxy info, built from the UTF-8 bytes of user:pass.
// SOCKS authenticates in its own handshake and keeps the header empty.
// Credentials are never printed here.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(JUGGLER, 'SessionRoute.js'), 'utf8');

function load(calls) {
  const code = source.replace(/^export function /gm, 'function ') +
      '\nthis.newProxyInfoFor = newProxyInfoFor;';
  const context = {
    Math, TextEncoder, btoa, String, Uint8Array, Array,
    Cc: {
      '@mozilla.org/network/protocol-proxy-service;1': { getService: () => ({
        newProxyInfoWithAuth: (...args) => { calls.push(args); return { args }; },
      }) },
      // nsICryptoHash, enough for the isolation key of an http proxy.
      '@mozilla.org/security/hash;1': { createInstance: () => {
        const h = crypto.createHash('sha256');
        return { init() {}, update(b, n) { h.update(Buffer.from(b.slice(0, n))); }, finish() { return h.digest('latin1'); } };
      } },
    },
    Ci: { nsICryptoHash: { SHA256: 3 } },
  };
  vm.createContext(context);
  vm.runInContext(code, context);
  return context.newProxyInfoFor;
}

const AUTH_HEADER = 5; // aProxyAuthorizationHeader in newProxyInfoWithAuth
const basic = (s) => 'Basic ' + Buffer.from(s, 'utf8').toString('base64');

test('an http and an https proxy with credentials carry a Basic header', () => {
  const calls = [];
  const newProxyInfoFor = load(calls);
  newProxyInfoFor({ type: 'http', host: 'gw', port: 3128, username: 'u', password: 'p' });
  newProxyInfoFor({ type: 'https', host: 'gw', port: 443, username: 'u', password: 'p' });
  assert.equal(calls[0][AUTH_HEADER], basic('u:p'));
  assert.equal(calls[1][AUTH_HEADER], basic('u:p'));
  // The credentials themselves still stay out of the endpoint (necko refuses them).
  for (const args of calls)
    assert.deepEqual([args[3], args[4]], ['', '']);
});

test('no credentials, no header', () => {
  const calls = [];
  const newProxyInfoFor = load(calls);
  newProxyInfoFor({ type: 'http', host: 'gw', port: 3128 });
  newProxyInfoFor({ type: 'https', host: 'gw', port: 443, username: '', password: '' });
  assert.equal(calls[0][AUTH_HEADER], '');
  assert.equal(calls[1][AUTH_HEADER], '');
});

test('a user without a password still sends user:', () => {
  const calls = [];
  load(calls)({ type: 'http', host: 'gw', port: 1, username: 'u' });
  assert.equal(calls[0][AUTH_HEADER], basic('u:'));
});

test('SOCKS credentials travel in the handshake and the header stays empty', () => {
  const calls = [];
  const newProxyInfoFor = load(calls);
  newProxyInfoFor({ type: 'socks', host: 'gw', port: 1080, username: 'u', password: 'p' });
  newProxyInfoFor({ type: 'socks4', host: 'gw', port: 1080, username: 'u', password: 'p' });
  for (const args of calls) {
    assert.equal(args[AUTH_HEADER], '');
    assert.deepEqual([args[3], args[4]], ['u', 'p']);
  }
});

test('non-ASCII characters are encoded as UTF-8, not as Latin-1', () => {
  const calls = [];
  load(calls)({ type: 'http', host: 'gw', port: 1, username: 'üser', password: 'pässwörd€' });
  const header = calls[0][AUTH_HEADER];
  assert.equal(header, basic('üser:pässwörd€'));
  const decoded = Buffer.from(header.slice('Basic '.length), 'base64');
  assert.equal(decoded.toString('utf8'), 'üser:pässwörd€');
});
