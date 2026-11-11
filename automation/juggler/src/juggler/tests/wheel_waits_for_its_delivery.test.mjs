// The wheel takes the APZ route and its command answers once the process it went to has dispatched it.
//
// The wheel was sent with `DispatchEvent` (no hit test), so it stopped in the process of the top at the <iframe> element
// when the point was in an iframe of another process, and its command answered when the event was handed to the widget.
// With WHEEL_EVENT_ASYNC_ENABLED the hit test routes it, and the engine's own delivery barrier (the synthesized-event
// callback, called by the target process when it has dispatched the event) is what the command waits for, bounded as every
// wait on the page's input and OUTSIDE the activation chain, so the chain is not held while the page scrolls.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const source = fs.readFileSync(path.join(JUGGLER, 'protocol/PageHandler.js'), 'utf8');
const strip = text => text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

function methodText(name) {
  const at = source.search(new RegExp('^  ' + name.replace(/[[\]'.]/g, '\\$&') + '\\(', 'm'));
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

// `_awaitBounded` as written, with a clock and a page target we control.
function bounded({dialogs = [], timers}) {
  const listeners = [];
  const PageTarget = {Events: {DialogOpened: 'dialog'}};
  const helper = {
    on: (target, event, fn) => { const l = {event, fn}; listeners.push(l); return l; },
    removeListeners: offs => { for (const l of offs) listeners.splice(listeners.indexOf(l), 1); },
  };
  const make = new Function('helper', 'PageTarget', 'setTimeout', 'clearTimeout',
      'return {\n' + methodText('async _awaitBounded') + '\n};');
  const object = make(helper, PageTarget, timers.setTimeout, timers.clearTimeout);
  object._pageTarget = {dialogs: () => dialogs};
  return {object, listeners, openDialog: () => listeners.filter(l => l.event === 'dialog').forEach(l => l.fn())};
}

const fakeTimers = () => {
  const pending = new Set();
  return {
    setTimeout: (fn, ms) => { const t = {fn, ms}; pending.add(t); return t; },
    clearTimeout: t => pending.delete(t),
    fire: () => { for (const t of [...pending]) { pending.delete(t); t.fn(); } },
    pending,
  };
};

test('what is waited for resolves the wait, and nothing is left behind', async () => {
  const timers = fakeTimers();
  const {object, listeners} = bounded({timers});
  let resolve;
  const waiting = object._awaitBounded(new Promise(r => { resolve = r; }), 'the wheel event');
  resolve();
  assert.equal(await waiting, 'done');
  assert.equal(timers.pending.size, 0, 'the 5 s timer is cleared');
  assert.equal(listeners.length, 0, 'the dialog listener is removed');
});

test('five seconds without it is an error that names what did not happen', async () => {
  const timers = fakeTimers();
  const {object} = bounded({timers});
  const waiting = object._awaitBounded(new Promise(() => {}), 'the wheel event');
  assert.equal([...timers.pending][0].ms, 5000);
  timers.fire();
  await assert.rejects(waiting, /the page did not handle the wheel event within 5 s/);
});

test('a dialog ends the wait, open already or opening', async () => {
  const timers = fakeTimers();
  const already = bounded({timers, dialogs: [{}]});
  assert.equal(await already.object._awaitBounded(new Promise(() => {}), 'x'), 'dialog');
  const opening = bounded({timers});
  const waiting = opening.object._awaitBounded(new Promise(() => {}), 'x');
  opening.openDialog();
  assert.equal(await waiting, 'dialog');
  assert.equal(timers.pending.size, 0);
});

test('the wheel is sent through the APZ route with the delivery callback, and awaited outside the activation', () => {
  const code = strip(source);
  const wheel = code.slice(code.indexOf("['Page.dispatchWheelEvent']"));
  const body = wheel.slice(0, wheel.indexOf("['Page.insertText']"));
  assert.match(body, /win\.windowUtils\.WHEEL_EVENT_ASYNC_ENABLED/);
  assert.match(body, /WHEEL_EVENT_ASYNC_ENABLED\s*,\s*delivered\s*,\s*latency/);
  const activation = body.indexOf('activateAndRun');
  const closing = body.indexOf('{ muteNotificationsPopup: true }');
  const wait = body.indexOf("this._awaitBounded(deliveredP, 'the wheel event')");
  assert.ok(activation > 0 && closing > activation && wait > closing, 'the wait follows the activation');
});

test('the mouse ack wait is the same bounded wait', () => {
  const code = strip(source);
  const ack = code.slice(code.indexOf('async _awaitInputHandled('));
  assert.match(ack.slice(0, 300), /this\._awaitBounded\(this\._whenInputHit\(eventId, owner\), 'mouse event ' \+ eventId\)/);
});
