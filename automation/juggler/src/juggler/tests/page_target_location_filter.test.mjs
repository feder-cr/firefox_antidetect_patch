// The progress listener a PageTarget registers on its <browser> (TargetRegistry.js), run from the real file: the
// real PageTarget constructor is executed in a vm context with the platform around it stubbed (the tab, the
// browser context, the services), and the listener it registers is the one called.
//
// The progress of a frame climbs to the browsing contexts above it (BrowsingContextWebProgress), so the <browser>'s
// listener hears the location changes of every frame of the page, whichever process holds it: onLocationChange
// takes the address as the page's own, and asks the page's permissions for it, so without the test on
// aWebProgress.isTopLevel the address of an iframe became the page's url. The listener is for locations only: the
// aborts of a navigation are relayed by the content process (frame_navigations.test.mjs), not taken from the
// document states here.
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

// nsIWebProgress notification flags.
const nsIWebProgress = {
  NOTIFY_STATE_DOCUMENT: 0x2,
  NOTIFY_STATE_NETWORK: 0x4,
  NOTIFY_PROGRESS: 0x10,
  NOTIFY_LOCATION: 0x80,
};

class EventEmitter {
  constructor() { this._handlers = new Map(); }
  on(name, handler) { this._handlers.set(name, [...(this._handlers.get(name) || []), handler]); }
  off(name, handler) { this._handlers.set(name, (this._handlers.get(name) || []).filter(h => h !== handler)); }
  once() {}
  emit(name, ...args) { for (const handler of this._handlers.get(name) || []) handler(name, ...args); }
}

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({ generateUUID: () => '{target-uuid}' }) } },
  Ci: {},
  Services: { obs: { addObserver() {}, removeObserver() {} } },
  ChromeUtils: { importESModule: () => ({ EventEmitter }) },
}, ['Helper']);

const { PageTarget } = load('TargetRegistry.js', {
  Components: { results: {}, interfaces: {}, utils: {} },
  Ci: { nsIWebProgress },
  Cc: {},
  Services: {
    obs: { addObserver() {}, removeObserver() {} },
    ppmm: { sharedData: { has: () => false, delete() {}, flush() {} } },
  },
  ChromeUtils: {
    // The listener's own interface, so the object stays what it is.
    generateQI: () => () => {},
    importESModule: url => {
      if (url.endsWith('/juggler/Helper.js'))
        return { Helper };
      if (url.endsWith('/StealthCursor.js'))
        return { StealthCursor: { forWindow: () => ({}), releaseWindow() {} } };
      if (url.endsWith('/ContentMux.js'))
        return { ContentMux: class { rootIds() { return []; } } };
      return new Proxy({}, { get: () => class {} });
    },
  },
  dump: () => {},
}, ['PageTarget']);

// A PageTarget on stubbed platform, and what it registered on its <browser>.
function pageTarget() {
  const registered = [];
  const removed = [];
  const permissionsAsked = [];
  const linkedBrowser = {
    browsingContext: { browserId: 7, overrideDPPX: 1 },
    ownerDocument: { getElementById: () => null },
    addProgressListener: (listener, flags) => registered.push({ listener, flags }),
    removeProgressListener: listener => removed.push(listener),
    addEventListener() {},
    removeEventListener() {},
  };
  const tab = { linkedBrowser };
  const win = { gBrowser: {}, addEventListener() {}, removeEventListener() {} };
  const registry = {
    _browserToTarget: new Map(),
    _browserIdToTarget: new Map(),
    _rootActors: new Map(),
    emit() {},
  };
  const browserContext = {
    pages: new Set(),
    grantPermissionsToOrigin: url => permissionsAsked.push(url),
  };
  const target = new PageTarget(registry, win, tab, browserContext, null);
  return { target, registered, removed, permissionsAsked, linkedBrowser };
}

const location = spec => ({ spec });

test('the target registers one listener on its <browser>, for the locations and nothing else', () => {
  const { registered } = pageTarget();
  assert.equal(registered.length, 1);
  const { flags } = registered[0];
  assert.equal(flags, nsIWebProgress.NOTIFY_LOCATION);
  // Not more than it needs: the document states, every request or the progress would call the parent for each.
  assert.equal(flags & nsIWebProgress.NOTIFY_STATE_DOCUMENT, 0);
  assert.equal(flags & nsIWebProgress.NOTIFY_STATE_NETWORK, 0);
  assert.equal(flags & nsIWebProgress.NOTIFY_PROGRESS, 0);
  assert.equal(typeof registered[0].listener.onLocationChange, 'function');
  assert.equal(registered[0].listener.onStateChange, undefined);
});

test('the location of the top-level browsing context is the page\'s url, and its permissions are asked', () => {
  const { target, registered, permissionsAsked } = pageTarget();
  assert.equal(target._url, 'about:blank');
  registered[0].listener.onLocationChange({ isTopLevel: true }, {}, location('http://a.test/page'), 0);
  assert.equal(target._url, 'http://a.test/page');
  assert.deepEqual(permissionsAsked, ['http://a.test/page']);
});

test('the location of a frame is not the page\'s url, and its permissions are not asked', () => {
  const { target, registered, permissionsAsked } = pageTarget();
  registered[0].listener.onLocationChange({ isTopLevel: true }, {}, location('http://a.test/page'), 0);
  registered[0].listener.onLocationChange({ isTopLevel: false }, {}, location('http://ads.test/frame'), 0);
  assert.equal(target._url, 'http://a.test/page');
  assert.deepEqual(permissionsAsked, ['http://a.test/page']);
});

test('the target no longer has a state event, and disposing it takes the listener off the <browser>', () => {
  assert.equal(PageTarget.Events.StateChange, undefined);
  const { target, registered, removed } = pageTarget();
  target.dispose();
  assert.deepEqual(removed, [registered[0].listener]);
});
