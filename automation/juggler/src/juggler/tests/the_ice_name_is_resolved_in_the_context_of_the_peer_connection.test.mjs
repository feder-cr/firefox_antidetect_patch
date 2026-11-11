// The name of an ICE server (stun:stun-a.test, turn:...) is resolved in the browser
// context of the page that made the PeerConnection. Upstream resolves it with default
// origin attributes (NrIceResolver::resolve: `OriginAttributes attrs;`), so the
// DNS-over-HTTPS request of the lookup leaves in the default user context (userContextId
// 0), by the proxy of the browser, not by the proxy of the context. A retail Firefox
// resolves the name and loses the datagrams; ours got a negative answer and failed at
// once.
//
// The attributes travel: WebrtcProxyConfig.loadInfoArgs().originAttributes() (already
// sent to the process that gathers, before the gather) -> NrIceResolver::mAttrs (set on
// the STS thread by MediaTransportImplNicer::SetProxyConfig) -> AsyncResolveNative.
// Every name of iceServers (STUN and TURN) goes through the one NrIceResolver.
//
// Source guards: the C++ cannot run here. The behaviour is in
// dom/media/webrtc/transport/test/gtest/TestStealthIceResolverContext.cpp.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');
const T = 'dom/media/webrtc/transport/';

test('the resolver looks the name up with the attributes it was given, never with a bare default', () => {
  const text = read(T + 'nriceresolver.cpp');
  assert.doesNotMatch(text, /\n\s*OriginAttributes attrs;\n/);
  assert.match(text, /\n\s*OriginAttributes attrs = mAttrs;  \/\/ STEALTH-HOOK\(network\/doh\)\n/);
  assert.match(text, /sts_thread_, attrs, getter_AddRefs\(pr->request_\)/);
  assert.equal(text.match(/AsyncResolveNative/g).length, 1, 'one lookup path in the class');
});

test('the resolver holds the attributes and gives them back', () => {
  const text = read(T + 'nriceresolver.h');
  assert.match(text, /#include "StealthIceResolverContext\.h"  \/\/ STEALTH-HOOK\(network\/doh\)/);
  assert.match(text, /void SetOriginAttributes\(const OriginAttributes& aAttrs\) \{ mAttrs = aAttrs; \}  \/\/ STEALTH-HOOK\(network\/doh\)/);
  assert.match(text, /const OriginAttributes& GetOriginAttributes\(\) const \{ return mAttrs; \}  \/\/ STEALTH-HOOK\(network\/doh\)/);
  assert.match(text, /OriginAttributes mAttrs;  \/\/ STEALTH-HOOK\(network\/doh\)/);
});

test('SetProxyConfig gives the resolver the attributes of the PeerConnection before the config is moved away', () => {
  const text = read('dom/media/webrtc/jsapi/MediaTransportHandler.cpp');
  assert.match(text, /#include "transport\/StealthIceResolverContext\.h"  \/\/ STEALTH-HOOK\(network\/doh\)/);
  const body = text.match(/void MediaTransportImplNicer::SetProxyConfig\([\s\S]*?\n}\n/)[0];
  const set = body.indexOf('StealthIceResolverAttributes(aProxyConfig)');
  assert.ok(set > 0, 'the resolver is not given the attributes');
  assert.match(body, /mDNSResolver->SetOriginAttributes\(StealthIceResolverAttributes\(aProxyConfig\)\);  \/\/ STEALTH-HOOK\(network\/doh\)/);
  assert.ok(set < body.indexOf('std::move(aProxyConfig)'), 'the config is read after it was moved');
});

test('our function reads the attributes of the load info of the config and keeps the user context alone', () => {
  const header = read(T + 'StealthIceResolverContext.h');
  const code = read(T + 'StealthIceResolverContext.cpp');
  assert.match(header, /OriginAttributes StealthIceResolverAttributes\(\s*const OriginAttributes& aPeerConnection\)/);
  assert.match(header, /OriginAttributes StealthIceResolverAttributes\(\s*const NrSocketProxyConfig& aConfig\)/);
  assert.match(code, /aConfig\.GetConfig\(\)\.loadInfoArgs\(\)\.originAttributes\(\)/);
  assert.match(code, /attrs\.mUserContextId = aPeerConnection\.mUserContextId;/);
});

test('the files are built', () => {
  assert.match(read(T + 'common.build'), /\ntransport_lcppsrcs \+= \["StealthIceResolverContext\.cpp"\]  # STEALTH-HOOK\(network\/doh\)\n/);
  assert.match(read(T + 'test/gtest/moz.build'), /SOURCES \+= \["TestStealthIceResolverContext\.cpp"\]  # STEALTH-HOOK\(network\/doh\)/);
});

test('the gtest covers the default, the context and the function', () => {
  const text = read(T + 'test/gtest/TestStealthIceResolverContext.cpp');
  assert.match(text, /TEST\(StealthIceResolverContext, /);
  assert.match(text, /ContextWithUser\(6\)/);
  assert.match(text, /GetOriginAttributes\(\)\.mUserContextId/);
  assert.match(text, /NrIceResolver/);
});
