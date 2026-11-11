// With the DoH service in global mode 0 (a proxied lookup is a per-request
// TRR_ONLY request), the host's hosts file is read at START-UP, not when a proxy
// arrives: nsDNSService::Init calls TRRService::ReadEtcHostsFile whenever native
// HTTPS queries are on (Windows 11), and TRRServiceBase::OnTRRModeChange does the
// same once, the first time it runs. Both go through
// DNSServiceBase::DoReadEtcHostsFile, whose only guard is
// network.trr.exclude-etc-hosts, and Mozilla's default for it is true. Juggler
// writes false when a proxy appears (TrrPrefs.js), which is long after the read:
// every name in the host's hosts file would stay in the list of the names that
// TRR skips, and a proxied context would resolve those names with the system
// resolver, an answer that comes from the host.
//
// The fix is the default, on Mozilla's own line, with the hook: no session
// without a proxy ever sends a TRR request, so the exclusion list means nothing
// there.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

function declaration(name) {
  const text = read('modules/libpref/init/StaticPrefList.yaml');
  const start = text.indexOf(`\n- name: ${name}\n`);
  assert.ok(start >= 0, `${name} is declared`);
  return text.slice(start + 1, text.indexOf('\n\n', start + 1));
}

test('premise: the file is read at start-up, whatever the mode, behind the pref alone', () => {
  const service = read('netwerk/dns/nsDNSService2.cpp');
  assert.match(service, /  if \(mTrrService && httpsEnabled\) \{\n    mTrrService->ReadEtcHostsFile\(\);\n  \}/);
  assert.match(read('netwerk/dns/TRRServiceBase.cpp'),
    /mMode == nsIDNSService::MODE_TRRONLY \|\| mNativeHTTPSQueryEnabled\) &&\n      !readHosts\) \{\n    readHosts = true;\n    ReadEtcHostsFile\(\);/);
  const base = read('netwerk/dns/DNSServiceBase.cpp');
  const start = base.indexOf('void DNSServiceBase::DoReadEtcHostsFile(');
  assert.ok(base.indexOf('if (!StaticPrefs::network_trr_exclude_etc_hosts()) {\n    return;\n  }', start) > start);
});

test('the hosts file is not read: the default of network.trr.exclude-etc-hosts is false', () => {
  const lines = declaration('network.trr.exclude-etc-hosts').split('\n');
  const value = lines.filter(l => /^  value:/.test(l));
  assert.equal(value.length, 1);
  assert.match(value[0], /^  value: false  # STEALTH-HOOK\(network\/doh\): /);
});

test('the pref is declared nowhere else', () => {
  const pref = /network\.trr\.exclude-etc-hosts/;
  for (const file of ['modules/libpref/init/StealthPrefs.js', 'modules/libpref/init/StealthPrefList.yaml',
    'modules/libpref/init/all.js', 'browser/app/profile/firefox.js'])
    assert.doesNotMatch(read(file), pref, file);
});
