// moz.build refuses a list that is not in order ("UnsortedError ... We expected X but got Y") and the build dies in
// the first seconds of the configure step, on every platform. mozbuild's StrictOrderingOnAppendList sorts without
// regard to case, and "/" comes before every letter: "content/x" goes before "ContentMux.js".
//
// This reads every moz.build of juggler/ and checks each `+= [ ... ]` list with that rule.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

function mozBuildFiles(dir) {
  const found = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory())
      found.push(...mozBuildFiles(full));
    else if (entry.name === 'moz.build')
      found.push(full);
  }
  return found;
}

// The lists of a moz.build: [name, [items]] for each `NAME += [ "a", "b" ]`.
function lists(source) {
  const text = source.replace(/#[^\n]*/g, '');
  const found = [];
  for (const m of text.matchAll(/([A-Za-z_][\w.]*)\s*\+=\s*\[([\s\S]*?)\]/g))
    found.push([m[1], Array.from(m[2].matchAll(/"([^"]*)"|'([^']*)'/g), x => x[1] ?? x[2])]);
  return found;
}

// Where a list stops being in order, as mozbuild says it: [expected, got], or null.
function firstDisorder(items) {
  const sorted = [...items].sort((a, b) => (a.toLowerCase() < b.toLowerCase() ? -1 : a.toLowerCase() > b.toLowerCase() ? 1 : 0));
  for (let i = 0; i < items.length; i++) {
    if (items[i] !== sorted[i])
      return [sorted[i], items[i]];
  }
  return null;
}

test('the rule finds the disorder mozbuild found: a file of a folder goes before "ContentMux.js"', () => {
  assert.deepEqual(firstDisorder(['ChannelEventSink.sys.mjs', 'ContentMux.js', 'content/JugglerFrameChild.jsm']),
    ['content/JugglerFrameChild.jsm', 'ContentMux.js']);
  assert.equal(firstDisorder(['ChannelEventSink.sys.mjs', 'content/JugglerFrameChild.jsm', 'ContentMux.js']), null);
  assert.equal(firstDisorder(['a.js']), null);
});

test('the lists parsed are the ones of the file: there are some, and they hold files', () => {
  const source = fs.readFileSync(path.join(JUGGLER, 'moz.build'), 'utf8');
  const all = lists(source);
  assert.ok(all.length >= 4, 'the lists of juggler/moz.build are read');
  assert.ok(all.some(([name, items]) => name === 'FINAL_TARGET_FILES.modules.juggler' && items.includes('ContentMux.js')));
});

test('every list of every moz.build of juggler/ is in the order mozbuild wants', () => {
  const files = mozBuildFiles(JUGGLER);
  assert.ok(files.length >= 1, 'a moz.build is found');
  for (const file of files) {
    for (const [name, items] of lists(fs.readFileSync(file, 'utf8'))) {
      const disorder = firstDisorder(items);
      assert.equal(disorder, null,
        `${path.relative(JUGGLER, file)} ${name}: We expected "${disorder && disorder[0]}" but got "${disorder && disorder[1]}"`);
    }
  }
});
