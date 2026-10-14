// Juggler is not a chrome package, and every URL that loads it names a file the
// build installs.
//
// A page can tell whether the browser registers a chrome package of a given
// name: the parent sends every registered package to each content process, and
// `new Worker("chrome://<name>/...")` returns for a registered name and throws
// SecurityError at once for an absent one. Until firefox-47 every build
// registered `juggler` (jar.mn, `% content juggler %content/`), and from
// 2026-10-06 captcha vendors probed it: hCaptcha's hsw.js runs
// `new Worker("chrome://juggler/content/content/WorkerMain.js")` (camoufox
// #847), and a solved challenge was refused on our build while retail passed
// (invisible_playwright #336). Measured from a page on firefox-46, 2026-10-14:
// of the 65 chrome packages and resource substitutions the build registers,
// `juggler` was the one retail does not.
//
// Juggler now loads from resource://gre/modules/juggler/, a namespace every
// Firefox registers, inside which a page cannot tell which files exist. This
// test bounds both sides: nothing registers or names the chrome package, and
// each resource URL in the tree is one moz.build installs, so a typo is caught
// here and not as a browser that cannot be driven.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const TREE = path.join(JUGGLER, '..');
const PREFIX = 'resource://gre/modules/juggler/';
// Outside juggler/, the files that load a Juggler module.
const LOADERS_OUTSIDE = [
  'toolkit/components/telemetry/app/TelemetrySend.sys.mjs',
  'toolkit/components/telemetry/pings/BackgroundTask_pingsender.sys.mjs',
];

function sources() {
  const out = [];
  const walk = dir => {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
      const p = path.join(dir, e.name);
      if (e.isDirectory()) {
        if (e.name !== 'tests')
          walk(p);
      } else if (/\.(js|mjs|jsm|conf|css|build|mn)$/.test(e.name)) {
        out.push(p);
      }
    }
  };
  walk(JUGGLER);
  return out.concat(LOADERS_OUTSIDE.map(f => path.join(TREE, f)));
}

// The paths under modules/juggler/ that juggler/moz.build installs.
function installed() {
  const build = fs.readFileSync(path.join(JUGGLER, 'moz.build'), 'utf8');
  const files = new Set();
  const re = /FINAL_TARGET_FILES\.modules\.juggler((?:\.\w+)*)\s*\+=\s*\[([^\]]*)\]/g;
  for (const [, sub, list] of build.matchAll(re)) {
    const dir = sub.split('.').filter(Boolean).join('/');
    for (const [, src] of list.matchAll(/"([^"]+)"/g))
      files.add((dir ? dir + '/' : '') + path.posix.basename(src));
  }
  return files;
}

test('no chrome package is registered for the juggler', () => {
  assert.equal(fs.existsSync(path.join(JUGGLER, 'jar.mn')), false, 'juggler/jar.mn is back');
  const build = fs.readFileSync(path.join(JUGGLER, 'moz.build'), 'utf8');
  assert.doesNotMatch(build, /^\s*JAR_MANIFESTS/m);
  const manifest = fs.readFileSync(path.join(TREE, 'browser/installer/package-manifest.in'), 'utf8');
  assert.doesNotMatch(manifest, /chrome\/juggler/);
});

test('no source names a chrome://juggler URL', () => {
  const offenders = sources().filter(f => fs.readFileSync(f, 'utf8').includes('chrome://juggler'));
  assert.deepEqual(offenders.map(f => path.relative(TREE, f)), []);
});

test('every juggler resource URL names a file the build installs', () => {
  const files = installed();
  // Near-miss guard: the parser read the lists, so an empty set is a broken
  // test and not a pass.
  assert.ok(files.has('components/Juggler.js') && files.has('content/WorkerMain.js'),
            [...files].join(' '));
  const missing = [];
  let seen = 0;
  for (const f of sources()) {
    const text = fs.readFileSync(f, 'utf8');
    for (const [, rel] of text.matchAll(/resource:\/\/gre\/modules\/juggler\/([\w./-]+)/g)) {
      if (rel.endsWith('/'))
        continue;
      seen++;
      if (!files.has(rel))
        missing.push(`${path.relative(TREE, f)}: ${PREFIX}${rel}`);
    }
  }
  assert.ok(seen >= 40, `only ${seen} juggler URLs found: the scan is not reading the tree`);
  assert.deepEqual(missing, []);
});

test('the test fails on the layout it replaces', () => {
  // The parser on a moz.build that installs nothing finds nothing, and a URL to
  // a file not in the lists is reported: the two assertions above can fire.
  const re = /FINAL_TARGET_FILES\.modules\.juggler((?:\.\w+)*)\s*\+=\s*\[([^\]]*)\]/g;
  assert.equal([...'JAR_MANIFESTS += ["jar.mn"]'.matchAll(re)].length, 0);
  assert.equal(installed().has('content/NoSuchFile.js'), false);
});
