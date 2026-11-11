// The engine does not read the host's proxy settings.
//
// network.proxy.type defaulted to 5 (system): the parent created the system
// settings object at the proxy service's Init, which opened and watched the
// WinINet "Internet Settings" key, and asked WinINet and the *_proxy variables
// for every channel, even when Juggler gives the proxy. The default is 0
// (direct): PrefsChanged then never creates the object and Resolve_Internal
// returns before any host read. The value is changed on Mozilla's own line,
// tagged; the pref is never declared a second time.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');
const YAML = 'modules/libpref/init/StaticPrefList.yaml';
// git grep exits 1 when nothing matches.
const gitGrep = args => {
  try {
    return execFileSync('git', ['grep', ...args], { cwd: TREE, encoding: 'utf8' });
  } catch (e) {
    if (e.status === 1) return '';
    throw e;
  }
};

function declaration() {
  const lines = read(YAML).split('\n');
  const at = lines.findIndex(l => l === '- name: network.proxy.type');
  assert.ok(at > 0, 'the pref is declared in StaticPrefList.yaml');
  const block = [];
  for (let i = at + 1; i < lines.length && /^\s+\S/.test(lines[i]); i++) block.push(lines[i]);
  return block;
}

test('network.proxy.type defaults to 0 and the hook is on the value line', () => {
  const block = declaration();
  const value = block.filter(l => /^\s+value:/.test(l));
  assert.equal(value.length, 1);
  assert.match(value[0], /^\s+value: 0\s+# STEALTH-HOOK\(network\/proxy\)/);
  assert.equal(block.filter(l => /type: RelaxedAtomicUint32/.test(l)).length, 1);
});

test('the pref is declared once in the whole tree, and no default file sets it', () => {
  const out = gitGrep(['-lE', '^- name: network[.]proxy[.]type$|pref[(][[:space:]]*"network[.]proxy[.]type"', '--',
    '*.yaml', '*.js', '*.mjs', '*.json', ':!*/test/*', ':!*/tests/*', ':!testing/*', ':!third_party/*']);
  assert.deepEqual(out.trim().split('\n'), [YAML]);
  for (const rel of [
    'modules/libpref/init/StealthPrefs.js',
    'modules/libpref/init/StealthPrefList.yaml',
    'modules/libpref/init/all.js',
    'modules/libpref/greprefs.js',
    'browser/app/profile/firefox.js',
    'browser/branding/unofficial/pref/firefox-branding.js',
    'browser/branding/official/pref/firefox-branding.js',
  ]) {
    if (fs.existsSync(path.join(TREE, rel))) assert.doesNotMatch(read(rel), /pref\(\s*"network\.proxy\.type"/, rel);
  }
});

test('no non-test source of ours sets another default or writes the pref', () => {
  const files = execFileSync('git', ['ls-files', 'juggler', '--', ':!juggler/tests/*'],
    { cwd: TREE, encoding: 'utf8' }).trim().split('\n').filter(f => /\.(js|mjs|jsm|cpp|h)$/.test(f));
  assert.ok(files.length > 10, 'juggler sources read');
  for (const rel of files)
    assert.doesNotMatch(read(rel), /network\.proxy\.type|PROXYCONFIG_|SetIntPref\(\s*"network\.proxy/, rel);
  // The C++ that is ours and talks about the proxy does not write the type either.
  const out = gitGrep(['-nE', '(SetInt|setIntPref)[A-Za-z]*[(][[:space:]]*"network[.]proxy[.]type"', '--',
    '*.cpp', '*.h', '*.js', '*.mjs', ':!*/test/*', ':!*/tests/*', ':!*test*.js', ':!testing/*', ':!third_party/*']);
  // Only Mozilla's own enterprise-policy / extension / preferences-UI writers remain.
  for (const line of out.trim().split('\n').filter(Boolean))
    assert.match(line, /^(browser\/components\/(enterprisepolicies|preferences)|toolkit\/components\/extensions)\//, line);
});

test('the proxy service creates the system settings only for type 5 (so 0 reads nothing from the host)', () => {
  const text = read('netwerk/base/nsProtocolProxyService.cpp');
  const from = text.indexOf('if (mProxyConfig == PROXYCONFIG_SYSTEM) {');
  assert.ok(from > 0);
  assert.match(text.slice(from, from + 400), /do_GetService\(NS_SYSTEMPROXYSETTINGS_CONTRACTID\)/);
  // Resolve_Internal returns before any host read when the type is direct.
  assert.match(text, /mProxyConfig == PROXYCONFIG_DIRECT/);
});
