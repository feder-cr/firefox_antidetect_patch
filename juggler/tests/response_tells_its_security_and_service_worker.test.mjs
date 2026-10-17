// A response says what certificate served it and whether a service worker
// answered it, as upstream does.
//
// Since 2026-08-24 (B170) `securityDetails` was always null and
// `fromServiceWorker` always false, to save parsing a certificate per response
// and an observer: `response.security_details()` answered None over HTTPS and
// `response.from_service_worker` False for what a service worker answered -
// values, not refusals, so a script read them as true (B303 E15). The owner
// chose upstream's behaviour on 2026-10-16.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const code = fs.readFileSync(path.join(JUGGLER, 'NetworkObserver.js'), 'utf8')
    .replace(/\/\*[\s\S]*?\*\//g, '').replace(/\/\/[^\n]*/g, '');

const responseEvent = () => {
  const emit = code.slice(code.indexOf('pageNetwork.emit(PageNetwork.Events.Response'));
  return emit.slice(0, emit.indexOf('}, this._frameId)'));
};

test('the response carries the certificate that served it', () => {
  assert.match(responseEvent(), /securityDetails:\s*getSecurityDetails\(this\.httpChannel\)/);
  assert.match(code, /function getSecurityDetails\(httpChannel\)/);
});

test('the response says whether a service worker answered it', () => {
  assert.match(responseEvent(), /fromServiceWorker:\s*this\._fulfilledByServiceWorker/);
  // Gecko's own notification, the one the devtools read.
  assert.match(code, /addObserver\(this\._onServiceWorkerResponse\.bind\(this\), 'service-worker-synthesized-response'\)/);
});

test('the mark lives on the request, not in a set nothing empties', () => {
  // Upstream kept the ids in a Set emptied only when a response was emitted,
  // so a channel with no page left its id there for good.
  assert.doesNotMatch(code, /_channelIdsFulfilledByServiceWorker/);
  const handler = code.slice(code.indexOf('_onServiceWorkerResponse(channel'));
  const body = handler.slice(0, handler.indexOf('\n  }\n'));
  assert.match(body, /_fulfilledByServiceWorker = true/);
});
