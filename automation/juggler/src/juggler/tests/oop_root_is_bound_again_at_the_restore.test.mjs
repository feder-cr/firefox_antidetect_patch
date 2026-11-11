// A root whose parent frame is in another process (an out-of-process iframe) is bound again to the parent when its
// document comes back from the back-forward cache, and so says its workers again.
//
// The restored document asks to be bound again with `juggler:became-current` (JugglerFrameChild, on its pageshow).
// JugglerFrameParent answers it, but only "while the channel is not bound to this actor": SimpleChannel.bindToActor
// replaces the actor's `receiveMessage`, so a bound actor never reaches JugglerFrameParent. The top is unbound by the
// time it comes back (the document that replaced it took the channel), so its request arrived. An out-of-process
// iframe keeps its actor bound through its stay in the cache (no other document takes its place): its request went to
// the parent end of the channel as a channel message, was dropped, and the root stayed detached: no reattachment hook,
// no workers announced again. Measured on the engine: the child end logs its pageshow and never its reattachment.
//
// The channel ends here are the real ones, each over an actor with the wiring of the browser.
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
const plain = x => JSON.parse(JSON.stringify(x));
const settle = () => new Promise(resolve => setTimeout(resolve, 20));

const channelScope = () => ({ dump: () => {}, ChromeUtils: { importESModule: () => ({ setTimeout }) } });

// A JugglerFrameParent whose registry binds the root to the actor it is asked about, as PageTarget.setActor does
// through the mux.
function parentActorClass(registry) {
  const modules = {
    TargetRegistry: { TargetRegistry: { instance: () => registry } },
    Helper: { Helper: class {} },
  };
  const { JugglerFrameParent } = load('JugglerFrameParent.sys.mjs', {
    ChromeUtils: { importESModule: url => (url.endsWith('TargetRegistry.js') ? modules.TargetRegistry : modules.Helper) },
    JSWindowActorParent: class {},
  }, ['JugglerFrameParent']);
  return JugglerFrameParent;
}

// One root: the parent end of its channel and the content end, each bound to the actor of its side (the real
// bindToActor), the two actors wired to each other.
function root() {
  const { SimpleChannel: ParentChannel } = load('SimpleChannel.js', channelScope(), ['SimpleChannel']);
  const { SimpleChannel: ContentChannel } = load('SimpleChannel.js', channelScope(), ['SimpleChannel']);
  const parentEnd = new ParentChannel('browser::page[1]', 'target-1');
  const seen = [];
  parentEnd.register('page', { event: value => { seen.push(value); } });
  const contentEnd = new ContentChannel('', 'process-9');
  let reattached = 0;
  contentEnd.onReattached(() => { reattached++; });

  const registry = { bound: [], onActorCreated(actor) { this.bound.push(actor); parentEnd.bindToActor(actor); },
    targetForBrowserId: () => ({}) };
  const JugglerFrameParent = parentActorClass(registry);
  const parentActor = Object.assign(new JugglerFrameParent(), {
    actorName: 'browser::page[1]/1/5/1', _browserId: 1, _rootId: 5, manager: { isCurrentGlobal: true },
  });
  const contentActor = { actorName: 'content::1/5/1', receiveMessage() { throw new Error('the original receiveMessage'); } };
  // the browser delivers what an actor sends to the receiveMessage the other actor has AT THAT MOMENT
  contentActor.sendAsyncMessage = (name, data) => setImmediate(() => parentActor.receiveMessage({ name, data: plain(data) }));
  parentActor.sendAsyncMessage = (name, data) => setImmediate(() => contentActor.receiveMessage({ name, data: plain(data) }));

  parentEnd.bindToActor(parentActor);
  contentEnd.bindToActor(contentActor);
  return { parentEnd, contentEnd, parentActor, contentActor, registry, seen, reattached: () => reattached };
}

test('an out-of-process root whose actor stayed bound is bound again when its document asks, and is reattached', async () => {
  const r = root();
  await settle();
  assert.equal(r.contentEnd.isReady(), true);

  // pagehide with `persisted`: the content end stops sending, the parent end stays bound to the actor
  r.contentEnd.detachFromPeer();
  assert.equal(r.contentEnd.isReady(), false);

  // pageshow: the document asks to be bound again
  r.contentActor.sendAsyncMessage('juggler:became-current', {});
  await settle();

  assert.equal(r.registry.bound.length, 1, 'the registry bound the root again');
  assert.equal(r.reattached(), 1, 'the reattachment hook ran once');
  assert.equal(r.contentEnd.isReady(), true);
});

test('what the restored root says after the reattachment reaches the parent', async () => {
  const r = root();
  await settle();
  r.contentEnd.detachFromPeer();
  // said while detached: waits
  r.contentEnd.connect('page').emit('event', 'from the cache');
  await settle();
  assert.deepEqual(r.seen, []);
  r.contentActor.sendAsyncMessage('juggler:became-current', {});
  await settle();
  assert.deepEqual(plain(r.seen), ['from the cache']);
});

test('the top, whose actor was unbound by the document that replaced it, is bound again as before', async () => {
  const r = root();
  await settle();
  r.contentEnd.detachFromPeer();
  // the next document's actor takes the channel: the old actor is unbound and its receiveMessage is its own again
  const next = { actorName: 'browser::page[1]/1/6/2', sendAsyncMessage() {} };
  r.parentEnd.bindToActor(next);
  assert.equal(r.parentActor.receiveMessage, Object.getPrototypeOf(r.parentActor).receiveMessage);
  r.contentActor.sendAsyncMessage('juggler:became-current', {});
  await settle();
  assert.equal(r.registry.bound.length, 1);
  assert.equal(r.reattached(), 1);
});

test('a message of the channel still reaches the channel of a bound actor', async () => {
  const r = root();
  await settle();
  r.contentEnd.connect('page').emit('event', 'hello');
  await settle();
  assert.deepEqual(plain(r.seen), ['hello']);
});
