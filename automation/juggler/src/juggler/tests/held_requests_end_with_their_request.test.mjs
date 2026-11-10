// A request held for interception, loaded the way the parent process loads
// NetworkObserver.js, with the real Helper.
//
// Two defects of the held map (PageNetwork._interceptedRequests), read in the
// source and worked around by a client that had to drop the engine's copy
// itself and guess the engine's state after a
// failed Network.setRequestInterception:
// - a request that ended while held (the page cancelled it, a navigation or
//   the tab took it) stayed in the map: only a command or
//   disableRequestInterception took it out;
// - disableRequestInterception turned the flag off, then resumed every held
//   request with no guard: one channel that could no longer be resumed threw,
//   the command failed with the flag already off, and the requests after it
//   stayed held for good.
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
const Ci = { nsITransportSecurityInfo: { TLS_VERSION_1: 1, TLS_VERSION_1_1: 2, TLS_VERSION_1_2: 3, TLS_VERSION_1_3: 4 } };

// What decorateAsEventEmitter needs: the events this page network emits.
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

const { PageNetwork, NetworkRequest } = load('NetworkObserver.js', {
  Components: { classes: {}, interfaces: Ci, utils: {}, results: Cr, manager: {},
                Constructor: () => function () {} },
  ChromeUtils: { importESModule: () => ({ Helper, NetUtil: {}, ChannelEventSinkFactory: {}, newProxyInfoFor: () => null }) },
  Services: {},
  dump: () => {},
}, ['PageNetwork', 'NetworkRequest']);

// A held request as far as the map needs it: the real methods on an object
// that skips the constructor's channel plumbing.
function held(pageNetwork, requestId, channel) {
  const request = Object.create(NetworkRequest.prototype);
  request.requestId = requestId;
  request.httpChannel = { responseEndTime: undefined };
  request._pageNetwork = pageNetwork;
  request._frameId = 'frame-1';
  request._interceptedChannel = channel;
  request._networkObserver = {
    _channelToRequest: new Map([[request.httpChannel, request]]),
    _forgetExpectedRedirects() {},
  };
  pageNetwork._interceptedRequests.set(requestId, request);
  return request;
}

const channel = (log, name, broken) => ({
  resetInterceptionWithURI() {
    if (broken)
      throw new Error('NS_ERROR_NOT_AVAILABLE');
    log.push(name);
  },
});

function pageNetwork() {
  const p = new PageNetwork({});
  p.enableRequestInterception();
  return p;
}

test('a held request that fails is no longer held', () => {
  const p = pageNetwork();
  const r = held(p, '11', channel([], '11'));
  r._sendOnRequestFailed(Cr.NS_BINDING_ABORTED);
  assert.equal(p._interceptedRequests.has('11'), false);
  assert.equal(r._interceptedChannel, undefined);
  assert.throws(() => p.abortInterceptedRequest('11', 'failed'), /Cannot find request "11"/);
});

test('a held request that finishes is no longer held', () => {
  const p = pageNetwork();
  const r = held(p, '12', channel([], '12'));
  r._sendOnRequestFinished();
  assert.equal(p._interceptedRequests.has('12'), false);
});

test('the end of a request leaves another request held under its id alone', () => {
  const p = pageNetwork();
  const ended = held(p, '13', channel([], 'a'));
  const other = held(p, '13', channel([], 'b'));   // the map now names `other`
  ended._sendOnRequestFailed(Cr.NS_BINDING_ABORTED);
  assert.equal(p._interceptedRequests.get('13'), other);
});

test('turning interception off lets every held request go, even past one that cannot be resumed', () => {
  const p = pageNetwork();
  const resumed = [];
  held(p, '21', channel(resumed, '21'));
  held(p, '22', channel(resumed, '22', true));
  held(p, '23', channel(resumed, '23'));
  assert.doesNotThrow(() => p.disableRequestInterception());
  assert.deepEqual(resumed, ['21', '23']);
  assert.equal(p._interceptedRequests.size, 0);
  assert.equal(p._requestInterceptionEnabled, false);
});
