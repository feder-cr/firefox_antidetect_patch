// The registry of navigations the parent keeps for a page (FrameNavigations.js), loaded the way the parent loads
// it.
//
// The navigations of a frame (Page.navigationStarted, navigationCommitted, navigationAborted) were authored by the
// content process that held the frame, from its own state. A navigation that changes the process starts in one
// process and commits in another, and the process that commits cannot know the id the navigation was started with.
// The parent sees both ends, so the registry lives there, fed by the document requests it sees, by what the content
// relays over the 'frames' namespace (a navigation begun, a document inserted or restored, a frame as it is, a
// navigation that ended in an error). The content process relays the aborts too, from its own ordered view of
// the load, so the parent's progress listener is not used. Each navigation id is started once, and ends once:
// committed, aborted, or replaced by a newer one.
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

const { FrameNavigations } = load('FrameNavigations.js', {}, ['FrameNavigations']);

const MAIN = 'mainframe-1';
const CHILD = 'frame-5';

function setup() {
  const events = [];
  const registry = new FrameNavigations((name, payload, frameId) => {
    // The frame the event is keyed by is the frame of the payload.
    assert.equal(frameId, payload.frameId);
    // Copied into this realm: the registry runs in a vm context, whose objects have another prototype.
    events.push([name, { ...payload }]);
  });
  return { registry, events };
}

const started = (frameId, navigationId) => ['navigationstarted', { frameId, navigationId }];
const committed = (frameId, navigationId, url, name) => ['navigationcommitted', { frameId, navigationId, url, name }];
const aborted = (frameId, navigationId, errorText) => ['navigationaborted', { frameId, navigationId, errorText }];
const initialFailed = frameId => ['initialloadfailed', { frameId }];

test('a navigation is started by its request and committed by the document, with the same id', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-1');
  assert.equal(registry.lastCommittedNavigationId(MAIN), null);
  registry.onDocumentInserted(MAIN, 'http://a/', 'name-a');
  assert.deepEqual(events, [started(MAIN, 'nav-1'), committed(MAIN, 'nav-1', 'http://a/', 'name-a')]);
  assert.equal(registry.pendingNavigationId(MAIN), undefined);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-1');
});

test('a document with no navigation in flight commits nothing', () => {
  const { registry, events } = setup();
  registry.onDocumentInserted(MAIN, 'about:blank', '');
  assert.deepEqual(events, []);
  assert.equal(registry.lastCommittedNavigationId(MAIN), null);
  // And a document inserted after the commit (document.write after document.open) commits nothing either.
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentInserted(MAIN, 'http://a/', '');
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'http://a/', '');
  assert.deepEqual(events, []);
});

test('a request without a navigation id starts nothing', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, undefined, 'http://a/');
  registry.onContentNavigationStarted(MAIN, '');
  assert.deepEqual(events, []);
});

test('the request and the content\'s relay of the same navigation start it once, in either order', () => {
  for (const order of ['request first', 'relay first']) {
    const { registry, events } = setup();
    if (order === 'request first') {
      registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
      registry.onContentNavigationStarted(MAIN, 'nav-1');
    } else {
      registry.onContentNavigationStarted(MAIN, 'nav-1');
      registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
    }
    assert.deepEqual(events, [started(MAIN, 'nav-1')], order);
    // A redirect of the navigation is one more request with the same id.
    registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/redirected');
    assert.deepEqual(events, [started(MAIN, 'nav-1')], order);
  }
});

test('a navigation only the content knows (about:blank, data:) starts and commits without a request', () => {
  const { registry, events } = setup();
  registry.onContentNavigationStarted(MAIN, 'nav-3');
  registry.onDocumentInserted(MAIN, 'data:text/html,x', '');
  assert.deepEqual(events, [started(MAIN, 'nav-3'), committed(MAIN, 'nav-3', 'data:text/html,x', '')]);
});

test('a request of a navigation already committed does not start it again (the redirect after the commit)', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentInserted(MAIN, 'http://a/', '');
  events.length = 0;
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/again');
  registry.onContentNavigationStarted(MAIN, 'nav-1');
  assert.deepEqual(events, []);
  assert.equal(registry.pendingNavigationId(MAIN), undefined);
  // The commit it already made is not undone: the next document of the frame has no navigation to commit.
  registry.onDocumentInserted(MAIN, 'http://a/again', '');
  assert.deepEqual(events, []);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-1');
});

test('a request of a navigation that ended in an abort does not start it again', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onNavigationAborted(MAIN, 'nav-1', 'NS_BINDING_ABORTED');
  events.length = 0;
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onContentNavigationStarted(MAIN, 'nav-1');
  assert.deepEqual(events, []);
  assert.equal(registry.pendingNavigationId(MAIN), undefined);
});

test('a new navigation replaces the one in flight without ending it, and a late request of the replaced one is ignored', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentRequest(MAIN, 'nav-2', 'http://b/');
  assert.deepEqual(events, [started(MAIN, 'nav-1'), started(MAIN, 'nav-2')]);   // no abort of nav-1
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-2');
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/slow');   // the replaced one's request arrives late
  registry.onContentNavigationStarted(MAIN, 'nav-1');
  assert.deepEqual(events, [started(MAIN, 'nav-1'), started(MAIN, 'nav-2')]);
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-2');
  registry.onDocumentInserted(MAIN, 'http://b/', '');
  assert.deepEqual(events.slice(2), [committed(MAIN, 'nav-2', 'http://b/', '')]);
});

test('frames are independent: a navigation of one does not replace, commit or end another\'s', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentRequest(CHILD, 'nav-2', 'http://a/frame');
  registry.onDocumentInserted(CHILD, 'http://a/frame', 'child');
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-1');
  assert.equal(registry.lastCommittedNavigationId(CHILD), 'nav-2');
  registry.onNavigationAborted(CHILD, 'nav-1', 'NS_BINDING_ABORTED');   // the main frame's id, said of the child
  registry.onNavigationAborted(CHILD, 'nav-2', 'NS_BINDING_ABORTED');   // nothing pending in the child
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-1');
  assert.deepEqual(events, [started(MAIN, 'nav-1'), started(CHILD, 'nav-2'), committed(CHILD, 'nav-2', 'http://a/frame', 'child')]);
});

test('the ended navigations remembered are bounded, the newest are kept', () => {
  const { registry, events } = setup();
  for (let i = 1; i <= 20; i++) {
    registry.onDocumentRequest(MAIN, 'nav-' + i, 'http://a/' + i);
    registry.onDocumentInserted(MAIN, 'http://a/' + i, '');
  }
  events.length = 0;
  registry.onDocumentRequest(MAIN, 'nav-20', 'http://a/20');   // the last: committed, still known
  registry.onDocumentRequest(MAIN, 'nav-19', 'http://a/19');   // a recent one: still remembered
  assert.deepEqual(events, []);
  // The oldest are let go, so the memory of a long-lived frame does not grow with its navigations.
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/1');
  assert.deepEqual(events, [started(MAIN, 'nav-1')]);
});

// --- a navigation that ended in an error, as the content process says it

test('an aborted navigation ends the navigation of its id, with the text the content gives', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onNavigationAborted(MAIN, 'nav-1', 'NS_ERROR_CONNECTION_REFUSED');
  assert.deepEqual(events, [started(MAIN, 'nav-1'), aborted(MAIN, 'nav-1', 'NS_ERROR_CONNECTION_REFUSED')]);
  assert.equal(registry.pendingNavigationId(MAIN), undefined);
  // It ends once.
  registry.onNavigationAborted(MAIN, 'nav-1', 'NS_ERROR_CONNECTION_REFUSED');
  assert.equal(events.length, 2);
});

test('a navigation only the content knows (data:, a refused port) is aborted the same way', () => {
  const { registry, events } = setup();
  registry.onContentNavigationStarted(MAIN, 'nav-88');
  registry.onNavigationAborted(MAIN, 'nav-88', 'NS_ERROR_PORT_ACCESS_NOT_ALLOWED');
  assert.deepEqual(events, [started(MAIN, 'nav-88'), aborted(MAIN, 'nav-88', 'NS_ERROR_PORT_ACCESS_NOT_ALLOWED')]);
  assert.equal(registry.pendingNavigationId(MAIN), undefined);
});

test('the abort of a navigation the newer one replaced is told under its own id, and the newer one stays', () => {
  // A page that navigates while a goto is in flight: the parent hears the new navigation before the process says the
  // old one's load was cut. The client still waits for the end of the old one (goto raises NS_BINDING_ABORTED).
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentRequest(MAIN, 'nav-2', 'http://b/');
  events.length = 0;
  registry.onNavigationAborted(MAIN, 'nav-1', 'NS_BINDING_ABORTED');
  assert.deepEqual(events, [aborted(MAIN, 'nav-1', 'NS_BINDING_ABORTED')]);
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-2');
  registry.onNavigationAborted(MAIN, 'nav-1', 'NS_BINDING_ABORTED');   // told once
  assert.equal(events.length, 1);
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'http://b/', '');
  assert.deepEqual(events, [committed(MAIN, 'nav-2', 'http://b/', '')], 'the newer one still commits');
});

test('the abort of a navigation never seen, or of another frame, says nothing', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  events.length = 0;
  registry.onNavigationAborted(MAIN, 'nav-9', 'NS_ERROR_PORT_ACCESS_NOT_ALLOWED');
  registry.onNavigationAborted(CHILD, 'nav-1', 'NS_ERROR_PORT_ACCESS_NOT_ALLOWED');
  assert.deepEqual(events, []);
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-1');
});

test('an abort with nothing in flight says nothing, and does not undo a commit', () => {
  const { registry, events } = setup();
  registry.onNavigationAborted(MAIN, 'nav-1', 'NS_BINDING_ABORTED');
  assert.deepEqual(events, []);
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentInserted(MAIN, 'http://a/', '');
  events.length = 0;
  registry.onNavigationAborted(MAIN, 'nav-1', 'NS_BINDING_ABORTED');   // the load cut after its document came
  assert.deepEqual(events, []);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-1');
});

test('the abort of a navigation that never committed, in a frame that is not the main one, also ends its first load', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(CHILD, 'nav-2', 'http://a/frame');
  registry.onNavigationAborted(CHILD, 'nav-2', 'NS_ERROR_CONNECTION_REFUSED');
  assert.deepEqual(events, [
    started(CHILD, 'nav-2'),
    aborted(CHILD, 'nav-2', 'NS_ERROR_CONNECTION_REFUSED'),
    initialFailed(CHILD),
  ]);
  // Once: the next refusal of the same frame is not a first load.
  events.length = 0;
  registry.onDocumentRequest(CHILD, 'nav-3', 'http://a/frame2');
  registry.onNavigationAborted(CHILD, 'nav-3', 'NS_ERROR_CONNECTION_REFUSED');
  assert.deepEqual(events.map(e => e[0]), ['navigationstarted', 'navigationaborted']);
});

test('a refused first load of a child frame known only to the content ends its load too, once', () => {
  const { registry, events } = setup();
  registry.onContentNavigationStarted(CHILD, 'nav-7');
  registry.onNavigationAborted(CHILD, 'nav-7', 'NS_ERROR_UNKNOWN_PROTOCOL');
  assert.deepEqual(events, [
    started(CHILD, 'nav-7'),
    aborted(CHILD, 'nav-7', 'NS_ERROR_UNKNOWN_PROTOCOL'),
    initialFailed(CHILD),
  ]);
});

test('the main frame\'s refused first load does not say the load of a frame failed', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onNavigationAborted(MAIN, 'nav-1', 'NS_ERROR_CONNECTION_REFUSED');
  assert.deepEqual(events.map(e => e[0]), ['navigationstarted', 'navigationaborted']);
});

test('a frame that already committed a document does not fail its first load when a later navigation is refused', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(CHILD, 'nav-2', 'http://a/frame');
  registry.onDocumentInserted(CHILD, 'http://a/frame', '');
  registry.onDocumentRequest(CHILD, 'nav-3', 'http://a/frame2');
  events.length = 0;
  registry.onNavigationAborted(CHILD, 'nav-3', 'NS_ERROR_CONNECTION_REFUSED');
  assert.deepEqual(events.map(e => e[0]), ['navigationaborted']);
});

// --- the navigation the content process had in flight when a document appeared

test('a document inserted under the id of the pending navigation commits it as usual', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentInserted(MAIN, 'http://a/', 'n', 'nav-1');
  assert.deepEqual(events, [started(MAIN, 'nav-1'), committed(MAIN, 'nav-1', 'http://a/', 'n')]);
  assert.equal(registry.pendingNavigationId(MAIN), undefined);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-1');
});

test('a document inserted under the id of a navigation the parent already replaced is committed under its own id, and the newer one stays pending', () => {
  // A page that navigates from its first script. Measured: the parent saw navigation 23 start, then 26 start (the
  // request of the script's navigation, which replaced 23), and only then heard of the document of 23. It was
  // committed as 26 with the url of 23, and the real document of 26 found nothing to commit.
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-23', 'http://a/first');
  registry.onDocumentRequest(MAIN, 'nav-26', 'http://a/second');
  events.length = 0;

  registry.onDocumentInserted(MAIN, 'http://a/first', 'first', 'nav-23');
  assert.deepEqual(events, [committed(MAIN, 'nav-23', 'http://a/first', 'first')]);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-23');
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-26', 'the newer navigation was not touched');

  // Its own document, when it comes, commits it under its id.
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'http://a/second', 'second', 'nav-26');
  assert.deepEqual(events, [committed(MAIN, 'nav-26', 'http://a/second', 'second')]);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-26');
  assert.equal(registry.pendingNavigationId(MAIN), undefined);
});

test('the same race, without the id on the second document, still commits the newer one', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-23', 'http://a/first');
  registry.onDocumentRequest(MAIN, 'nav-26', 'http://a/second');
  registry.onDocumentInserted(MAIN, 'http://a/first', '', 'nav-23');
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'http://a/second', '');
  assert.deepEqual(events, [committed(MAIN, 'nav-26', 'http://a/second', '')]);
});

test('a document of a replaced navigation is committed once, and a second document under its id commits nothing', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-23', 'http://a/first');
  registry.onDocumentRequest(MAIN, 'nav-26', 'http://a/second');
  registry.onDocumentInserted(MAIN, 'http://a/first', '', 'nav-23');
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'http://a/first', '', 'nav-23');   // e.g. document.open() of the same document
  assert.deepEqual(events, []);
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-26');
});

test('a document of a replaced navigation leaves its frame with no first load to fail', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(CHILD, 'nav-23', 'http://a/first');
  registry.onDocumentRequest(CHILD, 'nav-26', 'http://a/second');
  registry.onDocumentInserted(CHILD, 'http://a/first', '', 'nav-23');
  events.length = 0;
  registry.onNavigationAborted(CHILD, 'nav-26', 'NS_ERROR_CONNECTION_REFUSED');
  assert.deepEqual(events, [aborted(CHILD, 'nav-26', 'NS_ERROR_CONNECTION_REFUSED')]);   // no initialloadfailed
});

test('a document inserted under an id that was never started is ignored, whatever is pending', () => {
  const { registry, events } = setup();
  registry.onDocumentInserted(MAIN, 'http://x/', '', 'nav-404');   // nothing pending
  assert.deepEqual(events, []);
  assert.equal(registry.lastCommittedNavigationId(MAIN), null);
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'http://x/', '', 'nav-404');   // another navigation pending
  assert.deepEqual(events, []);
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-1', 'the pending navigation is not committed by a document that is not its own');
  assert.equal(registry.lastCommittedNavigationId(MAIN), null);
  registry.onDocumentInserted(MAIN, 'http://a/', '', 'nav-1');
  assert.deepEqual(events, [committed(MAIN, 'nav-1', 'http://a/', '')]);
});

test('a document under the id of another frame\'s navigation is ignored', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentRequest(CHILD, 'nav-2', 'http://a/frame');
  registry.onDocumentRequest(CHILD, 'nav-3', 'http://a/frame2');
  events.length = 0;
  registry.onDocumentInserted(CHILD, 'http://a/', '', 'nav-1');   // nav-1 is the main frame's, still pending there
  assert.deepEqual(events, []);
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-1');
  assert.equal(registry.pendingNavigationId(CHILD), 'nav-3');
});

test('a document under the id of a navigation no longer remembered (more than the last few) is ignored', () => {
  const { registry, events } = setup();
  for (let i = 1; i <= 20; i++) {
    registry.onDocumentRequest(MAIN, 'nav-' + i, 'http://a/' + i);
    registry.onDocumentInserted(MAIN, 'http://a/' + i, '');
  }
  registry.onDocumentRequest(MAIN, 'nav-21', 'http://a/21');
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'http://a/1', '', 'nav-1');
  assert.deepEqual(events, []);
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-21');
});

// --- a frame as its process has it

test('a document no navigation made does not borrow the id of the one it replaces', () => {
  // The about:blank a script navigates to is said by its process as a document that is already loaded, with no id:
  // measured, the commit carried the previous document's id and the client took the previous response for it.
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentInserted(MAIN, 'http://a/', 'n');
  events.length = 0;
  registry.onFrameKnown(MAIN, { url: 'about:blank', name: 'n2' });
  assert.deepEqual(events, [committed(MAIN, undefined, 'about:blank', 'n2')]);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-1', 'what was committed by a navigation stays');
});

test('a frame already loaded is committed with the id the process gives, which the registry keeps', () => {
  const { registry, events } = setup();
  registry.onFrameKnown(MAIN, { url: 'http://a/', name: '', navigationId: 'nav-4' });
  assert.deepEqual(events, [committed(MAIN, 'nav-4', 'http://a/', '')]);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-4');
  // It is a committed navigation like any other: its late request does not start it again.
  registry.onDocumentRequest(MAIN, 'nav-4', 'http://a/');
  assert.deepEqual(events, [committed(MAIN, 'nav-4', 'http://a/', '')]);
});

test('the first document of a frame no navigation made is committed with no id', () => {
  const { registry, events } = setup();
  registry.onFrameKnown(CHILD, { url: 'about:blank', name: '' });
  assert.deepEqual(events, [committed(CHILD, undefined, 'about:blank', '')]);
  assert.equal(registry.lastCommittedNavigationId(CHILD), null);
  // It counts as a first document: a later refusal is not a first load.
  registry.onContentNavigationStarted(CHILD, 'nav-2');
  registry.onNavigationAborted(CHILD, 'nav-2', 'NS_ERROR_UNKNOWN_PROTOCOL');
  assert.deepEqual(events.map(e => e[0]), ['navigationcommitted', 'navigationstarted', 'navigationaborted']);
});

test('a navigation in flight is started', () => {
  const { registry, events } = setup();
  registry.onFrameKnown(MAIN, { url: '', name: '', pendingNavigationId: 'nav-9' });
  assert.deepEqual(events, [started(MAIN, 'nav-9')]);
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-9');
  // The registry already has one in flight for the frame: the process's word does not replace it.
  registry.onFrameKnown(MAIN, { url: '', name: '', pendingNavigationId: 'nav-10' });
  assert.deepEqual(events, [started(MAIN, 'nav-9')]);
  // And committing it afterwards works.
  registry.onDocumentInserted(MAIN, 'http://a/', '');
  assert.deepEqual(events[1], committed(MAIN, 'nav-9', 'http://a/', ''));
});

test('a frame with a document and a navigation in flight says both, the document first', () => {
  const { registry, events } = setup();
  registry.onFrameKnown(MAIN, { url: 'http://a/', name: 'n', navigationId: 'nav-4', pendingNavigationId: 'nav-5' });
  assert.deepEqual(events, [committed(MAIN, 'nav-4', 'http://a/', 'n'), started(MAIN, 'nav-5')]);
});

test('a frame said twice is committed twice: the client dropped the frames in between', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.onDocumentInserted(MAIN, 'http://a/', '');
  events.length = 0;
  registry.onFrameKnown(MAIN, { url: 'http://a/', name: '' });
  registry.onFrameKnown(MAIN, { url: 'http://a/', name: '' });
  assert.deepEqual(events, [committed(MAIN, undefined, 'http://a/', ''), committed(MAIN, undefined, 'http://a/', '')]);
  registry.onFrameKnown(MAIN, { url: 'http://a/', name: '', navigationId: 'nav-1' });
  assert.deepEqual(events.slice(2), [committed(MAIN, 'nav-1', 'http://a/', '')], 'with the id its process gives');
});

test('a frame with neither a document nor a navigation says nothing', () => {
  const { registry, events } = setup();
  registry.onFrameKnown(MAIN, { url: '', name: '' });
  assert.deepEqual(events, []);
});

// --- a document restored from the back-forward cache

test('a restored document is a navigation of its own, started then committed, with ids the registry counts', () => {
  const { registry, events } = setup();
  registry.onDocumentRestored(MAIN, 'http://a/', 'n');
  assert.deepEqual(events, [
    started(MAIN, 'nav-bfcache-1'),
    committed(MAIN, 'nav-bfcache-1', 'http://a/', 'n'),
  ]);
  events.length = 0;
  registry.onDocumentRestored(MAIN, 'http://b/', '');
  assert.deepEqual(events, [
    started(MAIN, 'nav-bfcache-2'),
    committed(MAIN, 'nav-bfcache-2', 'http://b/', ''),
  ]);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-bfcache-2');
  assert.equal(registry.pendingNavigationId(MAIN), undefined);
});

test('the restore ids are counted for the whole page, not per frame', () => {
  const { registry, events } = setup();
  registry.onDocumentRestored(MAIN, 'http://a/', '');
  registry.onDocumentRestored(CHILD, 'http://a/frame', '');
  assert.deepEqual(events.filter(e => e[0] === 'navigationstarted').map(e => e[1].navigationId),
      ['nav-bfcache-1', 'nav-bfcache-2']);
});

test('a restore replaces the navigation in flight, whose late request does not restart it', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  events.length = 0;
  registry.onDocumentRestored(MAIN, 'http://b/', '');
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  assert.deepEqual(events.map(e => e[0]), ['navigationstarted', 'navigationcommitted']);
  assert.equal(registry.pendingNavigationId(MAIN), undefined);
});

// --- a frame that is gone

test('forget drops everything of a frame, and only of that frame', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(CHILD, 'nav-2', 'http://a/frame');
  registry.onDocumentInserted(CHILD, 'http://a/frame', '');
  registry.onDocumentRequest(CHILD, 'nav-3', 'http://a/frame2');
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/');
  registry.forget(CHILD);
  assert.equal(registry.pendingNavigationId(CHILD), undefined);
  assert.equal(registry.lastCommittedNavigationId(CHILD), null);
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-1');
  // Nothing of it is remembered: the same id is a new navigation, and an abort has nothing to end.
  events.length = 0;
  registry.onNavigationAborted(CHILD, 'nav-3', 'NS_BINDING_ABORTED');
  assert.deepEqual(events, []);
  registry.onDocumentInserted(CHILD, 'http://a/frame', '', 'nav-2');   // the document of a forgotten navigation
  assert.deepEqual(events, []);
  registry.onDocumentRequest(CHILD, 'nav-2', 'http://a/frame');
  assert.deepEqual(events, [started(CHILD, 'nav-2')]);
  // And its first load is a first load again.
  events.length = 0;
  registry.onNavigationAborted(CHILD, 'nav-2', 'NS_ERROR_CONNECTION_REFUSED');
  assert.deepEqual(events.map(e => e[0]), ['navigationaborted', 'initialloadfailed']);
});

// --- a document with no navigation of its process's to go by

test('a document at an address the navigation never went by is not its commit (document.open() on the page that stays)', () => {
  // Measured: a goto sent while the page rewrote itself with document.open() was reported committed on the old page,
  // at the old address, because the root element the write inserted was taken for the pending navigation's document.
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/new?k=1');
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'http://a/old', '');
  assert.deepEqual(events, [], "the old page's root element is not the new navigation's document");
  assert.equal(registry.pendingNavigationId(MAIN), 'nav-1');
  registry.onNavigationAborted(MAIN, 'nav-1', 'NS_BINDING_ABORTED');
  assert.deepEqual(events, [aborted(MAIN, 'nav-1', 'NS_BINDING_ABORTED')]);
});

test('the document of a redirect is at the address of one of its hops, and a fragment does not tell addresses apart', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/start');
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://a/landed');
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'http://a/landed#section', '');
  assert.deepEqual(events, [committed(MAIN, 'nav-1', 'http://a/landed#section', '')]);
});

test('a navigation with no request of its own (about:blank, data:) commits whatever document appears', () => {
  const { registry, events } = setup();
  registry.onContentNavigationStarted(MAIN, 'nav-1');
  events.length = 0;
  registry.onDocumentInserted(MAIN, 'about:blank', '');
  assert.deepEqual(events, [committed(MAIN, 'nav-1', 'about:blank', '')]);
});

// --- the document a frame had, for when it comes back from the cache

test('the last document told committed is kept, its address follows a same-document navigation, and forget drops it', () => {
  const { registry } = setup();
  assert.equal(registry.lastDocument(CHILD), undefined);
  registry.onDocumentRequest(CHILD, 'nav-3', 'http://b/mid');
  registry.onDocumentInserted(CHILD, 'http://b/mid', 'b');
  assert.deepEqual({ ...registry.lastDocument(CHILD) }, { navigationId: 'nav-3', url: 'http://b/mid', name: 'b' });
  registry.onSameDocumentNavigation(CHILD, 'http://b/mid#section');
  assert.equal(registry.lastDocument(CHILD).url, 'http://b/mid#section');
  registry.onFrameKnown(CHILD, { url: 'http://b/other', name: 'n', navigationId: 'nav-4' });
  assert.deepEqual({ ...registry.lastDocument(CHILD) }, { navigationId: 'nav-4', url: 'http://b/other', name: 'n' });
  registry.forget(CHILD);
  assert.equal(registry.lastDocument(CHILD), undefined);
});

// ── a navigation told aborted whose document appears all the same ───────────────────────────────────────────────────────────────
//
// The page's own location.replace (load 15032385539) is cancelled by the client's goto and restarted by the engine under the same
// load identifier. The first cancel is told as an abort; the document that appears is that navigation's. It used to be dropped
// (the registry knows the id as ended, not as replaced), so the frame had a document the client was never told of.

test('the document of a navigation told aborted is committed under its own id', () => {
  const { registry, events } = setup();
  registry.onContentNavigationStarted(MAIN, 'nav-page');          // the page's own navigation
  registry.onDocumentRequest(MAIN, 'nav-ours', 'http://x/ours');  // the client's replaces it in flight
  registry.onNavigationAborted(MAIN, 'nav-page', 'NS_BINDING_ABORTED');   // the cancel of the first, told
  events.length = 0;
  registry.onNavigationAborted(MAIN, 'nav-ours', 'NS_BINDING_ABORTED');   // the client's own is over (told by the content)
  registry.onDocumentInserted(MAIN, 'http://x/landed', '', 'nav-page');
  assert.deepEqual(events, [aborted(MAIN, 'nav-ours', 'NS_BINDING_ABORTED'), committed(MAIN, 'nav-page', 'http://x/landed', '')]);
  assert.equal(registry.lastCommittedNavigationId(MAIN), 'nav-page');
});

test('a navigation that is already committed is not committed again by a second document under its id', () => {
  const { registry, events } = setup();
  registry.onDocumentRequest(MAIN, 'nav-1', 'http://x/a');
  registry.onDocumentInserted(MAIN, 'http://x/a', '', 'nav-1');
  events.length = 0;
  registry.onDocumentRequest(MAIN, 'nav-2', 'http://x/b');
  registry.onDocumentInserted(MAIN, 'http://x/a', '', 'nav-1');
  assert.deepEqual(events, [started(MAIN, 'nav-2')]);
});
