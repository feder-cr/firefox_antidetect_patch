// The update system is the retail's (build/moz.configure/update-programs.configure
// is Mozilla's), and our own file, toolkit/mozapps/update/StealthUpdate.sys.mjs,
// refuses what follows an answer of the update server: no offered update is
// taken, nothing is downloaded, no scheduled task is left on the host.
//
// Two halves: a source guard that the hooks sit where they must, with Mozilla's
// lines around them intact, and a behaviour check of our functions on stubs.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const UPDATE = path.join(TREE, 'toolkit/mozapps/update');
const HOOK = 'STEALTH-HOOK(stillborn/updater)';
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

test('the update programs are configured as Mozilla wrote them', () => {
  const text = read('build/moz.configure/update-programs.configure');
  assert.doesNotMatch(text, /STEALTH/);
  assert.match(text, /"--enable-updater",\n\s+default=updater_default,/);
  assert.match(text, /"--enable-maintenance-service",/);
  assert.match(text, /"--disable-update-agent",/);
  const mozconfig = read('.mozconfig');
  assert.match(mozconfig, /^export MAR_CHANNEL_ID=firefox-mozilla-release$/m);
  assert.match(mozconfig, /^export ACCEPTED_MAR_CHANNEL_IDS=firefox-mozilla-release$/m);
  assert.doesNotMatch(mozconfig, /^ac_add_options --disable-(updater|maintenance-service|update-agent)/m);
});

test("the answer is parsed by Mozilla, then none is taken, before it is used", () => {
  const text = read('toolkit/mozapps/update/UpdateService.sys.mjs');
  const parsed = text.indexOf('      updates = this.#parseUpdates(request);');
  const hook = text.indexOf(`    updates = StealthUpdate.noneOffered(updates); // ${HOOK}`);
  const logged = text.indexOf('"CheckerService:#updateCheck - number of updates available: "');
  assert.ok(parsed > 0, "Mozilla's parse is gone");
  assert.ok(hook > parsed, 'the hook must follow the parse');
  assert.ok(logged > hook, 'the hook must come before the updates are used');
  // The request goes out as Mozilla builds it: nothing of ours before the parse
  // in the check.
  const check = text.indexOf('  async #updateCheck(');
  assert.ok(check > 0);
  assert.equal(text.slice(check, parsed).includes('Stealth'), false);
});

test('every download is refused at its one door', () => {
  const text = read('toolkit/mozapps/update/UpdateService.sys.mjs');
  const door = text.indexOf('  async #downloadUpdate(update) {');
  assert.ok(door > 0, "Mozilla's #downloadUpdate is gone");
  const body = text.slice(door, door + 400);
  assert.match(body, new RegExp(
    `// ${HOOK.replace(/[()/]/g, '\\$&')}\\n\\s+if \\(StealthUpdate\\.refusesDownload\\(\\)\\) \\{\\n\\s+return Ci\\.nsIApplicationUpdateService\\.DOWNLOAD_FAILURE_GENERIC;`));
});

test('the background update task is never registered', () => {
  const text = read('toolkit/mozapps/update/BackgroundUpdate.sys.mjs');
  const fn = text.indexOf('  async _registerBackgroundUpdateTask(taskId = this.taskId) {');
  assert.ok(fn > 0, "Mozilla's _registerBackgroundUpdateTask is gone");
  const head = text.slice(fn, fn + 250);
  assert.match(head, /if \(StealthUpdate\.refusesHostTask\(\)\) \{\n\s+return null;/);
  assert.ok(text.indexOf('lazy.TaskScheduler.registerTask(') > fn + head.indexOf('return null'));
});

test('every line naming StealthUpdate carries the marker', () => {
  for (const rel of ['toolkit/mozapps/update/UpdateService.sys.mjs',
                     'toolkit/mozapps/update/BackgroundUpdate.sys.mjs',
                     'toolkit/mozapps/update/moz.build']) {
    const lines = read(rel).split('\n');
    const named = lines.map((l, i) => [l, i]).filter(([l]) => l.includes('StealthUpdate'));
    assert.ok(named.length >= 1, `${rel}: no hook`);
    for (const [line, i] of named) {
      const marked = line.includes(HOOK) || lines[i - 1].includes(HOOK);
      assert.ok(marked, `${rel}: a hook without its marker: ${line}`);
    }
  }
});

test('the update cadence is the release one', () => {
  const prefs = read('browser/branding/unofficial/pref/firefox-branding.js');
  for (const [name, value] of [['app.update.interval', '21600'],
                               ['app.update.checkInstallTime.days', '63'],
                               ['app.update.promptWaitTime', '691200'],
                               ['app.update.badgeWaitTime', '345600']]) {
    const lines = prefs.split('\n').filter(l => l.startsWith(`pref("${name}",`));
    assert.equal(lines.length, 1, `${name} declared ${lines.length} times`);
    assert.ok(lines[0].startsWith(`pref("${name}", ${value});`), lines[0]);
  }
});

function load() {
  let src = fs.readFileSync(path.join(UPDATE, 'StealthUpdate.sys.mjs'), 'utf8');
  src = src.replace(/^export /gm, '');
  src += '\nmodule.exports = { noneOffered, refusesDownload, refusesHostTask };\n';
  const logged = [];
  const sandbox = { module: { exports: {} }, console: { info: m => logged.push(m) } };
  vm.runInNewContext(src, sandbox);
  return { m: sandbox.module.exports, logged };
}

test('our functions: none offered, every download and task refused', () => {
  const { m, logged } = load();
  const offered = [{}, {}];
  const out = m.noneOffered(offered);
  assert.equal(out.length, 0);
  assert.notEqual(out, offered, 'a new array, the parsed one untouched');
  assert.equal(offered.length, 2);
  assert.equal(logged.length, 1);
  assert.equal(m.noneOffered([]).length, 0);
  assert.equal(logged.length, 1, 'nothing to say when nothing was offered');
  assert.equal(m.refusesDownload(), true);
  assert.equal(m.refusesHostTask(), true);
});
