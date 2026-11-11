// Whether the session is under parental controls is declared, never read from
// the OS: with "on" Firefox skips TRR for every name (nsHostResolver.cpp,
// TRR_PARENTAL_CONTROL) and the lookup goes to the OS resolver, and it adds
// "Prefer: safe" to requests. The one place Firefox asks the OS is
// nsHttpHandler::UpdateParentalControlsEnabled; every other reader of the
// resolver (TRRService, TRRServiceParent, the socket process) takes the value
// from it. The behaviour is in netwerk/test/gtest/TestStealthTRRContext.cpp.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

test('the declared value is set, and the function returns before the OS service is created', () => {
  const text = read('netwerk/protocol/http/nsHttpHandler.cpp');
  const start = text.indexOf('void nsHttpHandler::UpdateParentalControlsEnabled(bool waitForCompletion) {');
  const declared = text.indexOf('sParentalControlsEnabled = StealthParentalControlsEnabled();', start);
  const early = text.indexOf('if (!sParentalControlsEnabled) {\n    return;\n  }', declared);
  const os = text.indexOf('do_CreateInstance("@mozilla.org/parental-controls-service;1")', start);
  assert.ok(start > 0 && declared > start, 'the value is not declared');
  assert.ok(early > declared, 'no return after the declared value');
  assert.ok(os > early, 'the OS service is created before the return');
  // The child process still returns first, as Mozilla wrote it.
  assert.ok(text.indexOf('if (!XRE_IsParentProcess()) {', start) < declared);
});

test('the declared value is false, in our file', () => {
  assert.match(read('netwerk/dns/StealthParentalControls.cpp'),
      /bool StealthParentalControlsEnabled\(\) \{ return false; \}/);
  assert.match(read('netwerk/dns/moz.build'),
      /UNIFIED_SOURCES \+= \["StealthParentalControls\.cpp"\]  # STEALTH-HOOK\(network\/doh\)/);
});

test('the resolver reads the flag from the handler, never from the OS service', () => {
  assert.match(read('netwerk/dns/TRRService.cpp'),
      /return nsHttpHandler::GetParentalControlsEnabled\(\);/);
  assert.match(read('netwerk/dns/nsHostResolver.cpp'),
      /if \(TRRService::Get\(\)->ParentalControlEnabled\(\)\) \{\n\s+aRec->RecordReason\(TRRSkippedReason::TRR_PARENTAL_CONTROL\);/);
  // Only the handler creates the OS service, among the network code.
  for (const file of ['netwerk/dns/TRRService.cpp', 'netwerk/dns/TRRServiceParent.cpp',
    'netwerk/dns/nsHostResolver.cpp', 'netwerk/dns/nsDNSService2.cpp'])
    assert.doesNotMatch(read(file), /do_CreateInstance\("@mozilla\.org\/parental-controls-service;1"\)/, file);
});

// The /etc/hosts file: the core declares network.trr.exclude-etc-hosts=false, and
// then Mozilla's own guard reads nothing.
test('with network.trr.exclude-etc-hosts=false the hosts file is never read', () => {
  const text = read('netwerk/dns/DNSServiceBase.cpp');
  const start = text.indexOf('void DNSServiceBase::DoReadEtcHostsFile(');
  const guard = text.indexOf('if (!StaticPrefs::network_trr_exclude_etc_hosts()) {\n    return;\n  }', start);
  const read_ = text.indexOf('rust_parse_etc_hosts(&path, aCallback);', start);
  assert.ok(start > 0 && guard > start && read_ > guard);
  // The only readers of the file go through that guard.
  for (const file of ['netwerk/dns/TRRService.cpp', 'netwerk/dns/TRRServiceParent.cpp'])
    assert.doesNotMatch(read(file), /rust_parse_etc_hosts/, file);
});
