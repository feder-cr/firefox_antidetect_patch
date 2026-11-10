// FrameLifecycle: the parent process says which frames a page has, from the browsing contexts it is told of.
//
// Until now only the content process of the top announced `Page.frameAttached` and `Page.frameDetached`, so a frame
// held by another process (a site of its own, once the engine isolates them) was never announced. The parent is
// notified of every browsing context that attaches or is discarded, whatever process holds it. This loads the real
// FrameLifecycle and the real Helper that names frames, with browsing contexts as plain objects and the observer
// service as a list of handlers the test fires.
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

class EventEmitter {
  constructor() { this.handlers = new Map(); }
  on(name, handler) { this.handlers.set(name, [...(this.handlers.get(name) || []), handler]); }
  off() {}
  once() {}
  // The toolkit's EventEmitter calls a listener with the name of the event first, then what was emitted.
  emit(name, payload) { for (const handler of this.handlers.get(name) || []) handler(name, payload); }
}

// The observer service: the handlers by topic, fired by the test.
function observers() {
  const byTopic = new Map();
  return {
    addObserver: (handler, topic) => byTopic.set(topic, [...(byTopic.get(topic) || []), handler]),
    removeObserver: (handler, topic) => byTopic.set(topic, (byTopic.get(topic) || []).filter(h => h !== handler)),
    notify: (browsingContext, topic, why) => { for (const handler of byTopic.get(topic) || []) handler(browsingContext, topic, why); },
    count: topic => (byTopic.get(topic) || []).length,
  };
}

function lifecycleWith(obs) {
  const modules = { 'resource://gre/modules/EventEmitter.sys.mjs': { EventEmitter } };
  const { Helper } = load('Helper.js', {
    Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
    Ci: {},
    Services: { obs },
    ChromeUtils: { importESModule: url => modules[url] || {} },
  }, ['Helper']);
  modules['resource://gre/modules/juggler/Helper.js'] = { Helper };
  const { FrameLifecycle } = load('FrameLifecycle.js', {
    ChromeUtils: { importESModule: url => modules[url] },
  }, ['FrameLifecycle']);
  return FrameLifecycle;
}

let nextId = 100;
function context({ parent = null, browserId = 9, extra = {} } = {}) {
  const bc = { id: nextId++, parent, browserId, isContent: true, webProgress: {}, currentRemoteType: 'web', children: [], ...extra };
  if (parent) parent.children.push(bc);
  return bc;
}

function start(top) {
  const obs = observers();
  const FrameLifecycle = lifecycleWith(obs);
  const lifecycle = new FrameLifecycle({ browsingContext: top });
  const events = [];
  lifecycle.on('frameattached', (_, p) => events.push(['attached', p.frameId, p.parentFrameId]));
  lifecycle.on('framedetached', (_, p) => events.push(['detached', p.frameId]));
  lifecycle.on('framedropped', (_, p) => events.push(['dropped', p.frameId]));
  return { obs, lifecycle, events };
}

test('the constructor emits nothing, start() emits the main frame first and then the frames already there, parents first', () => {
  const top = context();
  const a = context({ parent: top });
  const b = context({ parent: a });
  const c = context({ parent: top });
  const obs = observers();
  const FrameLifecycle = lifecycleWith(obs);
  const lifecycle = new FrameLifecycle({ browsingContext: top });
  const events = [];
  lifecycle.on('frameattached', (_, p) => events.push([p.frameId, p.parentFrameId]));
  assert.deepEqual(events, [], 'the constructor is silent: a listener registered right after it misses nothing');
  lifecycle.start();
  assert.deepEqual(events, [
    ['mainframe-9', undefined],
    ['subframe-' + a.id, 'mainframe-9'],
    ['subframe-' + b.id, 'subframe-' + a.id],
    ['subframe-' + c.id, 'mainframe-9'],
  ]);
});

test('a listener registered after the constructor and before start() gets the main frame (the event is not lost between the two)', () => {
  const { lifecycle, events } = start(context());
  assert.deepEqual(events, []);
  lifecycle.start();
  assert.deepEqual(events, [['attached', 'mainframe-9', undefined]]);
});

test('a frame attached later is announced with its parent, and only once', () => {
  const top = context();
  const { obs, lifecycle, events } = start(top);
  lifecycle.start();
  const child = context({ parent: top });
  obs.notify(child, 'browsing-context-attached', 'attach');
  obs.notify(child, 'browsing-context-attached', 'attach');
  assert.deepEqual(events, [['attached', 'mainframe-9', undefined], ['attached', 'subframe-' + child.id, 'mainframe-9']]);
});

test('the top replaced (cross-group): the main frame is not announced again nor detached, the frames of the new top are announced', () => {
  const oldTop = context();
  const { obs, lifecycle, events } = start(oldTop);
  lifecycle.start();
  const newTop = context(); // same browser, another browsing context
  obs.notify(newTop, 'browsing-context-attached', 'replace');
  obs.notify(oldTop, 'browsing-context-discarded', 'replace');
  const frameOfNewTop = context({ parent: newTop });
  obs.notify(frameOfNewTop, 'browsing-context-attached', 'attach');
  assert.deepEqual(events, [['attached', 'mainframe-9', undefined], ['attached', 'subframe-' + frameOfNewTop.id, 'mainframe-9']],
      'the main frame is the browser\'s: it stays, and the iframe of the new top is not lost because its top is not the old one');
});

test('a child told of before its parent: both announced, the parent first', () => {
  const top = context();
  const { obs, lifecycle, events } = start(top);
  lifecycle.start();
  const parent = context({ parent: top });
  const child = context({ parent });
  obs.notify(child, 'browsing-context-attached', 'attach');
  obs.notify(parent, 'browsing-context-attached', 'attach');
  assert.deepEqual(events.slice(1), [
    ['attached', 'subframe-' + parent.id, 'mainframe-9'],
    ['attached', 'subframe-' + child.id, 'subframe-' + parent.id],
  ]);
});

test('a frame of another tab, a chrome document and an extension page are not this page\'s frames, nor is anything below them', () => {
  const top = context();
  const { obs, lifecycle, events } = start(top);
  lifecycle.start();
  const elsewhere = context({ parent: context({ browserId: 77 }), browserId: 77 });
  const chrome = context({ parent: top, extra: { isContent: false, webProgress: null } });
  const extension = context({ parent: top, extra: { currentRemoteType: 'extension' } });
  const belowExtension = context({ parent: extension });
  const belowChrome = context({ parent: chrome });
  for (const bc of [elsewhere, chrome, extension, belowExtension, belowChrome])
    obs.notify(bc, 'browsing-context-attached', 'attach');
  assert.deepEqual(events, [['attached', 'mainframe-9', undefined]]);
});

test('a frame discarded detaches its descendants first, once each, and the main frame is never detached', () => {
  const top = context();
  const a = context({ parent: top });
  const b = context({ parent: a });
  const c = context({ parent: b });
  const { obs, lifecycle, events } = start(top);
  lifecycle.start();
  events.length = 0;
  obs.notify(a, 'browsing-context-discarded', 'discard');
  obs.notify(a, 'browsing-context-discarded', 'discard');
  obs.notify(top, 'browsing-context-discarded', 'discard');
  assert.deepEqual(events.filter(event => event[0] === 'detached'), [
    ['detached', 'subframe-' + c.id],
    ['detached', 'subframe-' + b.id],
    ['detached', 'subframe-' + a.id],
  ]);
});

test('a context of this page discarded before it was announced is dropped, so what waits for it can go', () => {
  const top = context();
  const { obs, lifecycle, events } = start(top);
  lifecycle.start();
  const chrome = context({ parent: top, extra: { isContent: false, webProgress: null } });
  const stranger = context({ parent: context({ browserId: 77 }), browserId: 77 });
  obs.notify(chrome, 'browsing-context-discarded', 'discard');
  obs.notify(stranger, 'browsing-context-discarded', 'discard');
  assert.deepEqual(events.slice(1), [['dropped', 'subframe-' + chrome.id]]);
});

test('a discarded context whose browserId cannot be read is ignored', () => {
  const top = context();
  const { obs, lifecycle, events } = start(top);
  lifecycle.start();
  const gone = { id: 5, parent: top, get browserId() { throw new Error('discarded'); } };
  obs.notify(gone, 'browsing-context-discarded', 'discard');
  assert.deepEqual(events.slice(1), []);
});

test('dispose() removes both observers', () => {
  const top = context();
  const { obs, lifecycle } = start(top);
  lifecycle.start();
  assert.equal(obs.count('browsing-context-attached'), 1);
  assert.equal(obs.count('browsing-context-discarded'), 1);
  lifecycle.dispose();
  assert.equal(obs.count('browsing-context-attached'), 0);
  assert.equal(obs.count('browsing-context-discarded'), 0);
});
