// Behind a proxy all name resolution is Firefox's own DNS over HTTPS (TRR mode
// 3, declared by the core), sent through the session proxy. So the engine has
// no gate of its own in front of the resolver: DNSForbiddenByActiveProxy is
// Mozilla's, as upstream wrote it.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');
const exists = rel => fs.existsSync(path.join(TREE, rel));

test('the DNS declaration of ours is gone', () => {
  assert.equal(exists('netwerk/dns/StealthDNSDeclaration.cpp'), false);
  assert.equal(exists('netwerk/dns/StealthDNSDeclaration.h'), false);
  assert.doesNotMatch(read('netwerk/dns/moz.build'), /StealthDNSDeclaration/);
});

test('DNSForbiddenByActiveProxy is upstream, line for line', () => {
  const text = read('netwerk/dns/DNSServiceBase.cpp');
  assert.doesNotMatch(text, /STEALTH/);
  assert.doesNotMatch(text, /StealthDNS/);
  const start = text.indexOf('bool DNSServiceBase::DNSForbiddenByActiveProxy(');
  const end = text.indexOf('void DNSServiceBase::DoReadEtcHostsFile');
  assert.ok(start > 0 && end > start);
  assert.match(text.slice(start, end), /RESOLVE_IGNORE_SOCKS_DNS\) \{\n    return false;\n  \}\n\n  \/\/ TODO\(Bug 1890542\)/);
});

test('no pref of ours declares a proxy to the resolver', () => {
  assert.doesNotMatch(read('modules/libpref/init/StealthPrefList.yaml'), /zoom\.stealth\.dns\./);
});
