// The proxy of a context answers Firefox's proxy lookup, with its credentials.
//
// Browser.setBrowserProxy and Browser.setContextProxy record the proxy, and the filter of the registry
// (TargetRegistry.getProxyInfo -> SessionRoute.newProxyInfoFor) answers every lookup with it: SOCKS with the
// context's credentials, through newProxyInfoWithAuth. A UDP socket asks the same lookup as any channel.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = rel => fs.readFileSync(path.join(JUGGLER, rel), 'utf8');

test('the browser proxy and a context proxy are recorded where the lookup reads them', () => {
  const registry = read('TargetRegistry.js');
  const browserProxy = registry.slice(registry.indexOf('  setBrowserProxy(proxy) {'));
  assert.ok(browserProxy.indexOf('this._browserProxy = proxy;') > 0);
  const contextProxy = registry.slice(registry.indexOf('  setProxy(proxy) {'));
  assert.ok(contextProxy.indexOf('this._proxy = proxy;') > 0);
  assert.match(registry, /getProxyInfo\(channel\) \{/);
});

test('the answer carries the credentials of the proxy', () => {
  assert.match(read('SessionRoute.js'), /protocolProxyService\.newProxyInfoWithAuth\(/);
});
