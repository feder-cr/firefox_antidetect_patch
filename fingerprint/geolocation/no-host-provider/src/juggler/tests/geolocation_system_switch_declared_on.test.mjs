// The persona is a Windows machine whose system location is declared ON. Firefox
// asked the OS: GetGeolocationPermissionBehavior (GeolocationSystemWin.cpp) reads
// the privacy switch through AppCapability("location").CheckAccess() on every
// request (capabilityaccessmanagerclient.dll is loaded in the parent for it), and
// with the OS switch off the request was cancelled with PERMISSION_DENIED
// (Geolocation.cpp nsGeolocationRequest::Allow, while
// dom.geolocation.require_system_permission.enabled is true). A page must see the
// declared answer, not the switch of the machine the browser runs on.
//
// The answer is given where Firefox reads the host: the first statement of
// GetGeolocationPermissionBehavior returns NoPrompt (the behavior the generic
// GeolocationSystem.cpp already has on Linux) before any AppCapability call, and
// Mozilla's body stays intact below it. The OS is read a second time, by the
// monitor navigator.permissions.query starts in the parent
// (PermissionObserver::EnsureMonitoringInParent -> the Windows permission monitor,
// AppCapability::Create + an access-changed subscription, which is what loads
// capabilityaccessmanagerclient.dll after the query): with the switch declared on
// it never changes, so the monitor is not started. The acceptance contract for the
// end-to-end is at the top of geolocation_windows_provider_never_selected.test.mjs.
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
const WIN = 'dom/geolocation/GeolocationSystemWin.cpp';
const FOLDER = 'fingerprint/geolocation/no-host-provider';

const win = read(WIN);
const fn = win.slice(win.indexOf('SystemGeolocationPermissionBehavior GetGeolocationPermissionBehavior() {'),
  win.indexOf('already_AddRefed<SystemGeolocationPermissionRequest>\nRequestLocationPermissionFromUser'));

test('the first statement of GetGeolocationPermissionBehavior answers the declared switch, before any OS read', () => {
  assert.ok(win.includes(
    `#include "mozilla/StealthGeolocation.h"  // STEALTH-HOOK(${FOLDER})\n`));
  assert.ok(fn.startsWith(
    'SystemGeolocationPermissionBehavior GetGeolocationPermissionBehavior() {\n' +
    `  // STEALTH-HOOK(${FOLDER}): the OS location switch is declared on; the OS is not asked.\n` +
    '  if (StealthSystemLocationDeclaredOn()) {\n' +
    '    return SystemGeolocationPermissionBehavior::NoPrompt;\n' +
    '  }\n'));
  assert.ok(fn.indexOf('StealthSystemLocationDeclaredOn()') < fn.indexOf('GetLocationAccess()'));
  assert.equal(win.split('\n').filter(l => l.includes('STEALTH-HOOK')).length, 2);
});

test("Mozilla's body of the function is intact below the hook (no line of Mozilla removed)", () => {
  assert.ok(fn.includes(
    '  auto locationAccess = GetLocationAccess();\n' +
    '  if (SystemWillPromptForPermissionHint(locationAccess)) {\n' +
    '    return SystemGeolocationPermissionBehavior::SystemWillPromptUser;\n' +
    '  }\n' +
    '  if (!LocationIsPermittedHint(locationAccess)) {\n' +
    '    return SystemGeolocationPermissionBehavior::GeckoWillPromptUser;\n' +
    '  }\n' +
    '  return SystemGeolocationPermissionBehavior::NoPrompt;\n' +
    '}\n'));
});

test('the declaration is one pure function of our own file, with no OS call', () => {
  const cpp = read('toolkit/components/geolocation/StealthGeolocation.cpp');
  const header = read('toolkit/components/geolocation/StealthGeolocation.h');
  assert.match(header, /bool StealthSystemLocationDeclaredOn\(\);/);
  assert.match(cpp, /bool StealthSystemLocationDeclaredOn\(\) \{\s*return true;\s*\}/);
  assert.doesNotMatch(cpp, /AppCapability|CheckAccess|GetLocationAccess|XP_WIN/);
});

test('AppCapability is read in three places only: the function above, the monitor of the hook below, and a comment', () => {
  const out = execFileSync('git', ['grep', '-lE', 'AppCapability|capabilityaccessmanager',
    '--', '*.cpp', '*.h', '*.mm', '*.rs', ':!third_party/*', ':!build/*', ':!toolkit/components/geolocation/*'],
  { cwd: TREE, encoding: 'utf8' });
  assert.deepEqual(out.trim().split('\n'), [
    WIN,
    'dom/permission/PermissionObserver.cpp',   // a comment; the monitor is started by the Rust component below
    'widget/windows/rust/src/permission_monitor.rs',
  ]);
  // RequestLocationPermissionFromUser (which reads the OS again) is called only for a behavior other than NoPrompt.
  const geo = read('dom/geolocation/Geolocation.cpp');
  assert.ok(geo.includes('if (mBehavior != SystemGeolocationPermissionBehavior::NoPrompt) {'));
  assert.ok(geo.includes('case geolocation::SystemGeolocationPermissionBehavior::NoPrompt:'));
});

test("the permission monitor of the OS switch is not started: an early return before Mozilla's call", () => {
  const rel = 'dom/permission/PermissionObserver.cpp';
  const obs = read(rel);
  assert.ok(obs.includes(
    `#include "mozilla/StealthGeolocation.h"  // STEALTH-HOOK(${FOLDER})\n`));
  assert.ok(obs.includes(
    'void PermissionObserver::EnsureMonitoringInParent(PermissionName aName) {\n' +
    '  MOZ_ASSERT(XRE_IsParentProcess());\n' +
    '  if (aName == PermissionName::Geolocation &&\n' +
    `      StealthSystemLocationDeclaredOn()) {  // STEALTH-HOOK(${FOLDER}): declared on, never changes\n` +
    '    return;\n' +
    '  }\n' +
    '  EnsureOSMonitoring(aName);\n' +
    '}\n'));
  assert.equal(obs.split('\n').filter(l => l.includes('STEALTH-HOOK')).length, 2);
  // The one caller, and the one place that creates the monitor.
  const out = execFileSync('git', ['grep', '-nE', 'EnsureMonitoringInParent|permission-monitor;1', '--', '*.cpp', '*.h', '*.mm'],
    { cwd: TREE, encoding: 'utf8' });
  assert.deepEqual(out.trim().split('\n').map(l => l.split(':')[0]).sort(), [
    'dom/ipc/ContentParent.cpp', rel, rel, 'dom/permission/PermissionObserver.h',
  ].sort());
});

test("the gtest row is in the suite the candidate's filter runs (StealthDeclaredNetwork)", () => {
  const gtest = read('toolkit/components/geolocation/test/gtest/TestStealthGeolocation.cpp');
  assert.match(gtest, /TEST\(StealthDeclaredNetwork, TheSystemLocationSwitchIsDeclaredOn\)/);
  assert.match(gtest, /EXPECT_TRUE\(mozilla::StealthSystemLocationDeclaredOn\(\)\)/);
});
