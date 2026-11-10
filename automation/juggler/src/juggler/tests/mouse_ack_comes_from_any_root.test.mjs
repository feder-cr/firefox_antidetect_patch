// The page handler waits for the renderer's ack of a mouse event from whichever root the event reached.
//
// With one process per site (Fission) the process that handles a click is the one that owns the point, which the
// APZ hit test decides: an iframe of another site is handled in its own process. The ack used to be read in the
// main root alone (`awaitInputHandled` on the top's channel), so a click on such an iframe was answered after the
// 5 s bound or not at all. Measured on firefox-61 with the event sent through the APZ path: the 15 ids of 5 clicks
// were all acked, in the iframe's process, a millisecond after they were sent, and none in the top's.
//
// The page handler keeps one counter of the ids acked by every root (ids only grow), and the verdict of an armed
// landing as the root that owns it pushed it with its ack. The methods are the real ones, taken out of the file.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { ContentMux } from '../ContentMux.js';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(JUGGLER, 'protocol/PageHandler.js'), 'utf8');

// The text of the method `name` of the class, comments and all, by matching its braces.
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

// A page handler with only what the ack path touches, and the real methods.
function handler({mainRootId = 1} = {}) {
  const sunk = [];
  const mux = new ContentMux({pageTarget: {}, name: 'x', createChannel: () => ({})});
  mux.mainRootId = () => mainRootId;
  const methods = ['_onPageInputEvent', '_whenInputHit', '_landingVerdict'].map(methodText).join(',\n');
  const object = new Function('return {\n' + methods + '\n};')();
  Object.assign(object, {
    _lastHitEventId: 0,
    // the ids this page sent (1-100 here): an ack of an id that is not one of them is another page's
    _ownEventIds: new Set(Array.from({length: 100}, (_, i) => i + 1)),
    _hitWaiters: new Set(),
    _landingVerdicts: new Map(),
    _contentChannel: mux,
    _pageEventSink: {emit: (type, event) => sunk.push([type, event.jugglerEventId])},
  });
  return {h: object, mux, sunk};
}

const arm = (mux, rootId, rootToken) => {
  const muxToken = ++mux._nextToken;
  mux._armed.set(muxToken, {rootId, rootToken});
  return muxToken;
};
const verdict = (type, landed) => ({type, landed, seen: 1, on: landed ? '' : 'p'});
const all = landed => ({mousemove: verdict('mousemove', landed), mousedown: verdict('mousedown', landed), mouseup: verdict('mouseup', landed)});

test('the ack of any root ends the wait, and an older id does not', async () => {
  const {h} = handler();
  let done = false;
  const waiting = h._whenInputHit(5).then(() => { done = true; });
  h._onPageInputEvent({type: 'mousemove', jugglerEventId: 4}, {rootId: 9});
  await Promise.resolve();
  assert.equal(done, false, 'id 4 is not 5');
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 5}, {rootId: 9});   // a root that is not the main one
  await waiting;
  assert.equal(done, true);
  // an id already acked needs no wait; ids are compared, not counted
  await h._whenInputHit(3);
  h._onPageInputEvent({type: 'mousemove', jugglerEventId: 2}, {rootId: 1});
  assert.equal(h._lastHitEventId, 5, 'a smaller id does not move the counter back');
});

test('the verdict is the one of the command\'s type, not of the last event\'s: a right button ends with a contextmenu', () => {
  const {h, mux} = handler();
  const token = arm(mux, 7, 1);
  // the ack of the last event of a right click is a contextmenu; the verdicts of all three types travel with it
  h._onPageInputEvent({type: 'contextmenu', jugglerEventId: 12, verdicts: {1: all(true)}}, {rootId: 7});
  const got = h._landingVerdict(12, 'mousedown', token);
  assert.equal(got.type, 'mousedown');
  assert.equal(got.landed, true);
});

test('two roots with the same root token keep their own verdicts', () => {
  const {h, mux} = handler();
  const a = arm(mux, 7, 1);      // both roots numbered their first armed landing 1
  const b = arm(mux, 8, 1);
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 20, verdicts: {1: all(true)}}, {rootId: 7});
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 21, verdicts: {1: all(false)}}, {rootId: 8});
  assert.equal(h._landingVerdict(20, 'mousedown', a).landed, true);
  assert.equal(h._landingVerdict(21, 'mousedown', b).landed, false);
});

test('a root that never acked the event has no verdict: it did not land', () => {
  const {h, mux} = handler();
  const token = arm(mux, 7, 1);
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 30, verdicts: {}}, {rootId: 8});   // the event went to root 8
  const got = h._landingVerdict(30, 'mousedown', token);
  assert.equal(got.landed, false);
  assert.match(got.on, /another document/);
  // and an ack of the owner that is older than the event is not the event's
  const again = arm(mux, 7, 2);
  h._onPageInputEvent({type: 'mousemove', jugglerEventId: 31, verdicts: {2: all(true)}}, {rootId: 7});
  assert.equal(h._landingVerdict(40, 'mousedown', again).landed, false);
});

test('a verdict is read once and takes the armed landing with it', () => {
  const {h, mux} = handler();
  const token = arm(mux, 7, 1);
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 50, verdicts: {1: all(true)}}, {rootId: 7});
  assert.ok(mux.armedEntry(token));
  assert.equal(h._landingVerdict(50, 'mousedown', token).landed, true);
  assert.equal(mux.armedEntry(token), undefined);
  assert.equal(h._landingVerdicts.size, 0);
  assert.equal(h._landingVerdict(50, 'mousedown', token), null, 'a token already read is no landing');
  assert.equal(h._landingVerdict(50, 'mousedown', 0), null, 'no landing was armed');
});

test('the verdicts kept for armed landings that nothing reads are bounded', () => {
  const {h} = handler();
  for (let token = 1; token <= 40; token++)
    h._onPageInputEvent({type: 'mousemove', jugglerEventId: token, verdicts: {[token]: all(true)}}, {rootId: 7});
  assert.equal(h._landingVerdicts.size, 16);
  assert.ok(h._landingVerdicts.has('7:40'), 'the newest stays');
  assert.ok(!h._landingVerdicts.has('7:1'), 'the oldest goes');
});

test('a drag signal is the main root\'s: an iframe in another process does not start the gesture of the top', () => {
  const {h, sunk} = handler({mainRootId: 1});
  h._onPageInputEvent({type: 'dragstart', jugglerEventId: 0}, {rootId: 8});
  h._onPageInputEvent({type: 'juggler-drag-finalized'}, {rootId: 8});
  assert.deepEqual(sunk, []);
  h._onPageInputEvent({type: 'dragstart', jugglerEventId: 0}, {rootId: 1});
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 3}, {rootId: 8});
  assert.deepEqual(sunk.map(s => s[0]), ['dragstart', 'mousedown']);
});

test('the command waits on the aggregate, bounded as before, and asks no root to wait', () => {
  const strip = text => text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
  const text = strip(methodText('async _awaitInputHandled'));
  assert.match(text, /this\._awaitBounded\(this\._whenInputHit\(eventId\), 'mouse event ' \+ eventId\)/);
  assert.doesNotMatch(text, /awaitInputHandled'/);
  assert.match(text, /this\._landingVerdict\(eventId, type, landingToken\)/);
  const bounded = strip(methodText('async _awaitBounded'));
  assert.match(bounded, /setTimeout\(\(\) => resolve\('expired'\), 5000\)/);
  assert.match(bounded, /dialogOpened/);
  // the root side pushes the verdict with the ack, and the armed landings it keeps are bounded
  const agent = fs.readFileSync(path.join(JUGGLER, 'content/PageAgent.js'), 'utf8').replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');
  assert.match(agent, /if \(inputEvent\.jugglerEventId\)\s*inputEvent\.verdicts = this\._landingVerdictsFor\(\)/);
  assert.match(agent, /this\._armedLandings\.size > 8/);
});
