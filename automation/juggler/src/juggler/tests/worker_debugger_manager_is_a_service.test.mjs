// Every page's FrameTree gets the worker debugger manager, the second page too.
//
// From Firefox 157 the manager is a service (dom/workers/WorkerDebuggerManager.cpp)
// and a second createInstance fails with NS_ERROR_XPC_CI_RETURNED_FAILURE. The
// FrameTree constructor asked for an instance, so the first page of a content
// process came up and every later one did not: its frame tree threw, the page
// never answered Page.setInitScripts, and after a minute and a half the client
// saw Page.crashed. Measured on the first 157 candidates with raw Playwright and
// with invisible_playwright alike (newPage), and in CI as the drive gate
// running out its 25 minutes on every leg.
//
// The component here behaves as Firefox 157's does: one manager, getService
// always answers, createInstance answers once.
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

const Services = { obs: { addObserver() {}, removeObserver() {} } };

class EventEmitter {
  on() {}
  off() {}
  once() {}
  emit() {}
}

const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci: {},
  ChromeUtils: { importESModule: () => ({ EventEmitter }) },
  Services,
}, ['Helper']);

const manager = {
  listeners: [],
  addListener(listener) { this.listeners.push(listener); },
  removeListener() {},
  getWorkerDebuggerEnumerator() { return []; },
};
const workerDebuggerManager = {
  created: 0,
  createInstance() {
    if (++this.created > 1)
      throw new Error('NS_ERROR_XPC_CI_RETURNED_FAILURE');
    return manager;
  },
  getService() { return manager; },
};

const { FrameTree } = load('content/FrameTree.js', {
  Components: {
    interfaces: {
      nsIWebProgress: { NOTIFY_STATE_DOCUMENT: 1, NOTIFY_LOCATION: 2 },
    },
    results: {},
    utils: {},
  },
  Cc: {
    '@mozilla.org/websocketevent/service;1': { getService: () => ({}) },
    '@mozilla.org/dom/workers/workerdebuggermanager;1': workerDebuggerManager,
  },
  ChromeUtils: { importESModule: () => ({ Helper }), generateQI: () => () => {} },
  Services,
  Runtime: class {},
  dump: () => {},
}, ['FrameTree']);

// A frame tree as far as its constructor needs one: no frames of its own.
FrameTree.prototype._createFrame = function () {};

function rootBrowsingContext() {
  const webProgress = { addProgressListener() {}, removeProgressListener() {} };
  return {
    group: {},
    children: [],
    docShell: { QueryInterface: () => ({ getInterface: () => webProgress }) },
  };
}

test('the second page of a content process gets the worker debugger manager too', () => {
  const first = new FrameTree(rootBrowsingContext());
  const second = new FrameTree(rootBrowsingContext());
  assert.equal(first._wdm, manager);
  assert.equal(second._wdm, manager);
  assert.equal(manager.listeners.length, 2);
});
