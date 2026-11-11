// The /etc/hosts file: the engine declares network.trr.exclude-etc-hosts=false (juggler/TrrPrefs.js), and
// then Mozilla's own guard reads nothing.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

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
