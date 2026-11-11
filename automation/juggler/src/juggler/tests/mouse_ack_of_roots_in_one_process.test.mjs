// The ack of a mouse event is heard by every root of the process that handled it, and the owner of the landing is not always the first.
//
// With sites isolated, the iframes of one site are held by ONE process, whatever the page they belong to: a page with
// five iframes of b.test has five roots in the process of b.test (eight such pages, forty). The renderer's ack is an
// observer notification of the process, so each of those roots tells its page handler that the event was handled, with
// its own armed landings. The first ack to arrive was taken for the answer: the id was let go, the wait ended, and the
// verdict was read when only a root next to the owner had spoken, so a click that landed was told "the event reached
// another document" (41 clicks of 3 rounds of eight tabs; one tab alone had its owner first by the order of the roots).
// The owner's ack, a millisecond later, came to an id that was not the page's own any more and was thrown away.
//
// The wait for a landing now ends on the ack of the root that owns it, or on an ack of the event from another PROCESS (the
// owner is not there and will not answer). And a root answers for the events that reached IT: a landing counts only
// if a pointer event of its type came to the tree after the landing was armed.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { ContentMux } from '../ContentMux.js';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const handlerSource = fs.readFileSync(path.join(JUGGLER, 'protocol/PageHandler.js'), 'utf8');
const agentSource = fs.readFileSync(path.join(JUGGLER, 'content/PageAgent.js'), 'utf8');

// The text of the method `name` of a class, comments and all, by matching its braces.
function methodText(source, name) {
  const at = source.search(new RegExp('^  ' + name + '\\(', 'm'));
  assert.ok(at >= 0, name + ' is a method');
  const open = source.indexOf('{', source.indexOf(')', at));
  let depth = 0;
  for (let i = open; i < source.length; i++) {
    if (source[i] === '{') depth++;
    else if (source[i] === '}' && --depth === 0)
      return source.slice(at, i + 1);
  }
  throw new Error('unbalanced ' + name);
}

const OWNER_PID = 4242;
const OTHER_PID = 5151;

function handler() {
  const mux = new ContentMux({pageTarget: {}, name: 'x', createChannel: () => ({})});
  mux.mainRootId = () => 1;
  const methods = ['_onPageInputEvent', '_ackEndsWait', '_ownerIsPast', '_whenInputHit', '_landingVerdict']
      .map(name => methodText(handlerSource, name)).join(',\n');
  const object = new Function('return {\n' + methods + '\n};')();
  Object.assign(object, {
    _lastHitEventId: 0,
    _ownEventIds: new Set(Array.from({length: 100}, (_, i) => i + 1)),
    _hitWaiters: new Set(),
    _landingVerdicts: new Map(),
    _contentChannel: mux,
    _pageEventSink: {emit() {}},
  });
  return {h: object, mux};
}

// What `armLanding` of a root answers through the mux: the root, its token, and the process of the root.
const arm = (mux, rootId, rootToken, pid) => {
  const muxToken = ++mux._nextToken;
  mux._armed.set(muxToken, {rootId, rootToken, pid});
  return muxToken;
};
const verdict = (type, landed) => ({type, landed, seen: 1, on: landed ? '' : 'p'});
const all = landed => ({mousemove: verdict('mousemove', landed), mousedown: verdict('mousedown', landed), mouseup: verdict('mouseup', landed)});

test('the ack of a root next to the owner, in the owner\'s process, does not end the wait: the owner\'s ack after it brings the verdict', async () => {
  const {h, mux} = handler();
  const token = arm(mux, 10, 1, OWNER_PID);          // the click is for the iframe of root 10; root 11 is another iframe of the same site
  let done = false;
  const waiting = h._whenInputHit(7, mux.armedEntry(token)).then(() => { done = true; });
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 7, pid: OWNER_PID, verdicts: {}}, {rootId: 11});
  await Promise.resolve();
  assert.equal(done, false, 'root 11 acked the event of root 10: the process says so, it does not say root 10 has judged it');
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 7, pid: OWNER_PID, verdicts: {1: all(true)}}, {rootId: 10});
  await waiting;
  assert.equal(done, true);
  const got = h._landingVerdict(7, 'mousedown', token);
  assert.equal(got.landed, true, 'the owner\'s verdict was kept although another root acked first');
});

test('the owner acked first, a root next to it after: the verdict is the owner\'s and the late ack harms nothing', async () => {
  const {h, mux} = handler();
  const token = arm(mux, 10, 1, OWNER_PID);
  const waiting = h._whenInputHit(8, mux.armedEntry(token));
  h._onPageInputEvent({type: 'mouseup', jugglerEventId: 8, pid: OWNER_PID, verdicts: {1: all(true)}}, {rootId: 10});
  await waiting;
  assert.equal(h._landingVerdict(8, 'mouseup', token).landed, true);
  h._onPageInputEvent({type: 'mouseup', jugglerEventId: 8, pid: OWNER_PID, verdicts: {}}, {rootId: 11});
  assert.equal(h._landingVerdicts.size, 0, 'the late ack of the root next to it keeps nothing');
});

test('an ack of the event from another process ends the wait at once: the owner is not there, nothing landed', async () => {
  const {h, mux} = handler();
  const token = arm(mux, 10, 1, OWNER_PID);
  const waiting = h._whenInputHit(9, mux.armedEntry(token));
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 9, pid: OTHER_PID, verdicts: {}}, {rootId: 20});
  await waiting;
  const got = h._landingVerdict(9, 'mousedown', token);
  assert.equal(got.landed, false);
  assert.match(got.on, /another document/);
});

test('an event already acked from another process needs no wait; one acked only inside the owner\'s process does', async () => {
  const {h, mux} = handler();
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 12, pid: OTHER_PID}, {rootId: 20});
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 13, pid: OWNER_PID}, {rootId: 11});
  const elsewhere = arm(mux, 10, 1, OWNER_PID);
  await h._whenInputHit(12, mux.armedEntry(elsewhere));
  let done = false;
  h._whenInputHit(13, mux.armedEntry(elsewhere)).then(() => { done = true; });
  await Promise.resolve();
  assert.equal(done, false, 'the owner has not answered 13');
});

test('the processes that acked are kept per event, and bounded', () => {
  const {h} = handler();
  h._ownEventIds = new Set(Array.from({length: 400}, (_, i) => i + 1));
  for (let id = 1; id <= 400; id++)
    h._onPageInputEvent({type: 'mousemove', jugglerEventId: id, pid: OWNER_PID}, {rootId: 11});
  assert.equal(h._ackedEvents.size, 256);
  assert.ok(h._ackedEvents.has(400));
  assert.ok(!h._ackedEvents.has(1));
});

test('the mux keeps the process of the root with the armed landing, when the root says it', async () => {
  const mux = new ContentMux({pageTarget: {}, name: 'x', createChannel: () => ({})});
  mux.rootOf = () => 10;
  mux._sendToRoot = async () => ({token: 3, pid: OWNER_PID});
  const result = await mux._dispatch({namespace: 'page'}, 'armLanding', [{frameId: 'f'}], {});
  assert.equal(result.pid, OWNER_PID);
  assert.deepEqual({...mux.armedEntry(result.token)}, {rootId: 10, rootToken: 3, pid: OWNER_PID});
  mux._sendToRoot = async () => ({token: 4});
  const old = await mux._dispatch({namespace: 'page'}, 'armLanding', [{frameId: 'f'}], {});
  assert.deepEqual({...mux.armedEntry(old.token)}, {rootId: 10, rootToken: 4}, 'a root that does not say its process leaves the entry as it was');
});

// ---- the root's side -----------------------------------------------------------------------------------------------

function agent() {
  globalThis.Services = {appinfo: {processID: OWNER_PID}};
  const names = ['_armLanding', '_pruneArmedLandings', '_landingVerdictsFor', '_landingOn'];
  const methods = names.map(name => methodText(agentSource, name)).join(',\n');
  const object = new Function('return {\n' + methods + '\n};')();
  const doc = {defaultView: {}};
  const node = {ownerDocument: doc, contains: () => false};
  const landings = new Map();
  Object.assign(object, {
    _armedLandings: new Map(),
    _armedSeen: new Map(),
    _lastLandingToken: 0,
    _frameTree: {
      frame: () => ({unsafeObject: () => node}),
      pointerLanding: type => landings.get(type) || null,
    },
  });
  // a pointer event of `type` reaches the tree, on the node itself or on another document
  const land = (type, on = node) => landings.set(type, {target: on, document: on.ownerDocument, seen: (landings.get(type)?.seen || 0) + 1});
  return {a: object, node, land};
}

test('a landing counts only a pointer event that reached the tree after it was armed: an older event of the type is not the answer', () => {
  const {a, node, land} = agent();
  land('mousedown');                                     // the click before, on the same button
  const armed = a._armLanding({frameId: 'f', objectId: 'o'});
  assert.equal(armed.pid, OWNER_PID, 'the root says its process');
  // the next event went to a root next to this one, which is in the same process: this tree hears the ack and answers
  const stale = a._landingVerdictsFor()[armed.token].mousedown;
  assert.equal(stale.landed, false, 'the mousedown on record is the one before');
  assert.match(stale.on, /since the landing was armed/);
  // the event reaches this tree: it is the answer
  node.contains = () => true;
  land('mousedown');
  assert.equal(a._landingVerdictsFor()[armed.token].mousedown.landed, true);
});

test('what the armed landing remembers of the events before it goes with it', () => {
  const {a, land} = agent();
  land('mousemove');
  const first = a._armLanding({frameId: 'f', objectId: 'o'});
  for (let i = 0; i < 9; i++)
    a._armLanding({frameId: 'f', objectId: 'o'});
  assert.ok(!a._armedLandings.has(first.token), 'the oldest armed landing is let go (8 at most)');
  a._landingVerdictsFor();
  assert.deepEqual([...a._armedSeen.keys()].sort((x, y) => x - y), [...a._armedLandings.keys()].sort((x, y) => x - y));
});
