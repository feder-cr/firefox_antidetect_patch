// network.trr.async_connInfo is @IS_EARLY_BETA_OR_EARLIER@ in StaticPrefList.yaml:
// ON in a nightly (milestone with "a1"), OFF in a release. When it is ON, TRR
// takes the connection info it cached from ONE proxy lookup (default context,
// made when the first DNS request arrives) and skips the proxy lookup of the
// request: a proxy set later by Juggler (no notification reaches the proxy
// service: only registerChannelFilter notifies, once at startup) would be
// ignored, and the connection would stay DIRECT. Our build is a release
// (milestone 159.0, no --as-milestone), so the pref is OFF and every DoH request
// asks the proxy filter itself. This test fails if the build ever becomes a
// nightly, or the pref stops following the milestone.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

test('the pref follows the milestone, as Mozilla wrote it', () => {
  assert.match(read('modules/libpref/init/StaticPrefList.yaml'),
      /- name: network\.trr\.async_connInfo\n  type: RelaxedAtomicBool\n  value: @IS_EARLY_BETA_OR_EARLIER@\n/);
  assert.match(read('modules/libpref/init/StaticPrefList.yaml'),
      /#ifdef EARLY_BETA_OR_EARLIER\n#define IS_EARLY_BETA_OR_EARLIER true/);
  assert.match(read('build/moz.configure/init.configure'),
      /is_early_beta_or_earlier=is_nightly,/);
});

test('the milestone of this build is a release', () => {
  const lines = read('config/milestone.txt').split(/\r?\n/).filter(l => l && !l.startsWith('#'));
  assert.equal(lines.at(-1), '159.0');
  assert.doesNotMatch(lines.at(-1), /a1/);
  assert.doesNotMatch(read('.mozconfig'), /as-milestone/);
});

test('the cached connection info is used only behind that pref, and not for another context', () => {
  const text = read('netwerk/dns/TRR.cpp');
  // One Mozilla line is rewritten (registered): the last operand of the condition
  // is Mozilla's, and the lookup's own proxy is a plain operand after it.
  assert.match(text, /  if \(UseDefaultServer\(\) && !useOHTTP &&\n      StaticPrefs::network_trr_async_connInfo\(\) && StealthTRRContextOfSuffix\(mOriginSuffix\)\.isNothing\(\)\) \{  \/\/ STEALTH-HOOK\(network\/doh\)/);
  // The only reader of the cached info that opens a connection (the preconnect)
  // is on application-foreground, which desktop never fires.
  assert.match(read('modules/libpref/init/StaticPrefList.yaml'),
      /application-foreground is not\n# fired on desktop\./);
});
