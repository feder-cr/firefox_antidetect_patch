// A position a page reads has the timestamp of the moment it is delivered:
// position.timestamp - Date.now() is between -2 and 0 ms, like a retail fix that
// the provider stamps with Date.now() when it hands it over (the network provider
// does: NetworkGeolocationProvider.sys.mjs, cachedLocation.timestamp = Date.now()).
//
// The override was built with `timestamp: Date.now() + 24 h`, once, when the
// context setting was applied. Measured on the candidate: timestamp - Date.now()
// = +86,398,285 .. +86,399,734 ms, falling about 0.3-0.5 s per later call (study
// notes/geolocation-no-host-provider.md, M-c): a clock that is a day ahead and
// ticks per call is not a machine. The 24 h existed so that the cached position
// never expires for maximumAge (Geolocation.cpp reads the timestamp of the cached
// position in nsGeolocationRequest::Allow and SendLocation).
//
// The override object is wrapped by XPConnect as an nsIDOMGeoPosition and its
// attribute `timestamp` is read each time Firefox asks (the cache check, the
// maximumAge check, GeolocationPosition::Timestamp -> the page), so a getter
// answers Date.now() at that moment: fresh for every delivery, and never older
// than a maximumAge the page asks for. The acceptance contract for the
// end-to-end is at the top of geolocation_windows_provider_never_selected.test.mjs.
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

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci: {},
  ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);
const helper = new Helper();

// content/main.js with the platform stubbed (as in fragment_root_agents.test.mjs);
// returns the handlers the channel registered and the calls the top received.
function runInitialize(settings) {
  let handlers;
  class SimpleChannel {
    constructor() {}
    register(_ns, h) { handlers = h; }
    onReattached() {}
  }
  const cookies = new Map([
    [helper.contextCookieKey(0), { initScripts: [], bindings: [], settings }],
  ]);
  const scope = {
    Services: {
      scriptloader: { loadSubScript() {} },
      cpmm: { sharedData: { get: (key) => cookies.get(key) } },
      appinfo: { processID: 1 },
    },
    ChromeUtils: {
      importESModule: (url) => {
        if (url.endsWith('/Helper.js')) return { Helper };
        if (url.endsWith('/FrameTree.js'))
          return { FrameTree: class { applyCrossProcessState() {} setDocShellSettings() {} } };
        if (url.endsWith('/PageAgent.js')) return { PageAgent: class {} };
        throw new Error('unexpected import ' + url);
      },
    },
    SimpleChannel,
  };
  const { initialize } = load('content/main.js', scope, ['initialize']);
  const calls = [];
  const b = {
    browserId: 7,
    originAttributes: { userContextId: 0 },
    setGeolocationServiceOverride(...args) { calls.push(args); },
  };
  b.top = b;
  initialize(b, {});
  return { handlers, calls };
}

const GEO = { latitude: 45.0, longitude: 9.0, accuracy: 1500 };
const wait = ms => { const end = Date.now() + ms; while (Date.now() < end); };

test('the timestamp of the override is the time it is read, not the time it was set', () => {
  const { calls } = runInitialize({ geolocation: GEO });
  const position = calls[0][0];
  const before = Date.now();
  const first = position.timestamp;
  const after = Date.now();
  assert.ok(first >= before && first <= after, `timestamp ${first} not in [${before}, ${after}]`);
  wait(60);
  const second = position.timestamp;
  assert.ok(second - first >= 50, `the same position read 60 ms later: ${second - first} ms apart`);
  assert.ok(second <= Date.now());
});

test('a page that reads timestamp then Date.now() sees a difference between -20 and 0 ms', () => {
  const { handlers, calls } = runInitialize({});
  handlers.applyContextSetting({ name: 'geolocation', value: GEO });
  wait(30); // time passes between the set and the delivery, as in a real page
  const position = calls[0][0];
  for (let i = 0; i < 20; i++) {
    const diff = position.timestamp - Date.now();
    // The set happened 30 ms before the reads: a timestamp taken at the set would read -30 or less. 20 ms is room for the
    // scheduler of a loaded machine (2 ms was not: the export gate found -3 once).
    assert.ok(diff >= -20 && diff <= 0, `timestamp - Date.now() = ${diff}`);
    wait(5);
  }
});

test('never a day ahead: no 24 h arithmetic is left in the setter', () => {
  const source = fs.readFileSync(path.join(ROOT, 'content/main.js'), 'utf8');
  assert.doesNotMatch(source, /24 \* 60 \* 60 \* 1000/);
  assert.match(source, /get timestamp\(\) \{ return Date\.now\(\); \}/);
});

test('the coordinates are unchanged: the declaration, with NaN for the fields not declared', () => {
  const { calls } = runInitialize({ geolocation: GEO });
  const { coords } = calls[0][0];
  assert.equal(coords.latitude, 45.0);
  assert.equal(coords.longitude, 9.0);
  assert.equal(coords.accuracy, 1500);
  for (const k of ['altitude', 'altitudeAccuracy', 'heading', 'speed'])
    assert.ok(Number.isNaN(coords[k]), k);
});
