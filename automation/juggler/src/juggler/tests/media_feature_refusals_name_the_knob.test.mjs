// The refusals of the media-feature commands, loaded the way the parent loads PageHandler.js and BrowserHandler.js.
//
// reduced motion, forced colors and contrast are declared by invisible_core as prefs; the protocol commands that used
// to override them refuse, and the refusal names the real knob. Page.setEmulatedMedia built that message from a
// variable that does not exist (`imposti`, the list is `imposed`), so a client that sent reducedMotion, forcedColors or
// contrast got "ReferenceError: imposti is not defined" and no hint of what to do. The messages of both refusals
// were also in Italian; a client reads them, so they are English like the rest of the protocol's errors.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `\n;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}

const anything = new Proxy(function () {}, { get: () => class {} });
const scope = () => ({
  Components: { classes: {}, interfaces: {}, utils: {}, results: {} },
  ChromeUtils: { importESModule: () => anything },
  dump: () => {},
});
const { PageHandler } = load('protocol/PageHandler.js', scope(), ['PageHandler']);
const { BrowserHandler } = load('protocol/BrowserHandler.js', scope(), ['BrowserHandler']);

function pageHandler() {
  const calls = [];
  const handler = Object.create(PageHandler.prototype);
  handler._pageTarget = {
    setColorScheme: value => calls.push(['setColorScheme', value]),
    setEmulatedMedia: value => calls.push(['setEmulatedMedia', value]),
  };
  return { handler, calls };
}

test('Page.setEmulatedMedia refuses reducedMotion, forcedColors and contrast and names them', async () => {
  const { handler, calls } = pageHandler();
  await assert.rejects(
      handler['Page.setEmulatedMedia']({ reducedMotion: 'reduce', forcedColors: 'active', contrast: 'more' }),
      error => {
        assert.ok(!(error instanceof ReferenceError), 'the refusal is built from a variable that exists: ' + error);
        assert.match(error.message, /^Page\.setEmulatedMedia: reducedMotion, forcedColors, contrast cannot be set from here/);
        assert.match(error.message, /invisible_core declares them in the profile/);
        return true;
      });
  assert.deepEqual(calls, [], 'a refused command changes nothing');
});

test('Page.setEmulatedMedia names only what was imposed', async () => {
  const { handler } = pageHandler();
  await assert.rejects(handler['Page.setEmulatedMedia']({ forcedColors: 'active' }),
      /^Error: Page\.setEmulatedMedia: forcedColors cannot be set from here/);
});

test('Page.setEmulatedMedia still sets the color scheme and the media type', async () => {
  const { handler, calls } = pageHandler();
  await handler['Page.setEmulatedMedia']({ colorScheme: 'dark', type: 'print', reducedMotion: null, forcedColors: '', contrast: undefined });
  assert.deepEqual(calls, [['setColorScheme', 'dark'], ['setEmulatedMedia', 'print']]);
});

test('the Browser.* media-feature commands refuse in English and accept the neutral value', async () => {
  const handler = Object.create(BrowserHandler.prototype);
  await assert.rejects(handler['Browser.setReducedMotion']({ reducedMotion: 'reduce' }), error => {
    assert.match(error.message, /^Browser\.setReducedMotion: 'reduce' cannot be set from the protocol\./);
    assert.match(error.message, /the only accepted value is 'no-preference'/);
    assert.match(error.message, /declare the preference in invisible_core/);
    return true;
  });
  await assert.rejects(handler['Browser.setForcedColors']({ forcedColors: 'active' }), /only accepted value is 'none'/);
  await assert.rejects(handler['Browser.setContrast']({ contrast: 'more' }), /only accepted value is 'no-preference'/);
  await handler['Browser.setReducedMotion']({ reducedMotion: 'no-preference' });
  await handler['Browser.setForcedColors']({ forcedColors: null });
  await handler['Browser.setContrast']({});
});

test('no error message of the protocol handlers is in Italian', () => {
  for (const file of ['protocol/PageHandler.js', 'protocol/BrowserHandler.js']) {
    const source = fs.readFileSync(path.join(ROOT, file), 'utf8');
    const messages = [...source.matchAll(/new Error\(([\s\S]*?)\);/g)].map(match => match[1]).join('\n');
    assert.doesNotMatch(messages, /\b(non|si|nel|dal|dichiara|rifiuta|imposta|impostano)\b/, file);
  }
});
