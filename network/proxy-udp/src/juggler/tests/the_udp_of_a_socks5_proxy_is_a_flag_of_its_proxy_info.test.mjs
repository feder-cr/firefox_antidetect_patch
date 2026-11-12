// The engine carries the UDP of a PeerConnection and of HTTP/3 only through a SOCKS5 proxy the session measured coherent
// (the core's `udp: 'coherent'` on Browser.setBrowserProxy / Browser.setContextProxy). The Juggler says so with ONE flag of the
// nsIProxyInfo it already builds (CARRIES_COHERENT_UDP, 1 << 2 in nsIProxyInfo.idl); the engine reads it in PcUdpProxy and in the
// layer of an HTTP/3 socket. Absent, 'no' and 'unmeasured' are the same: no flag, a router that drops the UDP.
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
const CARRIES_COHERENT_UDP = 4;   // nsIProxyInfo.idl: 1 << 2

function load(calls) {
  const code = source.replace(/^export function /gm, 'function ') + '\nthis.newProxyInfoFor = newProxyInfoFor;';
  const context = {
    Math, TextEncoder, btoa, String, Array,
    Cc: {
      '@mozilla.org/network/protocol-proxy-service;1': { getService: () => ({
        newProxyInfoWithAuth: (...args) => { calls.push(args); return { args }; },
      }) },
      '@mozilla.org/security/hash;1': { createInstance: () => {
        const h = crypto.createHash('sha256');
        return { init() {}, update(b, n) { h.update(Buffer.from(b.slice(0, n))); }, finish() { return h.digest('latin1'); } };
      } },
    },
    Ci: { nsIProxyInfo: { TRANSPARENT_PROXY_RESOLVES_HOST: 1, CARRIES_COHERENT_UDP }, nsICryptoHash: { SHA256: 3 } },
  };
  vm.createContext(context);
  vm.runInContext(code, context);
  return context.newProxyInfoFor;
}

const flagsOf = (calls) => calls.map((args) => args[7]);

test("a SOCKS5 proxy declared 'coherent' carries the flag, and only the flag", () => {
  const calls = [];
  load(calls)({ type: 'socks', host: 'gw', port: 1080, bypass: [], udp: 'coherent' });
  assert.deepEqual(flagsOf(calls), [CARRIES_COHERENT_UDP]);
});

test("'no', 'unmeasured' and an absent field are the same: no flag", () => {
  for (const udp of ['no', 'unmeasured', undefined]) {
    const calls = [];
    load(calls)({ type: 'socks', host: 'gw', port: 1080, bypass: [], udp });
    assert.deepEqual(flagsOf(calls), [0], String(udp));
  }
});

test('a proxy that is not SOCKS5 never carries the flag, whatever the field says', () => {
  for (const type of ['http', 'https', 'socks4']) {
    const calls = [];
    load(calls)({ type, host: 'gw', port: 3128, bypass: [], udp: 'coherent' });
    assert.deepEqual(flagsOf(calls), [0], type);
  }
});

test('the two commands carry the field to the registry', () => {
  const handler = fs.readFileSync(path.join(JUGGLER, 'protocol', 'BrowserHandler.js'), 'utf8');
  assert.match(handler, /\['Browser\.setBrowserProxy'\]\(\{[^}]*\budp\b[^}]*\}\)[\s\S]*?setBrowserProxy\(\{[^}]*\budp\b/);
  assert.match(handler, /\['Browser\.setContextProxy'\]\(\{[^}]*\budp\b[^}]*\}\)[\s\S]*?setProxy\(\{[^}]*\budp\b/);
  const protocol = fs.readFileSync(path.join(JUGGLER, 'protocol', 'Protocol.js'), 'utf8');
  assert.equal((protocol.match(/udp: t\.Optional\(t\.Enum\(\['coherent', 'no', 'unmeasured'\]\)\)/g) || []).length, 2);
});
