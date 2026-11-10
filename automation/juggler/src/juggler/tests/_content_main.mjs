// content/main.js, loaded the way the content process loads it, with a fake tree, channel and shared data: the
// tests of what `initialize` does for a fragment root. Not a test itself (no `.test.` in the name).

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

export const { Helper } = load('Helper.js', {
  Cc: { '@mozilla.org/uuid-generator;1': { getService: () => ({}) } },
  Ci: {},
  ChromeUtils: { importESModule: () => ({}) },
}, ['Helper']);

const helper = new Helper();

// `settings`: what the context's cookie holds; `page`: the page's. Returns what `initialize` returned, the tree it
// made, the channel it made and the handlers it registered on it, as the fakes recorded them.
export function initializeRoot({ browsingContext, docShell = {}, settings = {}, page = {} }) {
  const shared = new Map([
    [helper.contextCookieKey(browsingContext.originAttributes.userContextId),
      { initScripts: [], bindings: [], settings }],
    [helper.pageCookieKey(browsingContext.browserId),
      { initScripts: [], bindings: [], interceptFileChooserDialog: false, ...page }],
  ]);
  const made = { trees: [], channels: [] };

  class FakeFrameTree {
    constructor(root) {
      this.root = root;
      this.docShellSettings = [];
      made.trees.push(this);
    }
    // copied out of the vm realm, so deepEqual compares plain objects
    setDocShellSettings(s) { this.docShellSettings.push(JSON.parse(JSON.stringify(s))); }
    applyCrossProcessState() {}
    announceRestoredDocument() {}
  }
  class FakeChannel {
    constructor() {
      this.handlers = {};
      made.channels.push(this);
    }
    register(namespace, handlers) { this.handlers[namespace] = handlers; }
    onReattached() {}
  }
  const modules = {
    'resource://gre/modules/juggler/Helper.js': { Helper },
    'resource://gre/modules/juggler/content/FrameTree.js': { FrameTree: FakeFrameTree },
    'resource://gre/modules/juggler/content/PageAgent.js': { PageAgent: class {} },
  };
  const scope = {
    Services: {
      scriptloader: { loadSubScript: () => {} },
      cpmm: { sharedData: { get: key => shared.get(key) } },
      appinfo: { processID: 1 },
    },
    ChromeUtils: { importESModule: url => modules[url] },
    SimpleChannel: FakeChannel,
    Date,
  };
  const { initialize } = load('content/main.js', scope, ['initialize']);
  const data = initialize(browsingContext, docShell);
  return { data, tree: made.trees[0], channel: made.channels[0], handlers: made.channels[0].handlers[''] };
}
