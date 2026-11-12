// With the Windows provider gone (geolocation_windows_provider_never_selected),
// the provider of every process is the network one, and it starts the Wi-Fi
// monitor when geo.provider.network.scan is true (its fallback in
// NetworkGeolocationProvider.sys.mjs): wlanapi.dll is loaded in the parent and the
// host's access points are scanned, although no URL is declared and the result
// goes nowhere (measured, study notes/geolocation-no-host-provider.md, M-d).
//
// The pref is declared false ONCE, in StealthPrefs.js: Mozilla declares it nowhere
// (the only default is the `true` fallback in the .sys.mjs, left as Mozilla wrote
// it). Every other reader of the Wi-Fi monitor is closed on its own:
//   - Region.sys.mjs: only when browser.region.network.scan (false in all.js);
//   - GeolocationSystemWin.cpp: only on the permission-prompt path, which the
//     declared OS switch (geolocation_system_switch_declared_on) never takes.
// The acceptance contract for the end-to-end is at the top of
// geolocation_windows_provider_never_selected.test.mjs.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

test('geo.provider.network.scan is false in our default prefs, declared once', () => {
  const prefs = new Map();
  vm.runInNewContext(read('modules/libpref/init/StealthPrefs.js'), {
    pref: (name, value) => prefs.set(name, value),
    sticky_pref: (name, value) => prefs.set(name, value),
  });
  assert.equal(prefs.get('geo.provider.network.scan'), false);
  const out = execFileSync('git', ['grep', '-lF', 'geo.provider.network.scan', '--',
    '*.js', '*.yaml', '*.mjs', '*.cpp', '*.h', ':!*/test/*', ':!*/tests/*', ':!testing/*'],
  { cwd: TREE, encoding: 'utf8' });
  assert.deepEqual(out.trim().split('\n').sort(), [
    'dom/system/NetworkGeolocationProvider.sys.mjs',
    'modules/libpref/init/StealthPrefs.js',
  ]);
});

test("the network provider's Wi-Fi gate is Mozilla's own line, reading that pref", () => {
  const provider = read('dom/system/NetworkGeolocationProvider.sys.mjs');
  assert.ok(provider.includes('"geo.provider.network.scan",\n    true\n'));
  assert.ok(provider.includes(
    'return Cc["@mozilla.org/wifi/monitor;1"] && this._wifiScanningEnabled;'));
  assert.ok(provider.includes('if (this.isWifiScanningEnabled) {'));
  assert.doesNotMatch(provider, /STEALTH/);
});

test('nothing else in the parent reaches the Wi-Fi monitor (wlanapi.dll) on a normal path', () => {
  const out = execFileSync('git', ['grep', '-lE', 'wifi/monitor;1|components::WifiMonitor|nsIWifiMonitor',
    '--', '*.cpp', '*.h', '*.mjs', '*.js', '*.jsm', '*.conf', ':!*/test/*', ':!*/tests/*', ':!netwerk/wifi/*'],
  { cwd: TREE, encoding: 'utf8' });
  assert.deepEqual(out.trim().split('\n').sort(), [
    'dom/geolocation/GeolocationSystemWin.cpp',
    'dom/system/NetworkGeolocationProvider.sys.mjs',
    'netwerk/build/components.conf',
    'netwerk/build/nsNetCID.h',
    'toolkit/modules/Region.sys.mjs',
  ]);
  const region = read('toolkit/modules/Region.sys.mjs');
  assert.ok(region.includes('if (lazy.wifiScanningEnabled) {\n      let wifiData = await this.#fetchWifiData();'));
  assert.ok(region.includes('"browser.region.network.scan"'));
  assert.ok(read('modules/libpref/init/all.js').includes('pref("browser.region.network.scan", false);'));
});
