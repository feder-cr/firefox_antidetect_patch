// The persona's location comes ONLY from the core's declaration (the geolocation
// override sent by Browser.setGeolocationOverride). The engine never reads the
// host for a position. The Windows location provider (WindowsLocationProvider and
// the actors of the WINDOWS_UTILS utility process: geolocation.dll,
// locationframeworkps.dll) is not in the tree any more: its files are deleted and
// no moz.build, IPDL list or source names it, so nothing can start it. The hook
// StealthSelectGeolocationProvider only keeps the Linux (Portal, GeoClue), macOS
// (CoreLocation) and Android providers out. A watchPosition reaches the PARENT's
// global service even with an override (Geolocation.cpp StartDevice ->
// AddGeolocationListener -> ContentParent.cpp RecvAddGeolocationListener).
//
// ACCEPTANCE CONTRACT for the end-to-end of the whole family (hooks A, B, C, D;
// the e2e itself is another agent's). Browser with a declared geolocation override
// (Browser.setGeolocationOverride, made-up 45.0 / 9.0 / accuracy 1500), headless,
// permission granted for the probe origin, then on the page, in this order:
// navigator.permissions.query({name:'geolocation'}), getCurrentPosition, and
// watchPosition left running 10 s (first callback, then clearWatch).
//   1. Page-visible: permission state 'granted'; getCurrentPosition and the first
//      watchPosition callback give coords 45.0 / 9.0, accuracy 1500, altitude,
//      altitudeAccuracy, heading, speed null; the watch gets exactly one callback
//      and no error (not code 1 PERMISSION_DENIED, not code 2).
//   2. Page-visible (hook D): in every callback position.timestamp - Date.now(),
//      with Date.now() read right after the timestamp, is between -2 and 0 ms
//      (never +86,400,000 ms, never growing positive); two reads of the timestamp
//      of the same position taken 50 ms apart differ by about 50 ms.
//   3. Host-visible, for every firefox.exe of the process tree (parent, content,
//      utility, socket, GPU...), module BASE NAMES ever loaded, sampled after each
//      of the three steps and 2 s after the last: geolocation.dll,
//      locationframeworkps.dll, wlanapi.dll and capabilityaccessmanagerclient.dll
//      are all ABSENT (case-insensitive).
//   4. Host-visible: no process of the tree has the command line option
//      -sandboxingKind 3 (WINDOWS_UTILS); 0 (GENERIC_UTILITY) may exist.
//   5. Control: the same run on the previous candidate (before these hooks) shows
//      geolocation.dll + locationframeworkps.dll + a -sandboxingKind 3 process
//      after watchPosition, and capabilityaccessmanagerclient.dll after the
//      permission query; the e2e must fail on it and pass on this build.
//   6. Without an override (declared position absent), same run: getCurrentPosition
//      and watchPosition both answer POSITION_UNAVAILABLE (code 2, message
//      "Unknown error acquiring position") within about 100 ms, and items 3 and 4
//      hold as well.
//
// This file pins hook A (the provider is never the Windows one, nor any other
// host provider); B, C and D have their own tests.
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
const SERVICE = 'toolkit/components/geolocation/GeolocationService.cpp';
const OWN_CPP = 'toolkit/components/geolocation/StealthGeolocation.cpp';
const OWN_H = 'toolkit/components/geolocation/StealthGeolocation.h';
const FOLDER = 'fingerprint/geolocation/no-host-provider';

const service = read(SERVICE);
const init = service.slice(service.indexOf('nsresult GeolocationService::Init()'),
  service.indexOf('GeolocationService::~GeolocationService()'));

test('Init() carries the hook: an include and a call with an early return, marked STEALTH-HOOK', () => {
  assert.ok(service.includes(
    `#include "mozilla/StealthGeolocation.h"  // STEALTH-HOOK(${FOLDER})\n`));
  assert.ok(init.includes(
    `  // STEALTH-HOOK(${FOLDER}): the provider is the network one, never a host one.\n` +
    '  if (StealthSelectGeolocationProvider(mProvider)) {\n' +
    '    return NS_OK;\n' +
    '  }\n'));
  const hookLines = service.split('\n').filter(l => l.includes('STEALTH-HOOK'));
  assert.equal(hookLines.length, 2);
  const added = ['#include "mozilla/StealthGeolocation.h"', 'StealthSelectGeolocationProvider', 'return NS_OK;'];
  assert.ok(added.length <= 5);
});

test('the hook runs after the shutdown observer and before every provider Mozilla selects', () => {
  const hook = init.indexOf('StealthSelectGeolocationProvider(mProvider)');
  assert.ok(hook > 0);
  assert.ok(init.indexOf('obs->AddObserver(this, "xpcom-shutdown", false);') < hook);
  for (const marker of ['#ifdef MOZ_WIDGET_ANDROID', 'geo.provider.use_mls',
    'MOZ_WIDGET_COCOA', 'new PortalLocationProvider()', 'new GeoclueLocationProvider()'])
    assert.ok(init.indexOf(marker) > hook, marker + ' must come after the hook');
});

// The Windows provider is removed from the build and the tree. These are the only
// places that may still carry one of its names: dead names (a metric's description,
// the module ownership map, the permission path's own helper), and the files that
// say it is gone.
const DEAD_NAME_LEAVES = new Set([
  'dom/geolocation/GeolocationSystemWin.cpp',      // OpenWindowsLocationSettings: the OS settings page, not the provider
  'dom/geolocation/metrics.yaml',                  // windows_failure metric description
  'mots.yaml',                                     // ownership glob
  'remote/test/puppeteer/packages/browsers/src/browser-data/chrome.ts',  // getChromeWindowsLocation: unrelated
]);
const gitGrep = args => {
  try {
    return execFileSync('git', ['grep', ...args], { cwd: TREE, encoding: 'utf8' });
  } catch (e) {
    if (e.status === 1) return '';
    throw e;
  }
};

test("the Windows provider's files do not exist and no list names them", () => {
  for (const f of ['WindowsLocationProvider.cpp', 'WindowsLocationProvider.h', 'WindowsLocationChild.cpp',
    'WindowsLocationChild.h', 'WindowsLocationParent.cpp', 'WindowsLocationParent.h',
    'WindowsLegacyLocationChild.cpp', 'WindowsLegacyLocationChild.h', 'WindowsRuntimeLocationChild.cpp',
    'WindowsRuntimeLocationChild.h', 'PWindowsLocation.ipdl', 'moz.build'])
    assert.ok(!fs.existsSync(path.join(TREE, 'dom/system/windows/location', f)), f);
  assert.ok(!fs.existsSync(path.join(TREE, 'dom/system/windows/location')));
  // No build file, IPDL list, jar or test manifest mentions the directory (mots.yaml is an ownership glob).
  const lists = gitGrep(['-n', 'windows/location', '--', '*.build', '*.mozbuild', '*.ipdl', '*.ipdlh',
    '*.toml', '*.ini', '*.mn', '*.conf', '*.py', '*.txt', '*.json', '*.yaml', ':!mots.yaml', ':!juggler/tests/*']);
  assert.equal(lists, '');
  for (const rel of ['dom/system/windows/moz.build', 'toolkit/components/geolocation/moz.build'])
    assert.doesNotMatch(read(rel), /windows\/location|DIRS \+= \["location"\]|MOZ_WIDGET_TOOLKIT"\] == "windows"/, rel);
});

test('no source names the provider or its actors, outside the dead-name leaves', () => {
  const out = gitGrep(['-lE', 'WindowsLocationProvider|PWindowsLocation|WindowsLocation(Child|Parent)|WindowsRuntimeLocation|WindowsLegacyLocation|getChromeWindowsLocation|OpenWindowsLocationSettings',
    '--', '.', ':!juggler/tests/*']);
  for (const rel of out.trim().split('\n').filter(Boolean))
    assert.ok(DEAD_NAME_LEAVES.has(rel), rel + ' still names the Windows provider');
  // The two Mozilla files that built it no longer do.
  assert.doesNotMatch(read(SERVICE), /WindowsLocation|ms-windows-location|#ifdef XP_WIN/);
  assert.doesNotMatch(read('dom/system/windows/WindowsUtilsChild.h'), /Location/);
  assert.doesNotMatch(read('dom/system/windows/PWindowsUtils.ipdl'), /Location|manages/);
});

test('the WINDOWS_UTILS process kind has no caller left (its definition may remain)', () => {
  const out = gitGrep(['-nE', 'GetWindowsUtilsPromise|ReleaseWindowsUtils', '--', '*.cpp', '*.h', '*.mm', '*.js', '*.mjs', ':!juggler/tests/*']);
  for (const line of out.trim().split('\n').filter(Boolean))
    assert.match(line, /^ipc\/glue\/UtilityProcessManager\.(cpp|h):/, line);
  // Inside the manager, the only uses are the definitions themselves.
  const cpp = read('ipc/glue/UtilityProcessManager.cpp');
  const calls = cpp.split('\n').filter(l => /GetWindowsUtilsPromise|ReleaseWindowsUtils/.test(l) &&
    !/^UtilityProcessManager::GetWindowsUtilsPromise\(\) \{|^void UtilityProcessManager::ReleaseWindowsUtils|"UtilityProcessManager::GetWindowsUtilsPromise"|LaunchError\("GetWindowsUtilsPromise|PWindowsUtils|WindowsUtilsPromise>/.test(l));
  assert.deepEqual(calls, []);
});

test('the own file picks the network provider by contract and reads neither a pref nor the host', () => {
  const cpp = read(OWN_CPP);
  const header = read(OWN_H);
  assert.match(cpp, /bool StealthSelectGeolocationProvider\(\s*nsCOMPtr<nsIGeolocationProvider>& aProvider\)/);
  assert.match(cpp, /do_GetService\(StealthGeolocationProviderContract\(\)\)/);
  assert.match(cpp, /return true;/);
  assert.match(header, /const char\* StealthGeolocationProviderContract\(\);/);
  assert.match(cpp, /return "@mozilla\.org\/geolocation\/provider;1";/);
  for (const bad of [/Preferences::/, /StaticPrefs::/, /WindowsLocationProvider/, /ms-windows-location/,
    /XP_WIN/, /MOZ_WIDGET/, /WinRT|Geolocator|wlanapi/i])
    assert.doesNotMatch(cpp, bad);
});

test('the own file is registered in the build by lines after the closing bracket, marked STEALTH-HOOK', () => {
  const build = read('toolkit/components/geolocation/moz.build');
  assert.ok(build.includes(
    `EXPORTS.mozilla += [\n    "GeolocationService.h",\n]\n` +
    `EXPORTS.mozilla += ["StealthGeolocation.h"]  # STEALTH-HOOK(${FOLDER})\n`));
  assert.ok(build.includes(
    `UNIFIED_SOURCES += ["StealthGeolocation.cpp"]  # STEALTH-HOOK(${FOLDER})\n`));
  assert.ok(build.includes(`TEST_DIRS += ["test/gtest"]  # STEALTH-HOOK(${FOLDER})\n`));
  const gtestBuild = read('toolkit/components/geolocation/test/gtest/moz.build');
  assert.ok(gtestBuild.includes('UNIFIED_SOURCES += ["TestStealthGeolocation.cpp"]'));
  assert.ok(gtestBuild.includes('FINAL_LIBRARY = "xul-gtest"'));
});

test("the gtest row is in the suite the candidate's filter runs (StealthDeclaredNetwork)", () => {
  const gtest = read('toolkit/components/geolocation/test/gtest/TestStealthGeolocation.cpp');
  assert.match(gtest, /TEST\(StealthDeclaredNetwork, GeolocationProviderIsTheNetworkProviderOnEveryOS\)/);
  assert.match(gtest, /StealthGeolocationProviderContract\(\)/);
  assert.match(gtest, /NS_GEOLOCATION_PROVIDER_CONTRACTID/);
});
