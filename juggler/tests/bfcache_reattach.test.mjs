// A document restored from the back-forward cache is heard from its first
// word, and the client learns the document before it hears it.
//
// B303 E3: the parent end of a page's channel follows the CURRENT document's
// actor, so on a navigation it rebinds away from the old one; the old
// document's content end was never told, kept its transport and stayed ready.
// Restored, it spoke at once - from its own `pageshow`, before the parent had
// rebound - into an actor nothing listened to, and since it was ready the
// READY of the rebinding replayed nothing. Measured on firefox-47 for
// Windows: what a page logged in its `pageshow` and one task later never
// reached the client in 10 rounds out of 10, nor did a binding it called;
// what it logged 500 ms later did.
//
// The content end now leaves the ready state when its document goes into the
// cache (`detachFromPeer`, from the actor's `pagehide` with `persisted`), so
// what it says meanwhile waits in its queue, and the READY of the rebinding
// runs the reattachment hook - the restored document's announcement - before
// the queue goes out.
//
// Run: node --test juggler/tests/

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const SOURCE = fs.readFileSync(
  path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'SimpleChannel.js'), 'utf8');

function loadIntoProcess() {
  const scope = {
    dump: () => {},
    ChromeUtils: { importESModule: () => ({ setTimeout }) },
  };
  vm.createContext(scope);
  vm.runInContext(SOURCE, scope);
  return scope.SimpleChannel;
}

const settle = () => new Promise(resolve => setTimeout(resolve, 20));

// The browser's wiring, not a symmetric link: each content end keeps the
// transport of its own actor for the life of its document, and the parent end
// is bound to one actor at a time. What an actor sends while the parent is
// bound elsewhere reaches a JugglerFrameParent that drops it.
function page() {
  const Parent = loadIntoProcess();
  const Content = loadIntoProcess();
  const parent = new Parent('browser::page[1]', 'target-1');
  const seen = [];
  parent.register('page', { event: value => { seen.push(value); } });
  let boundTo = null;
  const deliver = (to, copy) => setImmediate(() => to._onMessage(copy));
  const documents = {};
  const open = name => {
    const end = new Content('', 'process-9');
    end.setTransport({
      sendMessage: message => {
        if (boundTo === name)
          deliver(parent, JSON.parse(JSON.stringify(message)));
      },
      dispose: () => {},
    });
    documents[name] = end;
    return end;
  };
  const bind = name => {
    boundTo = name;
    const end = documents[name];
    parent.resetTransport();
    parent.setTransport({
      sendMessage: message => deliver(end, JSON.parse(JSON.stringify(message))),
      dispose: () => {},
    });
  };
  return { open, bind, seen };
}

test('what a restored document says before the rebinding is heard', async () => {
  const { open, bind, seen } = page();
  const one = open('one');
  bind('one');
  await settle();
  one.connect('page').emit('event', 'one loaded');
  await settle();

  // To page two: page one goes into the cache, the parent follows page two.
  one.detachFromPeer();
  const two = open('two');
  bind('two');
  await settle();
  two.connect('page').emit('event', 'two loaded');
  await settle();

  // Back: page one's pageshow speaks before the parent has rebound to it.
  one.connect('page').emit('event', 'one pageshow');
  await settle();
  bind('one');
  await settle();
  assert.deepEqual(seen, ['one loaded', 'two loaded', 'one pageshow']);
});

test('the reattachment hook speaks before the queue', async () => {
  const { open, bind, seen } = page();
  const one = open('one');
  one.onReattached(() => one.connect('page').emit('event', 'one announced'));
  bind('one');
  await settle();

  one.detachFromPeer();
  const two = open('two');
  bind('two');
  await settle();

  one.connect('page').emit('event', 'one pageshow');
  one.connect('page').emit('event', 'one tick0');
  bind('one');
  await settle();
  // The first READY of a document is not a reattachment: no announcement then.
  assert.deepEqual(seen, ['one announced', 'one pageshow', 'one tick0']);
});

test('an end that was never detached announces nothing on a READY', async () => {
  const { open, bind, seen } = page();
  const one = open('one');
  let announced = 0;
  one.onReattached(() => { announced++; });
  bind('one');
  await settle();
  bind('one');
  await settle();
  assert.equal(announced, 0);
  assert.deepEqual(seen, []);
});

test('a request answered before the cache is not dispatched again', async () => {
  const { open, bind, seen } = page();
  const one = open('one');
  bind('one');
  await settle();
  await one.connect('page').send('event', 'answered');
  one.detachFromPeer();
  const two = open('two');
  bind('two');
  await settle();
  bind('one');
  await settle();
  assert.deepEqual(seen, ['answered']);
});
