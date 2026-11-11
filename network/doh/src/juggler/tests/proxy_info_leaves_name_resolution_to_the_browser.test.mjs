// The proxy info of the session does NOT say "the proxy resolves the host".
// Names are resolved by Firefox itself (DNS over HTTPS through the session
// proxy, TRR mode 3 declared by the core) and the browser connects to the IP,
// as it does at home. A TRANSPARENT_PROXY_RESOLVES_HOST proxy would make
// necko hand the proxy a name and skip its own lookup.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
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
    Math,
    Cc: { '@mozilla.org/network/protocol-proxy-service;1': { getService: () => ({
      newProxyInfoWithAuth: (...args) => { calls.push(args); return { args }; },
    }) } },
    // The value is the one of nsIProxyInfo.idl: 1 << 0.
    Ci: { nsIProxyInfo: { TRANSPARENT_PROXY_RESOLVES_HOST: 1 } },
  };
  vm.createContext(context);
  vm.runInContext(code, context);
  return context.newProxyInfoFor;
}

test('a SOCKS and an HTTP proxy are built with no flags', () => {
  const calls = [];
  const newProxyInfoFor = load(calls);
  newProxyInfoFor({ type: 'socks', host: 'gw', port: 1080, username: 'u', password: 'p' });
  newProxyInfoFor({ type: 'http', host: 'gw', port: 3128 });
  assert.equal(calls.length, 2);
  for (const args of calls) {
    assert.equal(args.length, 10, 'the signature of newProxyInfoWithAuth');
    assert.equal(args[7], 0, 'aFlags');
  }
});

test('the credentials still travel with a SOCKS endpoint only', () => {
  const calls = [];
  const newProxyInfoFor = load(calls);
  newProxyInfoFor({ type: 'socks', host: 'gw', port: 1080, username: 'u', password: 'p' });
  newProxyInfoFor({ type: 'http', host: 'gw', port: 3128, username: 'u', password: 'p' });
  assert.deepEqual([calls[0][3], calls[0][4]], ['u', 'p']);
  assert.deepEqual([calls[1][3], calls[1][4]], ['', '']);
});

test('juggler never asks for the proxy to resolve the host', () => {
  for (const file of fs.readdirSync(JUGGLER)) {
    if (!/\.(js|mjs|jsm)$/.test(file))
      continue;
    assert.doesNotMatch(fs.readFileSync(path.join(JUGGLER, file), 'utf8'),
        /RESOLVES_HOST/, file);
  }
});
