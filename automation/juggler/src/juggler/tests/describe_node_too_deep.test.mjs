// An <iframe> nested too deep names no frame (firefox-62, Fission, step 8; measured on the engine).
//
// A page that nests iframes past nsFrameLoader::DepthTooGreat (10) gets, for the last <iframe>, a browsing context
// that loads nothing: the client was told of a frame with an empty url, and `describeNode` named it as the content
// frame of the element, because it reads `element.browsingContext`, which does not apply the depth check. The old
// way (`contentWindow`) answered null there. `contentWindow` is null for such an <iframe> and is a window (a
// WindowProxy) for one in another process, so a null window is the sign that there is no frame to name.
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
const { PageAgent } = load('content/PageAgent.js', {
  Components: { interfaces: {}, results: {}, utils: {} },
  Cc: new Proxy({}, { get: () => ({ getService: () => ({}) }) }),
  ChromeUtils: { importESModule: () => ({ Helper, NetUtil: {}, setTimeout }) },
  dump: () => {},
}, ['PageAgent']);

const top = { id: 14, browserId: 9, parent: null, children: [] };
const child = { id: 20, browserId: 9, parent: top, children: [] };

function describe(node) {
  const doc = { name: 'document' };
  const mainFrame = { id: () => 'mainframe-9', domWindow: () => ({ document: doc }), docShell: () => ({}) };
  mainFrame.unsafeObject = () => ({ ownerDocument: doc, ...node });
  const agent = Object.create(PageAgent.prototype);
  agent._frameTree = { frame: () => mainFrame, frames: () => [mainFrame] };
  return JSON.parse(JSON.stringify(agent._describeNode({ objectId: 'x', frameId: 'mainframe-9' })));
}

test('an <iframe> with a window names its frame, in this process or in another', () => {
  assert.equal(describe({ localName: 'iframe', browsingContext: child, contentWindow: {} }).contentFrameId, 'subframe-20');
});

test('an <iframe> nested too deep has a browsing context and no window: no frame', () => {
  assert.equal(describe({ localName: 'iframe', browsingContext: child, contentWindow: null }).contentFrameId, undefined);
  assert.equal(describe({ localName: 'frame', browsingContext: child, contentWindow: null }).contentFrameId, undefined);
});

test('an <object> and an <embed> are named from their browsing context, whatever their window says', () => {
  // an <object> of another process answers contentWindow null, an <embed> has none at all
  assert.equal(describe({ localName: 'object', browsingContext: child, contentWindow: null }).contentFrameId, 'subframe-20');
  assert.equal(describe({ localName: 'embed', browsingContext: child }).contentFrameId, 'subframe-20');
});
