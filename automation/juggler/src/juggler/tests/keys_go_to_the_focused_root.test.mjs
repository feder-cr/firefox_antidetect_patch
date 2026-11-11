// A key goes to the process that holds the focused document of the page, not to the process of the top.
//
// The text input processor of a content process delivers a key to the documents of that process only: a key sent
// through the top's channel stops at the <iframe> element when the focus is inside an iframe that another
// process holds (one process per site). The page handler picks the root of the focused browsing context of its
// own page (design A of the step): the main root whenever the focus is not in this page's content.
//
// The method is the real one, taken out of PageHandler.js, run against fakes of the focus manager and the mux.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(JUGGLER, 'protocol/PageHandler.js'), 'utf8');
const strip = text => text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

function methodText(name) {
  const at = source.search(new RegExp('^  ' + name + '\\(', 'm'));
  assert.ok(at >= 0, name + ' is a method of PageHandler');
  const open = source.indexOf('{', source.indexOf(')', at));
  let depth = 0;
  for (let i = open; i < source.length; i++) {
    if (source[i] === '{') depth++;
    else if (source[i] === '}' && --depth === 0)
      return source.slice(at, i + 1);
  }
  throw new Error('unbalanced ' + name);
}

const TOP = {id: 10};                    // this page's top browsing context
const bc = (id, top) => ({id, top});
// `rootOf`: browsing context id -> root id (null: unknown); the focused context is what the focus manager says
function handler({focused, rootOf = {}, mainRootId = 10}) {
  const Services = {focus: {focusedContentBrowsingContext: focused}};
  const make = new Function('Services', 'return {\n' + methodText('_focusedRootId') + '\n};');
  const object = make(Services);
  object._pageTarget = {linkedBrowser: () => ({browsingContext: TOP})};
  object._contentChannel = {
    mainRootId: () => mainRootId,
    frameRootOf: context => (context.id in rootOf ? rootOf[context.id] : null),
  };
  return object;
}

test('the focus in an iframe held by another process: that iframe\'s root', () => {
  const frame = bc(21, TOP);
  assert.equal(handler({focused: frame, rootOf: {21: 21}})._focusedRootId(), 21);
});

test('the focus in an iframe of the top\'s process, or in the top: the main root', () => {
  assert.equal(handler({focused: bc(22, TOP), rootOf: {22: 10}})._focusedRootId(), 10);
  assert.equal(handler({focused: TOP, rootOf: {10: 10}})._focusedRootId(), 10);
});

test('the focus is not in this page\'s content: the main root', () => {
  // nothing focused in content (the address bar, say)
  assert.equal(handler({focused: null})._focusedRootId(), 10);
  // another tab or window: the focused context is the browser\'s, not the page\'s
  const elsewhere = bc(31, {id: 30});
  assert.equal(handler({focused: elsewhere, rootOf: {31: 31}})._focusedRootId(), 10);
});

test('a context whose root is not known yet (it is moving between processes): the main root', () => {
  assert.equal(handler({focused: bc(23, TOP), rootOf: {}})._focusedRootId(), 10);
});

test('the root is read at every key: the same context in a new root is followed', () => {
  const frame = bc(24, TOP);
  const rootOf = {24: 24};
  const h = handler({focused: frame, rootOf});
  assert.equal(h._focusedRootId(), 24);
  rootOf[24] = 50;                       // the iframe changed process with the focus in it
  assert.equal(h._focusedRootId(), 50);
});

test('keys and text are sent to that root, through the mux, inside the activation', () => {
  const code = strip(source);
  const key = code.slice(code.indexOf("['Page.dispatchKeyEvent']"));
  const keyTail = key.slice(key.indexOf('activateAndRun'), key.indexOf('activateAndRun') + 400);
  assert.match(keyTail, /this\._contentChannel\.sendTo\(this\._focusedRootId\(\), 'page', 'dispatchKeyEvent'/);
  const text = code.slice(code.indexOf("['Page.insertText']"));
  assert.match(text.slice(0, 300), /this\._contentChannel\.sendTo\(this\._focusedRootId\(\), 'page', 'insertText', options\)/);
  assert.doesNotMatch(code, /_contentPage\.send\('(dispatchKeyEvent|insertText)'/);
});
