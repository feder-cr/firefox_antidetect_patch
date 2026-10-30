// Runtime.js asks for the debugger module where Firefox 159 serves it.
//
// 159 moved devtools/platform/jsdebugger.sys.mjs to moz-src:/// (it is listed
// under MOZ_SRC_FILES in devtools/platform/moz.build); the resource://gre/modules
// address of 157 is gone, so a content process without a Debugger of its own
// threw at load and no page ever got a Runtime.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const SOURCE = fs.readFileSync(
  path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'content', 'Runtime.js'), 'utf8');

test('a process without a Debugger imports the module from its 159 address', () => {
  const asked = [];
  const added = [];
  const scope = {
    ChromeUtils: {
      importESModule: url => {
        asked.push(url);
        return { addDebuggerToGlobal: global => added.push(global) };
      },
    },
    Components: { utils: { getGlobalForObject: g => g } },
  };
  vm.createContext(scope);
  vm.runInContext(SOURCE, scope);
  assert.deepEqual(asked, ['moz-src:///devtools/platform/jsdebugger.sys.mjs']);
  assert.equal(added.length, 1);
});
