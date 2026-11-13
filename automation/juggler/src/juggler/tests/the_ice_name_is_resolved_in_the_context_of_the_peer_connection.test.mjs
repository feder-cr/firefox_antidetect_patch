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
// The names of the UDP candidates (STUN, TURN over UDP) go through the one NrIceResolver. A TURN
// server given by name over TCP (turn:name:port?transport=tcp) does not: in the process that
// gathers, NR_ICE_REG_USE_NR_RESOLVER_FOR_TCP is set only in the parent process, so the name
// reaches NrTcpSocket::connect -> WebrtcTCPSocket::OpenWithoutHttpProxy, which creates an
// nsSocketTransport whose origin attributes are the default ones. nsSocketTransport::ResolveHost
// then asks the DNS service for userContextId 0, which is in no context's DoH list
// (network.trr.stealth.contexts): the name is resolved by the machine's resolver. The same
// attributes are therefore given to that transport, before it is bound (it refuses them once its
// socket exists), whenever the socket has a proxy config (the config carries the load info of the
// PeerConnection).
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
const TCP = T + 'ipc/WebrtcTCPSocket.cpp';

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

test('a TURN server given by name over TCP is resolved in the context of the PeerConnection: no query reaches the system resolver', () => {
  const text = read(TCP);
  const body = text.match(/void WebrtcTCPSocket::OpenWithoutHttpProxy\([\s\S]*?\n}\n/)[0];
  // One call gives the transport its context and the family of its bind address (StealthTcpSocketBorn): the hook of the
  // context and the hook of the bind address of this function are one line, so the file keeps its number of lines.
  const hook = '\n  if (NS_WARN_IF(!mozilla::StealthTcpSocketBorn(mTransport, mProxyConfig.ptrOr(nullptr), mLocalAddress.get(), mLocalPort, &prAddr))) {  // STEALTH-HOOK(fingerprint/webrtc/declared-network)\n';
  assert.ok(body.includes(hook), 'the transport of the TCP socket is not given the attributes of the PeerConnection');
  assert.equal(text.match(/StealthTcpSocketBorn\(/g).length, 1);
  assert.doesNotMatch(text, /StealthTcpNameContext\(/, 'the context has its own line again: the merge is undone');
  const at = body.indexOf(hook);
  const created = body.indexOf('sts->CreateTransport(');
  assert.ok(created > 0 && created < at, 'the attributes are set on a transport that does not exist yet');
  assert.ok(at < body.indexOf('mTransport->Bind('), 'the transport refuses its attributes once it is bound');
  assert.ok(at < body.indexOf('OpenInputStream('), 'the lookup starts when the streams open');
  assert.ok(body.indexOf('CloseWithReason(rv);', created) < at, 'a transport that was not created is not used');
  // The function itself: the attributes the resolver of the UDP names uses, taken from the load info of the config.
  const own = read(T + 'StealthIceResolverContext.cpp');
  const fn = own.match(/void StealthTcpNameContext\([\s\S]*?\n}\n/)[0];
  assert.match(fn, /if \(aTransport && aConfig\) \{/);
  assert.match(fn, /aTransport->SetOriginAttributes\(StealthIceResolverAttributes\(\s*aConfig->loadInfoArgs\(\)\.originAttributes\(\)\)\);/);
  // The merged call: the context first, then the bind address, both in the file that holds them.
  const born = own.match(/bool StealthTcpSocketBorn\([\s\S]*?\n}\n/)[0];
  assert.ok(born.includes('StealthTcpNameContext(aTransport, aConfig);'));
  assert.ok(born.indexOf('StealthTcpNameContext(') < born.indexOf('return StealthTcpBindAddress('), 'the bind address is decided before the context is given');
});

test('a TURN name over TCP does not go through NrIceResolver in the process that gathers, so the transport is the only place to carry the context', () => {
  const nricectx = read(T + 'nricectx.cpp');
  assert.match(nricectx, /NR_ICE_REG_USE_NR_RESOLVER_FOR_TCP/);
  assert.match(nricectx, /XRE_IsParentProcess\(\)/);
  assert.equal(read(TCP).match(/CreateTransport\(/g).length, 1, 'a second transport of the TCP socket would need the same attributes');
});

test('the function is declared in a header the TCP socket already includes, so its file gains one line and no include', () => {
  assert.match(read(T + 'nr_stealth_network.h'), /\nvoid StealthTcpNameContext\(nsISocketTransport\* aTransport, const net::WebrtcProxyConfig\* aConfig\);\n/);
  const text = read(TCP);
  assert.match(text, /\n#include "transport\/nr_stealth_network\.h"/);
  assert.doesNotMatch(text, /StealthIceResolverContext\.h/);
  assert.doesNotMatch(read(T + 'build/moz.build'), /StealthIceResolverContext/);
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
