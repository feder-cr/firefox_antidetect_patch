// The back-forward cache goes by fragment root (firefox-62, Fission, step 8.3).
//
// A document that goes into the cache (`pagehide` with `persisted`) detaches its end of the channel, and one that
// comes back (`pageshow` with `persisted`) asks the parent to bind it again. That was said of the top only
// (`browsingContext.parent` was null); a frame whose parent is in another process is the top of its own part of the
// page and goes into the cache, and comes back, on its own. A frame whose parent is in the same process is served by
// the agents of its root and says nothing.
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

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci: {},
  ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);

const { JugglerFrameChild } = load('content/JugglerFrameChild.sys.mjs', {
  Components: { interfaces: {} },
  ChromeUtils: { importESModule: url => (url.endsWith('Helper.js') ? { Helper } : { initialize: () => ({}) }) },
  JSWindowActorChild: class {},
}, ['JugglerFrameChild']);

// A document in a browsing context: `parent` is null (the top), a parent with a docShell (this process) or without
// one (another process).
function frame(parent) {
  const document = {};
  const actor = new JugglerFrameChild();
  const agents = { channel: { detached: 0, isReady: () => true, detachFromPeer() { this.detached++; } }, pageAgent: { enterCache() {} } };
  actor._agents = () => agents;
  actor.sent = [];
  actor.sendAsyncMessage = (name, obj) => actor.sent.push(name);
  Object.defineProperty(actor, 'browsingContext', { value: { parent } });
  Object.defineProperty(actor, 'document', { value: document });
  return { actor, agents, document };
}

const top = null;
const inAnotherProcess = { docShell: null };
const inThisProcess = { docShell: {} };
const event = (type, target, persisted = true) => ({ type, persisted, target });

test('the top goes into the cache and comes back', () => {
  const { actor, agents, document } = frame(top);
  actor.handleEvent(event('pagehide', document));
  assert.equal(agents.channel.detached, 1);
  actor.handleEvent(event('pageshow', document));
  assert.deepEqual(Array.from(actor.sent), ['juggler:became-current']);
});

test('a frame whose parent is in another process goes into the cache and comes back as well', () => {
  const { actor, agents, document } = frame(inAnotherProcess);
  actor.handleEvent(event('pagehide', document));
  assert.equal(agents.channel.detached, 1);
  actor.handleEvent(event('pageshow', document));
  assert.deepEqual(Array.from(actor.sent), ['juggler:became-current']);
});

test('a frame whose parent is in this process says nothing: its root does', () => {
  const { actor, agents, document } = frame(inThisProcess);
  actor.handleEvent(event('pagehide', document));
  actor.handleEvent(event('pageshow', document));
  assert.equal(agents.channel.detached, 0);
  assert.deepEqual(Array.from(actor.sent), []);
});

test('a page that is not persisted, or an event of another document, is not the cache', () => {
  const { actor, agents, document } = frame(inAnotherProcess);
  actor.handleEvent(event('pagehide', document, false));
  actor.handleEvent(event('pageshow', document, false));
  actor.handleEvent(event('pagehide', {}));
  actor.handleEvent(event('pageshow', {}));
  assert.equal(agents.channel.detached, 0);
  assert.deepEqual(Array.from(actor.sent), []);
});
