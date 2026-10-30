// A frame's WebSocket listener reads its arguments in the order the IDL gives.
//
// From Firefox 159 nsIWebSocketEventListener.webSocketClosed, frameReceived and
// frameSent take the HTTP channel id right after the serial
// (netwerk/protocol/websocket/nsIWebSocketEventService.idl). Listeners still
// written for 157 read that id as the frame, as wasClean, as the close code:
// every frame came out with no opcode and no data, and every close was
// reported as an error or not at all.
//
// The listener here is called the way 159's service calls it.
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

const { Frame, FrameTree } = load('content/FrameTree.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  Cc: {},
  Ci: { nsIWebSocketChannel: { CLOSE_ABNORMAL: 1006 } },
  ChromeUtils: { importESModule: () => ({ Helper }), generateQI: () => () => {} },
  Services,
  Runtime: class {},
  dump: () => {},
  btoa,
}, ['Frame', 'FrameTree']);

const HTTP_CHANNEL_ID = 987654321;
const SERIAL = 7;

function listener() {
  const events = [];
  const frameTree = { emit: (name, payload) => events.push({ name, payload }) };
  const browsingContext = { browserId: 1, parent: null };
  const frame = new Frame(frameTree, {}, browsingContext, null);
  return { events, ws: frame._webSocketListener };
}

function open(ws) {
  ws.webSocketCreated(SERIAL, 'ws://example.test/', '');
  ws.webSocketOpened(SERIAL, 'ws://example.test/', '', '', HTTP_CHANNEL_ID);
}

test('a received text frame is read from the third argument', () => {
  const { events, ws } = listener();
  open(ws);
  ws.frameReceived(SERIAL, HTTP_CHANNEL_ID, { opCode: 1, payload: 'hello' });
  const received = events.find(e => e.name === FrameTree.Events.WebSocketFrameReceived);
  assert.ok(received, 'the frame was reported');
  assert.equal(received.payload.opcode, 1);
  assert.equal(received.payload.data, 'hello');
  assert.equal(received.payload.wsid, SERIAL + '');
});

test('a sent binary frame is read from the third argument', () => {
  const { events, ws } = listener();
  open(ws);
  ws.frameSent(SERIAL, HTTP_CHANNEL_ID, { opCode: 2, payload: 'ab' });
  const sent = events.find(e => e.name === FrameTree.Events.WebSocketFrameSent);
  assert.ok(sent, 'the frame was reported');
  assert.equal(sent.payload.opcode, 2);
  assert.equal(sent.payload.data, btoa('ab'));
});

test('a frame that arrives before the socket is open waits, and keeps its content', () => {
  const { events, ws } = listener();
  ws.webSocketCreated(SERIAL, 'ws://example.test/', '');
  ws.frameReceived(SERIAL, HTTP_CHANNEL_ID, { opCode: 1, payload: 'early' });
  ws.webSocketOpened(SERIAL, 'ws://example.test/', '', '', HTTP_CHANNEL_ID);
  const received = events.filter(e => e.name === FrameTree.Events.WebSocketFrameReceived);
  assert.equal(received.length, 1);
  assert.equal(received[0].payload.data, 'early');
});

test('a clean close reports no error', () => {
  const { events, ws } = listener();
  open(ws);
  ws.webSocketClosed(SERIAL, HTTP_CHANNEL_ID, true, 1000, 'bye');
  const closed = events.find(e => e.name === FrameTree.Events.WebSocketClosed);
  assert.equal(closed.payload.error, '');
});

test('an unclean close names its code', () => {
  const { events, ws } = listener();
  open(ws);
  ws.webSocketClosed(SERIAL, HTTP_CHANNEL_ID, false, 1006, '');
  const closed = events.find(e => e.name === FrameTree.Events.WebSocketClosed);
  assert.equal(closed.payload.error, 'CLOSE_ABNORMAL');
});
