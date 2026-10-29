// The chrome flag that marks a popup with an explicit size is ours: it is
// set in toolkit/components/windowwatcher/JugglerWindowSize.h and read in
// juggler/TargetRegistry.js, in a bit nsIWebBrowserChrome.idl leaves free.
// It used to be declared in that IDL as 1 << 25, the bit Firefox 159 gives
// to CHROME_MEDIA_PIP: a port would have marked every media
// Picture-in-Picture window as sized by the page, with no conflict to see.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

// {name: bit} of the single-bit constants of nsIWebBrowserChrome.idl.
function chromeBits(idl) {
  const bits = {};
  for (const m of idl.matchAll(/const unsigned long (\w+)\s*=\s*1 << (\d+);/g))
    bits[m[1]] = Number(m[2]);
  return bits;
}

function ourBit(text, name) {
  const m = text.match(new RegExp(`${name}\\s*=\\s*1u? << (\\d+);`));
  assert.ok(m, `${name} not found`);
  return Number(m[1]);
}

function collisions(bit, bits) {
  return Object.entries(bits).filter(([, b]) => b === bit).map(([n]) => n);
}

test('the C++ and the JS side use the same bit', () => {
  const cpp = ourBit(read('toolkit/components/windowwatcher/JugglerWindowSize.h'), 'kJugglerWindowExplicitSize');
  const js = ourBit(read('juggler/TargetRegistry.js'), 'JUGGLER_WINDOW_EXPLICIT_SIZE');
  assert.equal(cpp, js);
});

test('no flag of nsIWebBrowserChrome uses our bit', () => {
  const bits = chromeBits(read('toolkit/components/browser/nsIWebBrowserChrome.idl'));
  // Near-miss guard: an IDL this cannot parse would pass vacuously.
  assert.ok(Object.keys(bits).length >= 15, `only ${Object.keys(bits).length} flags read`);
  assert.ok(!('JUGGLER_WINDOW_EXPLICIT_SIZE' in bits), 'our flag is back in the Mozilla IDL');
  const bit = ourBit(read('juggler/TargetRegistry.js'), 'JUGGLER_WINDOW_EXPLICIT_SIZE');
  assert.deepEqual(collisions(bit, bits), []);
});

test('the check sees the 159 collision', () => {
  const idl159 = 'const unsigned long CHROME_SUPPRESS_ANIMATION     = 1 << 24;\n' +
                 'const unsigned long CHROME_MEDIA_PIP              = 1 << 25;\n';
  assert.deepEqual(collisions(25, chromeBits(idl159)), ['CHROME_MEDIA_PIP']);
});
