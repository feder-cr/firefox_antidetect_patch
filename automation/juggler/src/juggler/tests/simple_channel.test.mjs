// SimpleChannel's two ends, each loaded the way loadSubScript loads it into a
// process, connected by an in-memory transport that can lose messages.
//
// B286: a page's announcement that its main world was created never reached
// the client. The content end of a page's channel is created per agents
// instance with the uid 'process-<pid>' (content/main.js), so a second instance
// in the same process carried the uid of the first and numbered its requests
// from 1 again. The parent end keeps the answers it gave until they are
// acknowledged, keyed by request number alone, and forgot them only when the
// peer's uid changed: the new instance's first requests were answered from the
// old instance's answers and never dispatched. Measured on firefox-41 with an
// instrumented copy: "DEDUP #2 runtimeExecutionContextsCleared",
// "DEDUP #3 runtimeExecutionContextCreated" on a new content channel.
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

// One evaluation per process, as Services.scriptloader.loadSubScript does.
function loadIntoProcess() {
  const scope = {
    dump: () => {},
    ChromeUtils: { importESModule: () => ({ setTimeout }) },
  };
  vm.createContext(scope);
  vm.runInContext(SOURCE, scope);
  return scope.SimpleChannel;
}

// Connect two ends. `lose(message, from)` returning true drops a message, as
// IPC drops what an actor that is going away sends.
function link(a, b, lose = () => false) {
  const deliver = (from, to) => message => {
    if (lose(message, from))
      return;
    const copy = JSON.parse(JSON.stringify(message));
    setImmediate(() => to._onMessage(copy));
  };
  a.setTransport({ sendMessage: deliver(a, b), dispose: () => {} });
  b.setTransport({ sendMessage: deliver(b, a), dispose: () => {} });
}

const settle = () => new Promise(resolve => setTimeout(resolve, 20));

function parentEnd() {
  const Parent = loadIntoProcess();
  const parent = new Parent('browser::page[1]', 'target-1');
  const seen = [];
  parent.register('page', { event: value => { seen.push(value); } });
  return { parent, seen };
}

test('a new content instance in the same process is heard', async () => {
  const { parent, seen } = parentEnd();
  const Content = loadIntoProcess();

  // The first instance's acknowledgements are lost: its actor went away
  // before they left, so the parent still holds its answers.
  const first = new Content('', 'process-9');
  link(parent, first, (message, from) => from === first && message.ack === 'RESPONSE_ACK');
  first.connect('page').emit('event', 'first #1');
  first.connect('page').emit('event', 'first #2');
  await settle();
  assert.deepEqual(seen, ['first #1', 'first #2']);

  // A new agents instance in the same process, with the name main.js gives.
  parent.resetTransport();
  const second = new Content('', 'process-9');
  link(parent, second);
  second.connect('page').emit('event', 'second #1');
  second.connect('page').emit('event', 'second #2');
  await settle();
  assert.deepEqual(seen, ['first #1', 'first #2', 'second #1', 'second #2']);
});

test('the same instance over a new transport is not dispatched twice', async () => {
  const { parent, seen } = parentEnd();
  const Content = loadIntoProcess();
  const content = new Content('', 'process-9');

  // The answer is lost on the way back, so the content end sends again.
  link(parent, content, (message, from) => from === parent && message.responseId);
  const answered = content.connect('page').send('event', 'once');
  await settle();
  assert.deepEqual(seen, ['once']);

  parent.resetTransport();
  content.resetTransport();
  link(parent, content);
  await answered;
  await settle();
  assert.deepEqual(seen, ['once']);
});

test('an older instance that comes back keeps its deduplication', async () => {
  const { parent, seen } = parentEnd();
  const Content = loadIntoProcess();

  const first = new Content('', 'process-9');
  link(parent, first, (message, from) => from === parent && message.responseId);
  const firstAnswered = first.connect('page').send('event', 'first');
  await settle();

  parent.resetTransport();
  first.resetTransport();
  const second = new Content('', 'process-9');
  link(parent, second);
  second.connect('page').emit('event', 'second');
  await settle();

  // The first instance's document comes back from the back-forward cache and
  // its channel replays what it never had answered.
  parent.resetTransport();
  second.resetTransport();
  link(parent, first);
  await firstAnswered;
  await settle();
  assert.deepEqual(seen, ['first', 'second']);
});

test('an acknowledged answer is forgotten', async () => {
  const { parent, seen } = parentEnd();
  const Content = loadIntoProcess();
  const content = new Content('', 'process-9');
  link(parent, content);
  await content.connect('page').send('event', 'acknowledged');
  await settle();
  assert.deepEqual(seen, ['acknowledged']);
  assert.equal(parent._bufferedResponses.size, 0);
});
