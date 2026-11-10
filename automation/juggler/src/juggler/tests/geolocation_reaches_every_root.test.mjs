// The geolocation override is a field of the top browsing context that is not synchronized between processes, so
// each process that holds a root of the page must be told when the setting changes.
//
// The parent sends `applyContextSetting` through the mux; its route is `broadcast`: every root with an actor gets it
// (content/main.js sets the override on `.top` in each), and a root without one reads the shared settings when its
// process starts. A route to the main root alone would leave the iframe of another process with the old position.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');

// ContentMux.js is an ES module without Gecko in it: evaluated as a script, `export` dropped, as the other tests
// load Juggler's files (a bare import of a .js file in a package with no "type" needs a newer Node).
function load(file, scope, names) {
  const source = fs.readFileSync(path.join(ROOT, file), 'utf8').replace(/^export /gm, '');
  vm.createContext(scope);
  vm.runInContext(source + `
;globalThis.__names = { ${names.join(', ')} };`, scope);
  return scope.__names;
}
const { ContentMux, routeOf } = load('ContentMux.js', {}, ['ContentMux', 'routeOf']);

test('applyContextSetting is a broadcast', () => {
  assert.equal(routeOf('applyContextSetting', { name: 'geolocation', value: null }).kind, 'broadcast');
});

test('a setting changed after two roots have an actor reaches both; a root without an actor is skipped', async () => {
  const received = [];
  const channel = rootId => ({
    bindToActor() {},
    connect: namespace => ({ send: async (method, params) => { received.push([rootId, namespace, method, params.name]); } }),
    register() {},
    unregister() {},
  });
  let n = 0;
  const mux = new ContentMux({
    pageTarget: { linkedBrowser: () => ({ browsingContext: { id: 1 } }) },
    name: 'page',
    createChannel: () => channel(++n),
  });
  mux.onRootAdded(1, { parent: null });
  mux.bindRoot(1, {}, { isMain: true });
  mux.onRootAdded(2, { parent: {} });
  mux.bindRoot(2, {}, { isMain: false });
  mux.onRootAdded(3, { parent: {} });   // its process has no actor yet
  await mux.connect('').send('applyContextSetting', { name: 'geolocation', value: { latitude: 1 } });
  assert.deepEqual(received.map(r => r[0]).sort(), [1, 2]);
  assert.ok(received.every(r => r[2] === 'applyContextSetting' && r[3] === 'geolocation'));
});
