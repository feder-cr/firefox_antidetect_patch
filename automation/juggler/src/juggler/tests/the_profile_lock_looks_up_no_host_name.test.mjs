// The profile lock looks up no host name.
//
// On Unix nsProfileLock::LockWithSymlink writes an address into the signature of its symlink lock. Mozilla takes it
// from PR_GetHostByName(the machine's own host name) unless MOZ_PROXY_BYPASS_PROTECTION is set: a lookup made at
// start-up, before any proxy is decided, through the system resolver, carrying the machine's name to the network
// (seen by the resolver trap of the realness suite on a runner whose name is not in /etc/hosts). Windows takes another
// lock and makes no such lookup. The block is compiled out with the two tagged lines below, so the signature keeps
// 127.0.0.1, as Mozilla's own MOZ_PROXY_BYPASS_PROTECTION build does; nothing else of that build is taken.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const LOCK = 'toolkit/profile/nsProfileLock.cpp';

test('the lookup of the host name in the symlink lock is compiled out by two tagged lines', () => {
  const text = fs.readFileSync(path.join(ROOT, LOCK), 'utf8').replace(/\r\n/g, '\n');
  const open = '#  ifndef MOZ_PROXY_BYPASS_PROTECTION\n#    if 0  // STEALTH-HOOK(network/doh)';
  const start = text.indexOf(open);
  assert.ok(start >= 0, 'the block that looks up the host name is not compiled out');
  const close = text.indexOf('#    endif  // STEALTH-HOOK(network/doh)\n#  endif', start);
  assert.ok(close > start, 'the compiled-out block has no tagged end');
  const block = text.slice(start, close);
  assert.match(block, /PR_GetSystemInfo\(PR_SI_HOSTNAME/);
  assert.match(block, /PR_GetHostByName\(hostname/);
  // Outside the compiled-out block the file asks for no host name and no lookup.
  const rest = text.slice(0, start) + text.slice(close);
  assert.doesNotMatch(rest, /PR_GetHostByName|PR_SI_HOSTNAME|gethostbyname|getaddrinfo/);
  // The signature still starts from the loopback address.
  assert.match(text.slice(0, start), /inaddr\.s_addr = htonl\(INADDR_LOOPBACK\);/);
  assert.equal(text.split('\n').filter(l => /STEALTH-HOOK\(/.test(l)).length, 2);
});
