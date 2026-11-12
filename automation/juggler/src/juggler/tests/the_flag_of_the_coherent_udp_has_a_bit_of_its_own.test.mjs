// CARRIES_COHERENT_UDP (nsIProxyInfo.idl, `1 << 2`) is a value of ours in a numbered space that Mozilla owns: the Proxy Flags. The
// Juggler sets it by NUMBER (SessionRoute.js reads Ci.nsIProxyInfo.CARRIES_COHERENT_UDP) and the engine reads it by name, so a port
// that brings an upstream flag with the same bit would silently turn a Mozilla behaviour on (or ours off). This reads the IDL
// itself, so it catches ANY other constant of the interface that takes the bit, not only the two that exist today.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const IDL = path.join(JUGGLER, '..', 'netwerk', 'base', 'nsIProxyInfo.idl');

function constants() {
  assert.ok(fs.existsSync(IDL), 'the interface file is part of the tree these tests run in: ' + IDL);
  const text = fs.readFileSync(IDL, 'utf8').replace(/\/\*[\s\S]*?\*\//g, '');
  const found = {};
  for (const m of text.matchAll(/const\s+unsigned\s+(?:short|long)\s+(\w+)\s*=\s*([^;]+);/g)) {
    const expr = m[2].trim();
    const shift = expr.match(/^1\s*<<\s*(\d+)$/);
    found[m[1]] = shift ? 1 << Number(shift[1]) : Number(expr);
  }
  return found;
}

test('the flag of the coherent UDP is bit 2 and no other constant of nsIProxyInfo takes it', () => {
  const all = constants();
  assert.equal(all.CARRIES_COHERENT_UDP, 4, 'our flag is 1 << 2');
  // SOCKS_V4 = 4 and SOCKS_V5 = 5 are version numbers to compare against, not Proxy Flags: they are not in the bit space.
  const sharing = Object.entries(all).filter(([name, value]) =>
      name !== 'CARRIES_COHERENT_UDP' && !/^SOCKS_V\d$/.test(name) && (value & 4) !== 0);
  assert.deepEqual(sharing, [], 'another constant of nsIProxyInfo uses the bit of our flag (the SOCKS version constants are not flags)');
});
