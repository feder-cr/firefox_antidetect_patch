// A key with a latency is delivered at once and born that many ms in the past (B299); no timer of the content process.
//
// PageAgent._dispatchKeyEvent used to create the KeyboardEvent, wait `latency` ms on a setTimeout of the content
// process, then hand it to the text input processor. The wait moved the delivery itself: under load the page saw gaps
// and dwells of the timer, not of the declared timing. Now the key is delivered in the same turn, and the latency is
// declared to the processor (nsITextInputProcessor.setNextKeyLatency, chrome only) just before the key; the
// processor stamps the key event with the time it is born with: timeStamp = now - latency. Nothing is added to the
// web-exposed KeyboardEventInit, and no line of Mozilla's is rewritten: the logic lives in StealthInputEventBirth.h.
//
// The method is the real one, taken out of PageAgent.js, run against fakes of the frame and the processor; the
// processor's side (IDL and C++) is checked on its source, as the engine is not built here.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const TREE = path.join(JUGGLER, '..');
const read = rel => fs.readFileSync(path.join(JUGGLER, rel), 'utf8');
const readTree = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');
// The code, not the comments that explain why it is gone.
const strip = text => text.replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

function bodyOf(source, header) {
  const at = source.indexOf(header);
  assert.ok(at >= 0, header + ' is in the source');
  const open = source.indexOf('{', source.indexOf(')', at));
  let depth = 0;
  for (let i = open; i < source.length; i++) {
    if (source[i] === '{') depth++;
    else if (source[i] === '}' && --depth === 0)
      return source.slice(at, i + 1);
  }
  throw new Error('unbalanced ' + header);
}

const agentSource = read('content/PageAgent.js');
const methodSource = bodyOf(agentSource, 'async _dispatchKeyEvent(');
const methodCode = strip(methodSource);

// the real method, with the timers it could reach replaced by ones that record
function agent() {
  const timers = [];
  const calls = [];
  const tip = {
    setNextKeyLatency: (...args) => calls.push(['setNextKeyLatency', ...args]),
    keydown: (...args) => calls.push(['keydown', ...args]),
    keyup: (...args) => calls.push(['keyup', ...args]),
    commitCompositionWith: (...args) => calls.push(['commitCompositionWith', ...args]),
  };
  class KeyboardEvent { constructor(type, init) { this.type = type; this.init = init; } }
  const frame = {textInputProcessor: () => tip, domWindow: () => ({KeyboardEvent})};
  const make = new Function('setTimeout', 'return {\n' + methodSource + '\n};');
  const object = make((...args) => { timers.push(args); });
  object._frameTree = {mainFrame: () => frame};
  return {object, timers, calls};
}

const base = {keyCode: 65, code: 'KeyA', key: 'a', repeat: false, location: 0};

test('no timer on the key path of _dispatchKeyEvent', () => {
  assert.doesNotMatch(methodCode, /setTimeout|setInterval|requestAnimationFrame|new Promise|\bawait\b/);
});

test('a keydown with a latency: declared, then delivered, in the same turn', () => {
  const {object, timers, calls} = agent();
  const promise = object._dispatchKeyEvent({...base, type: 'keydown', latency: 40});
  // not awaited: the processor was already called, no timer was asked for
  assert.deepEqual(calls.map(c => c[0]), ['setNextKeyLatency', 'keydown', 'setNextKeyLatency']);
  assert.deepEqual(calls[2].slice(1), [0], 'taken back after the key');
  assert.deepEqual(calls[0].slice(1), [40], 'the latency the key is born with');
  assert.equal(calls[1].length, 3, 'keydown(event, flags): its own signature is untouched');
  assert.equal(timers.length, 0);
  return promise;
});

test('a keyup with a latency: declared, then delivered, in the same turn', () => {
  const {object, timers, calls} = agent();
  const promise = object._dispatchKeyEvent({...base, type: 'keyup', latency: 12.5});
  assert.deepEqual(calls.map(c => c[0]), ['setNextKeyLatency', 'keyup', 'setNextKeyLatency']);
  assert.deepEqual(calls[2].slice(1), [0]);
  assert.deepEqual(calls[0].slice(1), [12.5]);
  assert.equal(calls[1].length, 3, 'keyup(event, flags)');
  assert.equal(timers.length, 0);
  return promise;
});

test('a keydown with text goes through commitCompositionWith, born in the past too', () => {
  const {object, timers, calls} = agent();
  const promise = object._dispatchKeyEvent({...base, type: 'keydown', text: 'é', latency: 25});
  assert.deepEqual(calls.map(c => c[0]), ['setNextKeyLatency', 'commitCompositionWith', 'setNextKeyLatency']);
  assert.deepEqual(calls[2].slice(1), [0]);
  assert.deepEqual(calls[0].slice(1), [25]);
  assert.equal(calls[1][1], 'é');
  assert.equal(calls[1].length, 3, 'commitCompositionWith(text, event)');
  assert.equal(timers.length, 0);
  return promise;
});

test('no latency (absent, 0, negative): 0 is declared, the key keeps its construction stamp', async () => {
  for (const latency of [undefined, 0, -3]) {
    const {object, calls} = agent();
    await object._dispatchKeyEvent({...base, type: 'keydown', latency});
    assert.deepEqual(calls[0], ['setNextKeyLatency', 0]);
    assert.equal(calls[1][0], 'keydown');
  }
});

test('the declared latency is taken back whatever happens: try/finally, setNextKeyLatency(0) last', async () => {
  assert.match(methodCode, /try \{[\s\S]*\} finally \{\s*tip\.setNextKeyLatency\(0\);\s*\}/);
  // a throw (keyup with text, unknown type) leaves the processor with no latency declared
  for (const event of [{type: 'keyup', text: 'x', latency: 30}, {type: 'keypress', latency: 30}]) {
    const {object, calls, timers} = agent();
    await assert.rejects(object._dispatchKeyEvent({...base, ...event}));
    assert.deepEqual(calls.at(-1), ['setNextKeyLatency', 0]);
    assert.equal(timers.length, 0);
  }
  // the processor throws at the key: the latency is still taken back, and the throw reaches the caller
  const {object, calls} = agent();
  const failing = {
    setNextKeyLatency: (...args) => calls.push(['setNextKeyLatency', ...args]),
    keydown: () => { throw new Error('boom'); },
  };
  object._frameTree = {mainFrame: () => ({textInputProcessor: () => failing, domWindow: () => ({KeyboardEvent: class {}})})};
  await assert.rejects(object._dispatchKeyEvent({...base, type: 'keydown', latency: 30}), /boom/);
  assert.deepEqual(calls, [['setNextKeyLatency', 30], ['setNextKeyLatency', 0]]);
});

test('the scope opens right after the chrome assert, before any early return, and zeroes the declared latency', () => {
  const code = strip(readTree('dom/base/TextInputProcessor.cpp'));
  for (const header of ['TextInputProcessor::Keydown(Event* aDOMKeyEvent', 'TextInputProcessor::Keyup(Event* aDOMKeyEvent',
                        'TextInputProcessor::CommitCompositionWith(const nsAString& aCommitString']) {
    const body = bodyOf(code, header);
    const scope = body.indexOf('StealthKeyBirthScope stealthBirth(mStealthNextKeyLatencyMs);');
    const assertAt = body.indexOf('MOZ_RELEASE_ASSERT(nsContentUtils::IsCallerChrome());');
    assert.ok(assertAt >= 0 && scope > assertAt, header + ': the scope follows the chrome assert');
    assert.equal(body.slice(assertAt, scope).replace(/MOZ_RELEASE_ASSERT\(nsContentUtils::IsCallerChrome\(\)\);/, '').trim(), '',
                 header + ': nothing between the assert and the scope');
    assert.doesNotMatch(body.slice(0, scope), /\breturn\b/, header + ': no early return before the scope');
  }
});

test('the latency stays the optional protocol field of the three key commands', () => {
  const protocol = read('protocol/Protocol.js');
  const fields = [...protocol.matchAll(/latency: t\.Optional\(t\.Number\)/g)];
  assert.ok(fields.length >= 3, 'dispatchKeyEvent, and the mouse and wheel commands, keep their latency');
  const at = protocol.indexOf('dispatchKeyEvent');
  assert.ok(at >= 0 && /latency: t\.Optional\(t\.Number\)/.test(protocol.slice(at, at + 700)),
            'dispatchKeyEvent declares latency as an optional number');
});

test('the interface gets one method; keydown, keyup and commitCompositionWith keep Mozilla\'s signatures', () => {
  const raw = readTree('dom/interfaces/base/nsITextInputProcessor.idl');
  assert.match(raw, /^ *void setNextKeyLatency\(in double aLatencyMs\);  \/\/ STEALTH-HOOK\(fingerprint\/input\/event-birth\)$/m);
  const flat = strip(raw).replace(/\s+/g, ' ');
  assert.match(flat, /unsigned long keydown\(in Event aKeyboardEvent, \[optional\] in unsigned long aKeyFlags\);/);
  assert.match(flat, /boolean keyup\(in Event aKeyboardEvent, \[optional\] in unsigned long aKeyFlags\);/);
  assert.match(flat, /boolean commitCompositionWith\(in AString aCommitString, \[optional\] in Event aKeyboardEvent, \[optional\] in unsigned long aKeyFlags\);/);
  assert.doesNotMatch(flat, /aLatencyMs\);.*aLatencyMs\);/, 'one declaration of the latency, not one per method');
});

test('the processor opens a birth scope in the three key methods and stamps the copy before it is dispatched', () => {
  const cpp = readTree('dom/base/TextInputProcessor.cpp');
  const code = strip(cpp);
  for (const header of ['TextInputProcessor::Keydown(Event* aDOMKeyEvent', 'TextInputProcessor::Keyup(Event* aDOMKeyEvent',
                        'TextInputProcessor::CommitCompositionWith(const nsAString& aCommitString']) {
    const body = bodyOf(code, header);
    assert.match(body, /StealthKeyBirthScope stealthBirth\(mStealthNextKeyLatencyMs\);/, header + ' opens the scope');
    assert.ok(body.indexOf('StealthKeyBirthScope') < body.indexOf('Internal('), header + ': the scope is open before the key is delivered');
  }
  for (const [header, dispatch] of [['nsresult TextInputProcessor::KeydownInternal(', 'DispatchKeyboardEvent(eKeyDown'],
                                    ['nsresult TextInputProcessor::KeyupInternal(', 'DispatchKeyboardEvent(eKeyUp']]) {
    const body = bodyOf(code, header);
    const stamp = body.indexOf('StealthKeyBirthScope::Stamp(keyEvent);');
    assert.ok(stamp > 0, header + ' stamps its copy');
    assert.ok(stamp < body.indexOf(dispatch), header + ' stamps before the dispatch');
    assert.ok(stamp > body.indexOf('WidgetKeyboardEvent keyEvent(aKeyboardEvent);'), header + ' stamps the copy, never the event it was given');
  }
  assert.match(code, /StealthSetNextKeyLatency\(mStealthNextKeyLatencyMs, aLatencyMs, nsContentUtils::IsCallerChrome\(\)\)/);
  assert.doesNotMatch(code, /setTimeout|NS_NewTimer|NS_DelayedDispatch/);
  // every line of ours in this Mozilla file is tagged
  for (const line of cpp.split('\n').filter(l => /Stealth/.test(l)))
    assert.match(line, /STEALTH-HOOK\(fingerprint\/input\/event-birth\)/, line);
  assert.match(readTree('dom/base/TextInputProcessor.h'), /double mStealthNextKeyLatencyMs = 0;  \/\/ STEALTH-HOOK\(fingerprint\/input\/event-birth\)/);
});

test('the birth lives in our header: one use, the cap of an hour, nothing for a latency of 0', () => {
  const header = strip(readTree('dom/base/StealthInputEventBirth.h'));
  assert.match(header, /aLatencyMs > 0 && aLatencyMs < 3600000/);
  assert.match(header, /aNextLatencyMs = 0;/, 'the declared latency is used once');
  assert.match(header, /static void Stamp\(WidgetEventTime& aEvent\)/);
  assert.match(header, /MOZ_RELEASE_ASSERT\(aIsCallerChrome\)/);
});

test('the web-exposed KeyboardEventInit and KeyboardEvent get nothing', () => {
  for (const rel of ['dom/webidl/KeyboardEvent.webidl', 'dom/webidl/UIEvent.webidl']) {
    const webidl = strip(readTree(rel));
    assert.doesNotMatch(webidl, /latency|born|birth/i, rel + ' stays as upstream');
  }
});
