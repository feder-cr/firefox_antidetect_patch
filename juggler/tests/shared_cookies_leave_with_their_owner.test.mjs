// A page's and a context's sharedData entries leave with them.
//
// TargetRegistry writes each page's settings into Services.ppmm.sharedData
// under its browserId, and each context's under its userContextId, for the
// content processes to read (content/main.js). Nothing ever deleted either,
// so every page and every context the browser ever had stayed in a map that
// is copied whole into every content process at each flush: measured on
// firefox-47 with an instrumented copy, 31 page entries after 30 pages opened
// and closed. Now the owner deletes its entry when it ends, under the key it
// wrote it with, and both sides take the key from Helper.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const code = rel => fs.readFileSync(path.join(JUGGLER, rel), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
const method = (source, signature) => {
  const start = source.indexOf(signature);
  assert.notEqual(start, -1, signature);
  return source.slice(start, source.indexOf('\n  }\n', start));
};

test('the two keys are named in Helper only', () => {
  for (const rel of ['TargetRegistry.js', 'content/main.js'])
    assert.doesNotMatch(code(rel), /juggler:(page|context)-cookie-/, rel);
  assert.match(code('Helper.js'), /pageCookieKey\(browserId\)/);
  assert.match(code('Helper.js'), /contextCookieKey\(userContextId\)/);
});

test('a page deletes its entry when it is disposed, under the key it wrote', () => {
  const registry = code('TargetRegistry.js');
  const pageTarget = registry.slice(registry.indexOf('export class PageTarget'), registry.indexOf('class BrowserContext'));
  assert.match(method(pageTarget, '  dispose() {'), /sharedData\.delete\(this\._crossProcessCookieKey\(\)\)/);
  assert.match(method(pageTarget, '  _crossProcessCookieKey() {'), /helper\.pageCookieKey\(this\._browserId\)/);
  assert.match(method(pageTarget, '  _updateCrossProcessCookie() {'), /sharedData\.set\(this\._crossProcessCookieKey\(\)/);
});

test('a context deletes its entry when it is removed', () => {
  const registry = code('TargetRegistry.js');
  const context = registry.slice(registry.indexOf('class BrowserContext'));
  assert.match(method(context, '  async destroy() {'), /sharedData\.delete\(this\._crossProcessCookieKey\(\)\)/);
  assert.match(method(context, '  _crossProcessCookieKey() {'), /helper\.contextCookieKey\(this\.userContextId\)/);
});
