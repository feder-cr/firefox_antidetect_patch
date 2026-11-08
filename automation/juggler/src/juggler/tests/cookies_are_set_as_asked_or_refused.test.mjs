// Browser.setCookies, loaded the way the parent process loads
// TargetRegistry.js, on a cookie service that keeps what it is given.
//
// Three defects, read in the source (TargetRegistry.js, nsICookieManager.idl,
// CookieValidation.cpp, CookieStorage.cpp):
// - sameSite 'None' was stored as SAMESITE_UNSET, so SameSite=None, which
//   invisible_core's persona cookies declare, became no attribute at all, and
//   the refusal Firefox gives a non-Secure SameSite=None never ran;
// - a session cookie sent with expires -1 got an expiry of -1000 ms, which
//   Firefox honours for session cookies too: the cookie was expired, and gone,
//   before anyone read it;
// - Services.cookies.add answers NS_OK for a cookie it refuses and puts the
//   reason in the nsICookieValidation it returns, which was dropped: the
//   command answered success for a cookie Firefox did not store.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

const nsICookie = { SAMESITE_UNSET: 256, SAMESITE_NONE: 0, SAMESITE_LAX: 1, SAMESITE_STRICT: 2, SCHEME_UNSET: 0 };
const nsICookieValidation = { eOK: 0, eRejectedEmptyNameAndValue: 1, eRejectedNoneRequiresSecure: 8 };
const Ci = { nsICookie, nsICookieValidation };

// What CookieValidation and CookieStorage do with the cookies below.
const stored = [];
const cookies = {
  add(host, path, name, value, secure, httpOnly, isSession, expiry, attrs, sameSite) {
    if (!name && !value)
      return { result: nsICookieValidation.eRejectedEmptyNameAndValue };
    if (sameSite === nsICookie.SAMESITE_NONE && !secure)
      return { result: nsICookieValidation.eRejectedNoneRequiresSecure };
    if (expiry <= Date.now())
      return { result: nsICookieValidation.eOK };   // expired: dropped, as Firefox does
    stored.push({ name, sameSite, isSession });
    return { result: nsICookieValidation.eOK };
  },
};

class Helper {}
const { BrowserContext } = load('TargetRegistry.js', {
  Ci, Cc: {}, Components: { results: {} },
  ChromeUtils: { importESModule: () => ({ Helper }) },
  Services: { cookies },
  NetUtil: {},
  dump: () => {},
}, ['BrowserContext']);

function set(list) {
  stored.length = 0;
  const context = Object.create(BrowserContext.prototype);
  context.userContextId = 3;
  return () => context.setCookies(list);
}

test('SameSite=None is stored as None', () => {
  set([{ name: 'a', value: '1', domain: 'example.com', path: '/', secure: true, sameSite: 'None' }])();
  assert.equal(stored.length, 1);
  assert.equal(stored[0].sameSite, nsICookie.SAMESITE_NONE);
});

test('no sameSite is stored as no attribute', () => {
  set([{ name: 'a', value: '1', domain: 'example.com', path: '/' }])();
  assert.equal(stored[0].sameSite, nsICookie.SAMESITE_UNSET);
});

test('a session cookie sent with expires -1 is kept', () => {
  set([{ name: 's', value: '1', domain: 'example.com', path: '/', expires: -1 }])();
  assert.equal(stored.length, 1);
  assert.equal(stored[0].isSession, true);
});

test('a cookie Firefox refuses fails the command with the reason, after the others are set', () => {
  const run = set([
    { name: 'ok', value: '1', domain: 'example.com', path: '/' },
    { name: 'bad', value: '1', domain: 'example.com', path: '/', secure: false, sameSite: 'None' },
  ]);
  assert.throws(run, /refused the cookie "bad" \(eRejectedNoneRequiresSecure\); the others are set/);
  assert.deepEqual(stored.map(c => c.name), ['ok']);
});
