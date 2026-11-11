// A root comes up whatever the context's cookie holds.
//
// Every fragment root of a page runs `initialize` for its own part (a process holds one for each root it has) and
// reads the context's settings from the shared cookie. The cookie holds settings that a root does not apply:
// javaScriptDisabled is written by the parent on the top browsing context (PageTarget.updateJavaScriptDisabled),
// and the client can name a setting an older content script does not know. `applySetting[name](value)` on such a
// name is a TypeError that ends `initialize`: the root has no channel and no agents, and its page waits for it.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { initializeRoot } from './_content_main.mjs';

const root = () => {
  const bc = { id: 3, parent: null, browserId: 7, originAttributes: { userContextId: 0 } };
  bc.top = bc;
  return bc;
};

test('a setting the root does not apply is skipped at initialize, the others are applied', () => {
  let tree;
  assert.doesNotThrow(() => {
    ({ tree } = initializeRoot({
      browsingContext: root(),
      settings: { somethingFromTheFuture: 1, bypassCSP: true },
    }));
  });
  assert.deepEqual(tree.docShellSettings.slice(0, 1), [{ bypassCSP: true }]);
});

test('a setting the root does not apply is skipped when the parent sends it, whatever the name', () => {
  const { handlers } = initializeRoot({ browsingContext: root() });
  for (const name of ['somethingFromTheFuture', 'toString', 'constructor', '__proto__'])
    assert.doesNotThrow(() => handlers.applyContextSetting({ name, value: true }), name);
});
