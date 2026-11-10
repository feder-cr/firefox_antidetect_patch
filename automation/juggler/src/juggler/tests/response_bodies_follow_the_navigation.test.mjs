// What the parent process keeps of the network of a tab, loaded the way it loads NetworkObserver.js and
// PageHandler.js, with the real Helper.
//
// After 95 navigations in one reused tab the parent held 5,000 stored responses and 105 MB of their bodies, in
// strings of the JS heap (compressed bytes, HTML, CSS of pages the tab had left; a retail Firefox holds none of
// them): ResponseStorage kept one entry per request for good and dropped bodies only when the tab's total went
// over its cap. Chromium clears a tab's bodies when the main frame commits a navigation (InspectorNetworkAgent::
// DidCommitLoad -> NetworkResourcesData::Clear, keeping the loader that committed): the body of a document the tab
// left is no longer readable. The same here, with Network.getResponseBody answering "evicted" as it does over the
// cap, which Playwright turns into "Response body for ... was evicted!".
//
// Also: the map of redirects a request expects kept a request, with its channel and load info, when the new
// channel never came or was cancelled for a service worker.
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

const Cr = { NS_BINDING_ABORTED: 0x804b0002, NS_ERROR_FAILURE: 0x80004005 };
const Ci = {
  nsITransportSecurityInfo: { TLS_VERSION_1: 1, TLS_VERSION_1_1: 2, TLS_VERSION_1_2: 3, TLS_VERSION_1_3: 4 },
  // The fake requests below are not encoded channels: addResponseBody stores their bodies as they are.
  nsIEncodedChannel: class {},
};

class EventEmitter {
  on() {}
  off() {}
  once() {}
  emit() {}
}

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci, Cr,
  ChromeUtils: { importESModule: () => ({ EventEmitter }) },
}, ['Helper']);

const { PageNetwork, NetworkObserver, NetworkRequest, ResponseStorage } = load('NetworkObserver.js', {
  Components: { classes: {}, interfaces: Ci, utils: {}, results: Cr, manager: {},
                Constructor: () => function () {} },
  ChromeUtils: { importESModule: () => ({ Helper, NetUtil: {}, ChannelEventSinkFactory: {}, newProxyInfoFor: () => null }) },
  Services: {},
  btoa: text => Buffer.from(text, 'binary').toString('base64'),
  dump: () => {},
}, ['PageNetwork', 'NetworkObserver', 'NetworkRequest', 'ResponseStorage']);

// What a module evaluated in another context answers has that context's prototypes: compared as plain data.
const plain = value => JSON.parse(JSON.stringify(value));

const MB = 1024 * 1024;

function request(requestId, navigationId) {
  return { requestId, navigationId, httpChannel: {}, _fulfilled: false };
}

function stored(storage, requestId, navigationId, body) {
  storage.addResponseBody(request(requestId, navigationId), body);
}

test('the bodies of the documents a tab left are evicted when the main frame commits a navigation', () => {
  const storage = new ResponseStorage(100 * MB, 10 * MB);
  stored(storage, '1', 'nav-A', 'document A');
  stored(storage, '2', undefined, 'a stylesheet of A');
  stored(storage, '3', 'nav-B', 'document B');
  assert.equal(storage._totalSize, 'document A'.length + 'a stylesheet of A'.length + 'document B'.length);
  storage.evictBeforeNavigation('nav-B');
  assert.deepEqual(plain(storage.getBase64EncodedResponse('1')), { base64body: '', evicted: true });
  assert.deepEqual(plain(storage.getBase64EncodedResponse('2')), { base64body: '', evicted: true });
  assert.equal(storage.getBase64EncodedResponse('3').base64body, Buffer.from('document B').toString('base64'),
      'the document that committed keeps its body');
  assert.equal(storage._totalSize, 'document B'.length, 'the total counts what is still held');
});

test('a page of the new document asked for after the commit is stored and read as before', () => {
  const storage = new ResponseStorage(100 * MB, 10 * MB);
  stored(storage, '1', 'nav-A', 'document A');
  storage.evictBeforeNavigation('nav-B');
  stored(storage, '2', undefined, 'a script of B');
  assert.equal(storage.getBase64EncodedResponse('2').base64body, Buffer.from('a script of B').toString('base64'));
  assert.equal(storage.getBase64EncodedResponse('1').evicted, true);
});

test('a commit without a navigation id says nothing about a navigation and evicts nothing', () => {
  // The agent reports the frame it has just met (a new content process) with no id.
  const storage = new ResponseStorage(100 * MB, 10 * MB);
  stored(storage, '1', 'nav-A', 'document A');
  storage.evictBeforeNavigation(undefined);
  assert.equal(storage.getBase64EncodedResponse('1').base64body, Buffer.from('document A').toString('base64'));
  assert.equal(storage._totalSize, 'document A'.length);
});

test('a restored document, which was never requested, evicts everything stored before it', () => {
  const storage = new ResponseStorage(100 * MB, 10 * MB);
  stored(storage, '1', 'nav-A', 'document A');
  storage.evictBeforeNavigation('bfcache-1');
  assert.equal(storage.getBase64EncodedResponse('1').evicted, true);
  assert.equal(storage._totalSize, 0);
});

test("what the new document's requests stored before the commit arrived stays readable", () => {
  // A sub-resource has no navigation id of its own, and its body can be stored before the parent hears of the commit.
  const storage = new ResponseStorage(100 * MB, 10 * MB);
  stored(storage, '1', 'nav-A', 'document A');
  stored(storage, '2', undefined, 'a script of A');
  storage.noteNavigationStart('nav-B');
  stored(storage, '3', 'nav-B', 'document B');
  stored(storage, '4', undefined, 'a script of B, early');
  storage.evictBeforeNavigation('nav-B');
  assert.equal(storage.getBase64EncodedResponse('1').evicted, true);
  assert.equal(storage.getBase64EncodedResponse('2').evicted, true);
  assert.equal(storage.getBase64EncodedResponse('3').evicted, undefined);
  assert.equal(storage.getBase64EncodedResponse('4').base64body, Buffer.from('a script of B, early').toString('base64'));
  assert.equal(storage._totalSize, 'document B'.length + 'a script of B, early'.length);
});

test('a navigation that never commits leaves only a bounded number of marks', () => {
  const storage = new ResponseStorage(100 * MB, 10 * MB);
  for (let i = 0; i < 500; i++)
    storage.noteNavigationStart('aborted-' + i);
  assert.ok(storage._navigationStarts.size <= 100);
  assert.equal(storage._navigationStarts.has('aborted-499'), true);
});

test('the markers of evicted bodies go first when there are too many entries, the oldest first', () => {
  const storage = new ResponseStorage(100 * MB, 10 * MB, 3);
  stored(storage, '1', 'nav-A', 'body 1');
  stored(storage, '2', 'nav-A', 'body 2');
  storage.evictBeforeNavigation('nav-B');  // both are markers now
  stored(storage, '3', 'nav-B', 'body 3');
  stored(storage, '4', 'nav-B', 'body 4');
  assert.deepEqual(plain([...storage._responses.keys()]), ['2', '3', '4'], 'the oldest marker went, the bodies stayed');
  assert.equal(storage.getBase64EncodedResponse('3').base64body, Buffer.from('body 3').toString('base64'));
  assert.equal(storage._totalSize, 'body 3'.length + 'body 4'.length);
});

test('with a body in every entry the oldest bodies go, and the total goes down with them', () => {
  const storage = new ResponseStorage(100 * MB, 10 * MB, 3);
  for (let i = 1; i <= 5; i++)
    stored(storage, String(i), 'nav-A', 'body ' + i);
  assert.equal(storage._responses.size, 3, 'entries never pass the cap');
  assert.equal(storage._totalSize, 3 * 'body 1'.length, 'the total counts only the bodies that are held');
  assert.throws(() => storage.getBase64EncodedResponse('1'), /is not found/);
  assert.equal(storage.getBase64EncodedResponse('5').base64body, Buffer.from('body 5').toString('base64'));
});

test('a response over the size of one response is a marker and counts against the cap of entries', () => {
  const storage = new ResponseStorage(100 * MB, 4, 2);
  stored(storage, '1', 'nav-A', 'too long for one');
  stored(storage, '2', 'nav-A', 'again too long');
  stored(storage, '3', 'nav-A', 'ok');
  assert.equal(storage._responses.size, 2);
  assert.equal(storage.getBase64EncodedResponse('3').base64body, Buffer.from('ok').toString('base64'));
});

test('PageNetwork passes the commit of the main frame to its response storage', () => {
  const calls = [];
  const pageNetwork = Object.create(PageNetwork.prototype);
  pageNetwork._responseStorage = { evictBeforeNavigation: id => calls.push(id) };
  pageNetwork.onMainFrameNavigationCommitted('nav-C');
  assert.deepEqual(calls, ['nav-C']);
});

test('a request that ended takes with it the redirect the observer expected of it', () => {
  const observer = Object.create(NetworkObserver.prototype);
  observer._channelToRequest = new Map();
  observer._expectedRedirect = new Map();
  const request = Object.create(NetworkRequest.prototype);
  request.requestId = '7';
  request.httpChannel = {};
  request._pageNetwork = { _interceptedRequests: new Map() };
  request._networkObserver = observer;
  const other = { requestId: '8' };
  observer._expectedRedirect.set('100', request);
  observer._expectedRedirect.set('101', other);
  request._ended();
  assert.equal(observer._expectedRedirect.has('100'), false, 'the redirect expected of the ended request is gone');
  assert.equal(observer._expectedRedirect.get('101'), other, 'the one expected of another request stays');
});

// PageHandler: the commit event of the main frame reaches the page's network; a subframe's does not.
const anything = new Proxy(function () {}, { get: () => class {} });
const { PageHandler } = load('protocol/PageHandler.js', {
  Components: { classes: {}, interfaces: {}, utils: {}, results: {} },
  ChromeUtils: { importESModule: () => anything },
  dump: () => {},
}, ['PageHandler']);

const helper = new Helper();
const mainFrameId = helper.browsingContextToFrameId({ browserId: 3, id: 7, parent: null });
const subframeId = helper.browsingContextToFrameId({ browserId: 3, id: 12, parent: { id: 7 } });

function handler() {
  const emitted = [];
  const committed = [];
  const pageHandler = Object.create(PageHandler.prototype);
  pageHandler._session = { emitEvent: (name, params) => emitted.push([name, params.frameId]) };
  pageHandler._pageNetwork = { onMainFrameNavigationCommitted: id => committed.push(id) };
  // The frames the client has been told of: a frame's events leave once it is announced.
  pageHandler._reportedFrameIds = new Set([mainFrameId, subframeId]);
  pageHandler._frameEventsBeforeFrame = new Map();
  pageHandler._liveFrameIds = new Set();
  pageHandler._parentOfFrame = new Map();
  return { pageHandler, emitted, committed };
}

test('the navigation commit of the main frame evicts the bodies, and is still reported to the client', () => {
  const { pageHandler, emitted, committed } = handler();
  pageHandler._onNavigationCommitted({ frameId: mainFrameId, navigationId: 'nav-D', url: 'https://example.com/' });
  assert.deepEqual(emitted, [['Page.navigationCommitted', mainFrameId]]);
  assert.deepEqual(committed, ['nav-D']);
});

test('the navigation commit of a subframe leaves the bodies of the page where they are', () => {
  const { pageHandler, emitted, committed } = handler();
  pageHandler._onNavigationCommitted({ frameId: subframeId, navigationId: 'nav-E', url: 'https://example.com/frame' });
  assert.deepEqual(emitted, [['Page.navigationCommitted', subframeId]]);
  assert.deepEqual(committed, []);
});

test('the bodies are evicted even when the session refuses the event', () => {
  const { pageHandler, committed } = handler();
  pageHandler._session = { emitEvent: () => { throw new Error('Session has been disposed'); } };
  assert.throws(() => pageHandler._onNavigationCommitted({ frameId: mainFrameId, navigationId: 'nav-F' }), /Session has been disposed/);
  assert.deepEqual(committed, ['nav-F']);
});

test("the content process's commit of a navigation is the one that reaches the handler", () => {
  // The registration, read in the code: with `emitProtocolEvent` here the event would reach the client and not
  // the page's network.
  const source = fs.readFileSync(path.join(ROOT, 'protocol/PageHandler.js'), 'utf8');
  assert.match(source, /pageNavigationCommitted:\s*this\._onNavigationCommitted\.bind\(this\)/);
});
