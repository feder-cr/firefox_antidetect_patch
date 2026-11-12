// A resumed request whose replaced channel is cancelled before the replacement starts, loaded the way the parent process loads
// NetworkObserver.js, with the real Helper.
//
// Resuming an intercepted request replaces its channel by an internal redirect (`_onInternalRedirect`): the request follows the new
// channel and waits for it to reach http-on-modify-request (`_onInternalRedirectReady`). A beacon sent while its document is
// being left is cancelled with the document: the old channel's onStopRequest comes with NS_BINDING_ABORTED, the new channel never
// starts, and that stop was dropped because `aRequest` was no longer the request's channel. Nothing ever ended the request: no
// requestFinished and no requestFailed, so a client that waits for the network to be idle waited for it for ever (measured with a
// log in the engine: 33 of 196 beacons of a page that sends beacons from a frame it leaves, under a load of other browsers).
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


function resumedRequest(events) {
  const request = Object.create(NetworkRequest.prototype);
  const oldChannel = { responseEndTime: undefined };
  const pageNetwork = {
    emit: (event, payload) => events.push([event, payload]),
    _interceptedRequests: new Map(),
  };
  request.requestId = '42';
  request.httpChannel = oldChannel;
  request._pageNetwork = pageNetwork;
  request._frameId = 'frame-1';
  request._responseBodyChunks = [];
  request._originalListener = { onStopRequest() {} };
  request._networkObserver = {
    _channelToRequest: new Map([[oldChannel, request]]),
    _forgetExpectedRedirects() {},
  };
  request._expectingResumedRequest = { method: undefined, headers: undefined, postData: undefined };
  return { request, oldChannel };
}

test('the channel a resumed request replaced is cancelled before the replacement starts: the request failed', () => {
  const events = [];
  const { request, oldChannel } = resumedRequest(events);
  request._onInternalRedirect({ responseEndTime: undefined });           // resume(): the new channel is not ready yet
  request.onStopRequest(oldChannel, Cr.NS_BINDING_ABORTED);              // the document went away
  assert.equal(events.length, 1, JSON.stringify(events));
  assert.equal(events[0][0], PageNetwork.Events.RequestFailed);
  assert.equal(events[0][1].requestId, '42');
});

test('a clean stop of the replaced channel, or a replacement that started, is not a failure', () => {
  const events = [];
  const { request, oldChannel } = resumedRequest(events);
  request._onInternalRedirect({ responseEndTime: undefined });
  request.onStopRequest(oldChannel, 0);                                  // a clean end of the old channel: nothing to report
  assert.equal(events.length, 0);

  const started = [];
  const second = resumedRequest(started);
  second.request._onInternalRedirect({ responseEndTime: undefined });
  second.request._expectingResumedRequest = undefined;                   // _onInternalRedirectReady ran: the replacement is on its way
  second.request.onStopRequest(second.oldChannel, Cr.NS_BINDING_ABORTED);
  assert.equal(started.length, 0);
});
