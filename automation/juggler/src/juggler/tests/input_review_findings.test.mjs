// Findings of the review of the input steps (6.A-6.E): each of these tests fails on fission/integration before them.
//
// - an ack is the page's own: the observer of the acks is the process's, and a process holds the documents of more than
//   one page, so the ack of another page's event must not end this page's wait;
// - the crash of an iframe's process leaves the page handler with nothing of the frame or of the frames below it;
// - the mux keeps no armed landing of a command that ended without reading its verdict;
// - an armed node of a document that was replaced is let go.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { ContentMux } from '../ContentMux.js';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const read = rel => fs.readFileSync(path.join(JUGGLER, rel), 'utf8');
const strip = text => text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

function methodText(source, name) {
  const at = source.search(new RegExp('^  ' + name.replace(/[[\]'.]/g, '\\$&') + '\\(', 'm'));
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

const handlerSource = read('protocol/PageHandler.js');
const agentSource = read('content/PageAgent.js');

function ackHandler(ownIds = []) {
  const methods = ['_onPageInputEvent', '_ackEndsWait', '_ownerIsPast', '_whenInputHit', '_landingVerdict', '_sendMouseEvent']
      .map(name => methodText(handlerSource, name)).join(',\n');
  const object = new Function('return {\n' + methods + '\n};')();
  const mux = new ContentMux({pageTarget: {}, name: 'x', createChannel: () => ({})});
  mux.mainRootId = () => 1;
  return Object.assign(object, {
    _lastHitEventId: 0, _ownEventIds: new Set(ownIds), _hitWaiters: new Set(), _landingVerdicts: new Map(),
    _contentChannel: mux, _pageEventSink: {emit() {}},
  });
}

// ---- #9 -------------------------------------------------------------------------------------------------------------

test('the ack of another page\'s event does not end the wait of this page', async () => {
  const h = ackHandler([10]);                 // this page sent event 10
  let done = false;
  const waiting = h._whenInputHit(10).then(() => { done = true; });
  // another page of the same content process had event 11 handled first; its ack is heard here too
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 11, verdicts: {1: {mousedown: {landed: true}}}}, {rootId: 1});
  await Promise.resolve();
  assert.equal(done, false, 'an ack of a larger id that this page never sent is not this page\'s event handled');
  assert.equal(h._lastHitEventId, 0);
  assert.equal(h._landingVerdicts.size, 0, 'nor is its verdict');
  h._onPageInputEvent({type: 'mousedown', jugglerEventId: 10}, {rootId: 1});
  await waiting;
  assert.equal(done, true);
});

test('an event replaced by coalescing is acked by the id of the one that replaced it: that is a wait ended', async () => {
  const h = ackHandler([20, 21, 22]);         // three moves sent; the engine kept the last one
  const waiting = h._whenInputHit(20);
  h._onPageInputEvent({type: 'mousemove', jugglerEventId: 22}, {rootId: 1});
  await waiting;
  assert.equal(h._lastHitEventId, 22);
  // The acked id is let go. The ids the engine folded into it are not acked by anyone: they stay until the record's bound.
  assert.deepEqual([...h._ownEventIds].sort(), [20, 21]);
});

test('every mouse event of the page is recorded as its own, and the record is bounded', () => {
  const h = ackHandler();
  let next = 100;
  const win = {windowUtils: {jugglerSendMouseEvent: () => ++next}};
  assert.equal(h._sendMouseEvent(win, 'mousedown', 1, 2), 101);
  assert.ok(h._ownEventIds.has(101));
  for (let i = 0; i < 400; i++)
    h._sendMouseEvent(win, 'mousemove', 1, 2);
  assert.equal(h._ownEventIds.size, 256);
  assert.ok(!h._ownEventIds.has(101), 'the oldest unacked ids go');
  const code = strip(handlerSource);
  assert.equal(code.split('windowUtils.jugglerSendMouseEvent(').length - 1, 1,
      'the mouse is sent through _sendMouseEvent alone');
});

// ---- F4 ---------------------------------------------------------------------------------------------------------------

test('the crash of an iframe makes the handler forget the frame and the frames below it, deepest first', () => {
  const at = handlerSource.indexOf('PageTarget.Events.FrameCrashed, ({ frameId }) => {');
  assert.ok(at > 0, 'the listener is a block');
  const open = handlerSource.indexOf('{', at + 'PageTarget.Events.FrameCrashed, ({ frameId }) => '.length - 1);
  let depth = 0, end = open;
  for (let i = open; i < handlerSource.length; i++) {
    if (handlerSource[i] === '{') depth++;
    else if (handlerSource[i] === '}' && --depth === 0) { end = i; break; }
  }
  const body = strip(handlerSource.slice(open + 1, end));
  const forgotten = [];
  const fake = {
    _frameLifecycle: {framesBelow: id => (id === 'f' ? [{frameId: 'g'}, {frameId: 'h'}] : [])},   // g under f, h under g
    _onFrameDetached: id => forgotten.push(id),
  };
  new Function('frameId', body).call(fake, 'f');
  assert.deepEqual(forgotten, ['h', 'g', 'f']);
});

test('the roots of the crashed frame end before the client is told', () => {
  const registry = strip(read('TargetRegistry.js'));
  const method = methodText(registry, 'frameCrashed');
  assert.match(method, /this\._contentMux\.disposeRootsOf\(browsingContext\);\s*this\.emit\(/);
});

// ---- F3 ---------------------------------------------------------------------------------------------------------------

test('the mux keeps no armed landing of a command that ended on its bound, on a dialog, or early', async () => {
  const h = ackHandler([5]);
  h._contentChannel._armed.set(7, {rootId: 2, rootToken: 3});
  // a wait that ended on a dialog (the real _awaitInputHandled, with the bounded wait faked to say so)
  const awaitText = methodText(handlerSource, 'async _awaitInputHandled');
  const object = new Function('return {\n' + awaitText + '\n};')();
  Object.assign(object, {
    _contentChannel: h._contentChannel, _landingVerdict: h._landingVerdict, _whenInputHit: () => new Promise(() => {}),
    _awaitBounded: async () => 'dialog',
  });
  assert.equal(await object._awaitInputHandled(5, 'mouseup', 7), null);
  assert.equal(h._contentChannel.armedEntry(7), undefined);

  // and one that ended on its bound (an error)
  h._contentChannel._armed.set(8, {rootId: 2, rootToken: 4});
  object._awaitBounded = async () => { throw new Error('the page did not handle mouse event 5 within 5 s'); };
  await assert.rejects(object._awaitInputHandled(5, 'mouseup', 8), /within 5 s/);
  assert.equal(h._contentChannel.armedEntry(8), undefined);

  // the command takes the landing it armed however it ends, also when nothing was sent
  const command = strip(methodText(handlerSource, "async ['Page.dispatchMouseEvent']"));
  assert.match(command, /try \{\s*return await this\._dispatchMouseEvent\(options, armed\);\s*\} finally \{\s*if \(armed\)\s*this\._contentChannel\.takeArmed\(armed\.token\);/);
});

// ---- #5 ---------------------------------------------------------------------------------------------------------------

test('an armed node of a document that was replaced is let go, one of the current document stays', () => {
  const pruneText = methodText(agentSource, '_pruneArmedLandings');
  const object = new Function('return {\n' + pruneText + '\n};')();
  const win = {};
  object._armedLandings = new Map([
    [1, {ownerDocument: {defaultView: win}}],              // in its window
    [2, {ownerDocument: {defaultView: null}}],              // a navigation replaced the document: it has no window
    [3, {ownerDocument: null}],                              // not a document's node any more
    [4, {get ownerDocument() { throw new Error('dead object'); }}],
  ]);
  object._pruneArmedLandings();
  assert.deepEqual([...object._armedLandings.keys()], [1]);
});

test('a node is not compared with the document of its window by identity (two views of one document differ)', () => {
  const body = strip(methodText(agentSource, '_pruneArmedLandings'));
  assert.doesNotMatch(body, /\.document\s*!==|!==\s*node\.ownerDocument/);
});

test('the armed nodes are pruned when a navigation commits, when a landing is armed, and when verdicts are read', () => {
  const code = strip(agentSource);
  assert.match(code, /helper\.on\(this\._frameTree, 'navigationcommitted', \(\) => this\._pruneArmedLandings\(\)\)/);
  assert.match(methodText(code, '_armLanding'), /this\._pruneArmedLandings\(\)/);
  assert.match(methodText(code, '_landingVerdictsFor'), /this\._pruneArmedLandings\(\)/);
});
