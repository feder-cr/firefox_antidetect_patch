// The DNS-over-HTTPS hooks in Mozilla's files are small and leave Mozilla's lines
// as Mozilla wrote them: the logic is in our own files (netwerk/ipc/
// StealthTRRContext.{h,cpp}, dom/media/webrtc/transport/
// StealthIceResolverContext.{h,cpp}, toolkit/components/parentalcontrols/
// StealthParentalControls.h), the Mozilla file carries calls of one or two lines
// each, tagged STEALTH-HOOK(network/doh).
//
// Per file: how many lines carry the tag (at most), the longest run of
// consecutive tagged lines (at most 5), and how many lines of the release
// FIREFOX_159_0_RELEASE the file has lost (at most: the lines older features
// already rewrote, which stay as they are). Where the tag of the release does
// not exist in the checkout the last measure is skipped, as in the other tests
// that compare with it.
//
// Behaviour of what the hooks call: netwerk/test/gtest/TestStealthTRRContext.cpp,
// dom/media/webrtc/transport/test/gtest/TestStealthIceResolverContext.cpp.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');
const TAG = /STEALTH-HOOK\(network\/doh\)/;

function taggedLines(text) {
  return text.split('\n').filter(l => TAG.test(l));
}

function longestRun(text) {
  let best = 0;
  let run = 0;
  for (const line of text.split('\n')) {
    run = TAG.test(line) ? run + 1 : 0;
    best = Math.max(best, run);
  }
  return best;
}

// Lines of the release that the file no longer has (null: no tag in this checkout).
function removedFromRelease(file) {
  try {
    const diff = execFileSync('git', ['diff', '-U0', 'FIREFOX_159_0_RELEASE', '--', file],
      { cwd: TREE, encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] });
    return diff.split('\n').filter(l => l.startsWith('-') && !l.startsWith('---')).map(l => l.slice(1));
  } catch (e) {
    return null;
  }
}

// [file, tagged lines at most, Mozilla lines removed at most]
const FILES = [
  // The files that only pass the context on: one tagged line per function (a
  // parameter, an argument or a member). The Mozilla lines they have lost against
  // the release are the signature lines the older UDP lookup already rewrote;
  // this feature adds no removal to any of them.
  ['netwerk/ipc/ProxyConfigLookup.h', 4, 2],
  ['netwerk/ipc/ProxyConfigLookupChild.cpp', 2, 2],
  ['netwerk/ipc/ProxyConfigLookupChild.h', 2, 1],
  ['netwerk/ipc/ProxyConfigLookupParent.cpp', 3, 5],
  ['netwerk/ipc/ProxyConfigLookupParent.h', 3, 1],
  ['netwerk/ipc/SocketProcessParent.cpp', 3, 3],
  ['netwerk/ipc/SocketProcessParent.h', 2, 3],
  ['netwerk/ipc/PSocketProcess.ipdl', 1, 1],
  // The resolver hook: the include and the two calls.
  ['netwerk/dns/nsHostResolver.cpp', 3, 0],
  // The threading of the context through the lookup is one tagged line per
  // function it passes through (a parameter, an argument, a member): the
  // include, Create's parameter, the constructor's call, parameter and member,
  // DoProxyResolve's argument to the socket process, and the context applied to
  // the stand-in channel (two lines: the call and the check of its status, after
  // Mozilla's own check; a hook between the two would need a call that takes the
  // previous status, which is worse code than one more line). The other lines of
  // the file belong to the UDP lookup.
  ['netwerk/ipc/ProxyConfigLookup.cpp', 8, 6],
  // TRRServiceChannel.cpp: nine tagged lines and four Mozilla lines rewritten,
  // registered:
  // - `NS_SUCCEEDED(ResolveProxy())) {` reads the status: ResolveProxy has two
  //   failing exits (the dispatch to the main thread, which fails at shutdown, and
  //   the lookup) and a DoH request must end on both, not go DIRECT; the status is
  //   only visible in the condition that calls it;
  // - `getter_AddRefs(proxyRequest));` takes the context of the lookup as its
  //   last arguments;
  // - the connection info and the alternative service mapping take the attributes
  //   of the lookup's context instead of OriginAttributes() (two lines). The
  //   attributes are computed once, in one line. Making the objects again after
  //   Mozilla's calls would duplicate their argument lists and build each twice.
  // The other lines: the include, the declaration of the status, the fail-closed
  // line at the start of the request and the one in OnProxyAvailable.
  ['netwerk/protocol/http/TRRServiceChannel.cpp', 9, 4],
  // nsHttpChannel.cpp: five tagged lines and one Mozilla line rewritten,
  // registered: `} else if (NS_SUCCEEDED(ResolveProxy())) {` reads the status of
  // ResolveProxy, which is only visible in the condition that calls it; a DoH
  // request whose lookup failed must end, not go DIRECT. The other lines: the
  // include, the declaration of the status, and one line in each of the two
  // functions that fail over to DIRECT.
  ['netwerk/protocol/http/nsHttpChannel.cpp', 5, 1],
  // The include, the context applied after upstream's check of the channel (two
  // lines), and the condition of the cached connection info. That line is the one
  // Mozilla line rewritten, registered: `StaticPrefs::network_trr_async_connInfo()) {`
  // gets `&& StealthTRRContextOfSuffix(mOriginSuffix).isNothing()` before its
  // bracket. A plain operand is the readable form; there is no guard to return
  // early, the code goes on under the block.
  ['netwerk/dns/TRR.cpp', 4, 1],
  // The copy in Clone() and the two lines of SetOriginAttributes(). One Mozilla
  // line is rewritten, registered: `return NS_ERROR_NOT_IMPLEMENTED;` becomes the
  // store and `return NS_OK;`. Leaving it under an early return would be dead code
  // that the build warns about (-Wunreachable-code-return), and a helper that
  // returns true only to avoid that is worse code than one rewritten line.
  ['netwerk/base/TRRLoadInfo.cpp', 3, 1],
  // The name of an ICE server is looked up in the context of its PeerConnection
  // (the_ice_name_is_resolved_in_the_context_of_the_peer_connection.test.mjs).
  // nriceresolver.cpp: one Mozilla line rewritten, registered: `OriginAttributes
  // attrs;` becomes `OriginAttributes attrs = mAttrs;` (the one declaration the
  // lookup passes to the DNS service; a second variable would leave the default
  // one unused). nriceresolver.h: the include, the setter and getter, the member
  // (four lines, none of them in a Mozilla list). MediaTransportHandler.cpp: the
  // include and the one call that gives the resolver the attributes before the
  // config is moved into the ICE context. No Mozilla line is rewritten there.
  ['dom/media/webrtc/transport/nriceresolver.cpp', 1, 1],
  ['dom/media/webrtc/transport/nriceresolver.h', 4, 0],
  ['dom/media/webrtc/jsapi/MediaTransportHandler.cpp', 2, 0],
  ['toolkit/components/parentalcontrols/nsParentalControlsServiceWin.cpp', 2, 0],
  ['toolkit/components/parentalcontrols/nsParentalControlsServiceDefault.cpp', 2, 0],
];

// DNSServiceBase.cpp is Mozilla's: the gate of the declared proxy that used to be
// in it is not, and no line of the release is missing.
test('netwerk/dns/DNSServiceBase.cpp is as Mozilla wrote it', () => {
  const text = read('netwerk/dns/DNSServiceBase.cpp');
  assert.doesNotMatch(text, /STEALTH-HOOK|Stealth/);
  assert.match(text, /#include "DNS\.h"\n#include "mozilla\/ClearOnShutdown\.h"/);
  assert.match(text, /\n  \/\/ TODO\(Bug 1890542\): use nsIProxyInfo object whether sending DNS request to\n/);
  const removed = removedFromRelease('netwerk/dns/DNSServiceBase.cpp');
  if (removed !== null) assert.deepEqual(removed, []);
});

for (const [file, maxTagged, maxRemoved] of FILES) {
  test(`${file}: at most ${maxTagged} tagged lines, none longer than 5, ${maxRemoved} Mozilla lines removed`, () => {
    const text = read(file);
    assert.ok(taggedLines(text).length >= 1, 'no hook');
    assert.ok(taggedLines(text).length <= maxTagged, `${taggedLines(text).length} tagged lines`);
    assert.ok(longestRun(text) <= 5, `a hook of ${longestRun(text)} lines`);
    const removed = removedFromRelease(file);
    if (removed !== null)
      assert.ok(removed.length <= maxRemoved, `Mozilla lines removed: ${JSON.stringify(removed)}`);
  });
}

test('parental controls: the hook is one line that returns before the first OS call', () => {
  for (const file of ['nsParentalControlsServiceWin.cpp', 'nsParentalControlsServiceDefault.cpp']) {
    const text = read(`toolkit/components/parentalcontrols/${file}`);
    assert.match(text,
      /\n  if \(StealthParentalControlsDeclared\(&mEnabled\)\) \{ return; \}  \/\/ STEALTH-HOOK\(network\/doh\)\n/, file);
  }
});
