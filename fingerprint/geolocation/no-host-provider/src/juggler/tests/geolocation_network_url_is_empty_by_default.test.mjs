// The engine has no network geolocation service URL by default, on every OS.
//
// Mozilla declares geo.provider.network.url in modules/libpref/init/all.js, in a
// block for every OS but Android and Windows, with a Google URL (and an API key
// placeholder). A browser that asked it would send the host's Wi-Fi/IP data to a
// third party for a position nobody asked for: the position a page reads is the
// core's declaration (geolocation override), never a lookup. The default is now ""
// on Mozilla's own line, with the hook, and the pref is never declared a second time:
//   - Linux and macOS: all.js declares it, with the value "".
//   - Windows: Mozilla declares nothing there; the pref has no value, and
//     formatURLPref() answers "about:blank" for it.
// Either way NetworkGeolocationProvider.sys.mjs finds no URL and takes the path
// "No network geolocation service URL is configured", the only one that remains.
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
const PREF = 'geo.provider.network.url';
const ALL = 'modules/libpref/init/all.js';
// git grep exits 1 when nothing matches.
const gitGrep = args => {
  try {
    return execFileSync('git', ['grep', ...args], { cwd: TREE, encoding: 'utf8' });
  } catch (e) {
    if (e.status === 1) return '';
    throw e;
  }
};

// The macros each OS defines for the preprocessor of all.js (the subset that all.js tests).
const OS = {
  windows: ['XP_WIN'],
  linux: ['XP_UNIX', 'MOZ_WIDGET_GTK'],
  macos: ['XP_UNIX', 'XP_MACOSX'],
};

// A small preprocessor: #if/#ifdef/#ifndef/#elif/#else/#endif on defined(X), !, &&, ||.
function activeLines(text, defines) {
  const set = new Set(defines);
  const cond = expr => {
    // `NAME == value` / `NAME != value` compare against the build variables below.
    const vars = { MOZ_UPDATE_CHANNEL: 'default' };
    const js = expr
      .replace(/(\w+)\s*(==|!=)\s*(\w+)/g, (_, a, op, b) => String(op === '==' ? vars[a] === b : vars[a] !== b))
      .replace(/defined\(\s*(\w+)\s*\)/g, (_, m) => (set.has(m) ? 'true' : 'false'));
    assert.match(js, /^[\s!()&|truefals]*$/, 'condition understood: ' + expr);
    return vm.runInNewContext(js);
  };
  const stack = [];  // { active, taken }
  const out = [];
  const on = () => stack.every(s => s.active);
  for (const raw of text.split('\n')) {
    const line = raw.trimStart();
    let m;
    if ((m = /^#\s*ifdef\s+(\w+)/.exec(line))) {
      const c = set.has(m[1]); stack.push({ active: c, taken: c });
    } else if ((m = /^#\s*ifndef\s+(\w+)/.exec(line))) {
      const c = !set.has(m[1]); stack.push({ active: c, taken: c });
    } else if ((m = /^#\s*if\s+(.*)$/.exec(line))) {
      const c = cond(m[1]); stack.push({ active: c, taken: c });
    } else if ((m = /^#\s*elif\s+(.*)$/.exec(line))) {
      const top = stack[stack.length - 1];
      top.active = !top.taken && cond(m[1]); top.taken ||= top.active;
    } else if (/^#\s*else\b/.test(line)) {
      const top = stack[stack.length - 1];
      top.active = !top.taken; top.taken = true;
    } else if (/^#\s*endif\b/.test(line)) {
      stack.pop();
    } else if (!line.startsWith("#") && on()) {
      out.push(raw);
    }
  }
  assert.equal(stack.length, 0, 'balanced preprocessor blocks');
  return out;
}

// The values all.js gives the pref on one OS (a list: more than one means declared twice).
function allJsValues(os) {
  const values = [];
  vm.runInNewContext(activeLines(read(ALL), OS[os]).join('\n'), {
    pref: (n, v) => { if (n === PREF) values.push(v); },
    sticky_pref: (n, v) => { if (n === PREF) values.push(v); },
    user_pref: () => {},
    lockPref: () => {},
    sticky: "sticky",
    locked: "locked",
  });
  return values;
}

test('the effective default is empty on Windows, Linux and macOS', () => {
  assert.deepEqual(allJsValues('linux'), ['']);
  assert.deepEqual(allJsValues('macos'), ['']);
  // Windows: Mozilla declares nothing, so there is nothing to read but the "no value" answer.
  assert.deepEqual(allJsValues('windows'), []);
});

test("the value is changed on Mozilla's own line, with the hook, and the line is one", () => {
  const lines = read(ALL).split('\n').filter(l => l.includes('"' + PREF + '"'));
  assert.equal(lines.length, 1);
  assert.match(lines[0], /^\s+pref\("geo\.provider\.network\.url", ""\);\s+\/\/ STEALTH-HOOK\(fingerprint\/geolocation\/no-host-provider\)/);
  assert.doesNotMatch(lines[0], /googleapis|GOOGLE_LOCATION_SERVICE_API_KEY/);
  // The two siblings in the same block stay as Mozilla wrote them.
  const text = read(ALL);
  assert.ok(text.includes('  pref("geo.provider.network.timeToWaitBeforeSending", 5000);\n'));
  assert.ok(text.includes('  pref("geo.provider.network.timeout", 60000);\n'));
});

test('nothing else declares or sets the pref (it is declared once)', () => {
  for (const rel of [
    'modules/libpref/init/StealthPrefs.js',
    'modules/libpref/init/StealthPrefList.yaml',
    'modules/libpref/init/StaticPrefList.yaml',
    'modules/libpref/greprefs.js',
    'browser/app/profile/firefox.js',
    'browser/branding/official/pref/firefox-branding.js',
    'browser/branding/unofficial/pref/firefox-branding.js',
  ]) {
    if (fs.existsSync(path.join(TREE, rel))) assert.ok(!read(rel).includes(PREF), rel);
  }
  // No source of ours writes it.
  const ours = execFileSync('git', ['ls-files', 'juggler', 'toolkit/components/geolocation', '--', ':!juggler/tests/*'],
    { cwd: TREE, encoding: 'utf8' }).trim().split('\n').filter(f => /\.(js|mjs|jsm|cpp|h)$/.test(f));
  assert.ok(ours.length > 10, 'our sources read');
  for (const rel of ours) assert.ok(!read(rel).includes(PREF), rel);
  // Outside tests, the only code that mentions it is Mozilla's two readers, the declaration
  // and the telemetry description; nobody writes it.
  const out = gitGrep(['-lF', PREF, '--', '*.cpp', '*.h', '*.js', '*.mjs', '*.sys.mjs', '*.yaml', '*.py',
    ':!*/test/*', ':!*/tests/*', ':!*test*.js', ':!testing/*', ':!remote/*', ':!third_party/*']);
  assert.deepEqual(out.trim().split('\n').sort(), [
    'dom/geolocation/metrics.yaml',
    'dom/system/NetworkGeolocationProvider.sys.mjs',
    ALL,
    'toolkit/modules/Region.sys.mjs',
  ].sort());
});

test('the network provider takes the "no URL configured" path for an empty or missing value', () => {
  const provider = read('dom/system/NetworkGeolocationProvider.sys.mjs');
  assert.ok(provider.includes('let url = Services.urlFormatter.formatURLPref("geo.provider.network.url");'));
  assert.ok(provider.includes('if (!url || url == "about:blank") {\n        throw new Error("No network geolocation service URL is configured");'));
  // formatURLPref answers about:blank for a pref without a value (Windows).
  assert.match(read('toolkit/components/urlformatter/URLFormatter.sys.mjs'),
    /catch \(ex\) \{\n\s+console\.error\("formatURLPref: Couldn't get pref: ", aPref\);\n\s+return "about:blank";/);
});
