// Whether scripts run in a page is written by the parent, on the page's top-level browsing context.
//
// With sites isolated an iframe's document lives in another process. The content process of the top wrote
// `allowJavascript` on the WindowContext of each document of its own frame tree, and that field is
// owner-or-parent-only: the write on a document of another process is refused, so the iframe of a page with
// scripts disabled ran its scripts. The BrowsingContext's AllowJavascript is parent-only, synchronized to every
// process and inherited by the contexts born under it, so the parent writes it once and it holds for the
// iframes of any process, those created later, and the documents' first scripts (they compute it at birth).
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';
import { initializeRoot } from './_content_main.mjs';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = rel => fs.readFileSync(path.join(ROOT, rel), 'utf8');

function load(file, scope, names) {
  const source = read(file).replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

// Whatever the file imports is an empty class: only PageTarget's own methods run here.
const anything = new Proxy(function () {}, { get: () => class {} });
const { PageTarget } = load('TargetRegistry.js', {
  Components: { classes: {}, interfaces: {}, utils: {}, results: {} },
  ChromeUtils: { importESModule: () => anything },
  dump: () => {},
}, ['PageTarget']);

// A browsing context that records the writes to allowJavascript.
function context(allowJavascript = true) {
  const writes = [];
  let value = allowJavascript;
  return {
    writes,
    get allowJavascript() { return value; },
    set allowJavascript(v) { writes.push(v); value = v; },
  };
}

// A target with the fields the setting uses; `sent` is what it sends to the content processes.
function target({ settings = {}, top = context() } = {}) {
  const t = Object.create(PageTarget.prototype);
  t._browserContext = { crossProcessCookie: { settings } };
  t._linkedBrowser = { browsingContext: top };
  t.sent = [];
  t._contentMux = {
    connect: () => ({ send: async (method, params) => { t.sent.push([method, JSON.parse(JSON.stringify(params))]); } }),
    onRootAdded() {},
    bindRoot() {},
  };
  t.nextActorSequenceNumber = () => 1;
  t._targetId = 't';
  return t;
}

test('the context\'s setting is written on the top-level browsing context', () => {
  const top = context(true);
  const t = target({ settings: { javaScriptDisabled: true }, top });
  t.updateJavaScriptDisabled();
  assert.equal(top.allowJavascript, false);
  t._browserContext.crossProcessCookie.settings.javaScriptDisabled = undefined;
  t.updateJavaScriptDisabled();
  assert.equal(top.allowJavascript, true);
});

test('a setting that is already in place is not written again', () => {
  const top = context(true);
  const t = target({ top });
  t.updateJavaScriptDisabled();
  t.updateJavaScriptDisabled();
  assert.deepEqual(top.writes, []);
});

test('the browsing context given is the one written (a tab at its creation)', () => {
  const top = context(true), other = context(true);
  const t = target({ settings: { javaScriptDisabled: true }, top });
  t.updateJavaScriptDisabled(other);
  assert.deepEqual([other.allowJavascript, top.allowJavascript], [false, true]);
});

test('a new page has it before its first navigation: updateOverridesForBrowsingContext writes it', () => {
  const top = context(true);
  const t = target({ settings: { javaScriptDisabled: true }, top });
  for (const name of Object.getOwnPropertyNames(PageTarget.prototype)) {
    if (/^update(?!JavaScriptDisabled|OverridesForBrowsingContext)/.test(name))
      t[name] = () => {};
  }
  t.updateOverridesForBrowsingContext(top);
  assert.equal(top.allowJavascript, false);
});

test('Browser.setJavaScriptDisabled reaches the page without a message to the content processes', async () => {
  const top = context(true);
  const t = target({ top });
  t._browserContext.crossProcessCookie.settings.javaScriptDisabled = true;   // BrowserContext.applySetting stores it first
  await t.applyContextSetting('javaScriptDisabled', true);
  assert.equal(top.allowJavascript, false);
  assert.deepEqual(t.sent, []);
  t._browserContext.crossProcessCookie.settings.javaScriptDisabled = undefined;
  await t.applyContextSetting('javaScriptDisabled', undefined);
  assert.equal(top.allowJavascript, true);
});

test('the other settings still go to the content processes', async () => {
  const t = target();
  await t.applyContextSetting('bypassCSP', true);
  assert.deepEqual(t.sent, [['applyContextSetting', { name: 'bypassCSP', value: true }]]);
});

test('an actor that binds finds the setting in place (a page restored from the back-forward cache)', () => {
  const top = context(true);
  const t = target({ settings: { javaScriptDisabled: true }, top });
  t.setActor({ browsingContext: { id: 5, browserId: 1, parent: null } }, 5);
  assert.equal(top.allowJavascript, false);
});

test('the content process no longer writes it, and does not mind being told', () => {
  for (const rel of ['content/FrameTree.js', 'content/main.js'])
    assert.doesNotMatch(read(rel).replace(/\/\/[^\n]*/g, ''), /allowJavascript|setJavaScriptDisabled|_javaScriptDisabled/, rel);
  const bc = { id: 3, parent: null, browserId: 7, originAttributes: { userContextId: 0 } };
  bc.top = bc;
  const { handlers } = initializeRoot({ browsingContext: bc, settings: { javaScriptDisabled: true } });
  assert.doesNotThrow(() => handlers.applyContextSetting({ name: 'javaScriptDisabled', value: true }));
});
