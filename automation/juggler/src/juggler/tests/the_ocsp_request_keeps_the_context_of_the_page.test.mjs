// The OCSP request of a certificate is made for the TLS of a page, so it must leave by the
// proxy, and have its name resolved by the resolver, of the browser context of that page.
// Upstream creates it with the system principal and copies into its origin attributes only the
// first party domain and the private browsing id (nsNSSCallbacks.cpp, OCSPRequest::Run), so it
// leaves in the default user context (userContextId 0), outside the proxy of the page's context.
// The attributes travel unchanged from the socket to that line (the whole struct goes over IPC
// from the socket process), and the hook adds the userContextId there.
//
// Second place where the context is born wrong: the TLS layered over a tunnel (HTTPS proxy,
// HTTP/2 tunnel) is created by TLSTransportLayer::Init with `OriginAttributes()`, so its socket
// control, and with it the verification and the OCSP request, has no context at all. It takes the
// context of the socket transport it runs over.
//
// Source guards: the C++ cannot run here. The behaviour of the two functions is in
// netwerk/test/gtest/TestStealthTRRContext.cpp.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');
const CALLBACKS = 'security/manager/ssl/nsNSSCallbacks.cpp';
const TUNNEL = 'netwerk/protocol/http/TLSTransportLayer.cpp';
const TAG = '// STEALTH-HOOK(network/doh)';

test('the OCSP request is given the context of the page, after what Mozilla copies and before the attributes are set', () => {
  const text = read(CALLBACKS);
  assert.ok(text.includes('#include "mozilla/net/StealthTRRContext.h"  ' + TAG + '\n'), 'the include is missing');
  const hook = '\n    mozilla::net::StealthKeepRequestContext(mOriginAttributes, attrs);  ' + TAG + '\n';
  assert.ok(text.includes(hook), 'the OCSP request does not keep the context');
  assert.equal(text.match(/StealthKeepRequestContext\(/g).length, 1);
  const body = text.match(/OCSPRequest::Run\(\) \{[\s\S]*?\n}\n/)[0];
  const at = body.indexOf(hook);
  assert.ok(at > 0, 'the hook is not in OCSPRequest::Run');
  const decl = body.indexOf('\n    OriginAttributes attrs;\n');
  assert.ok(decl > 0 && decl < at, 'the attributes are given the context before they exist');
  assert.ok(body.indexOf('attrs.mFirstPartyDomain = mOriginAttributes.mFirstPartyDomain;') < at, 'Mozilla copies the first party domain after the hook');
  assert.ok(body.indexOf('attrs.mPrivateBrowsingId = mOriginAttributes.mPrivateBrowsingId;') < at, 'Mozilla copies the private browsing id after the hook');
  assert.ok(at < body.indexOf('loadInfo->SetOriginAttributes(attrs)'), 'the attributes are set on the load info before they have the context');
  assert.doesNotMatch(body, /attrs\.mUserContextId/, 'the logic is in our file, not in Mozilla\'s');
});

test('the TLS over a tunnel takes the context of the transport it runs over, never a bare default', () => {
  const text = read(TUNNEL);
  assert.ok(text.includes('#include "mozilla/net/StealthTRRContext.h"  ' + TAG + '\n'), 'the include is missing');
  const body = text.match(/bool TLSTransportLayer::Init\([\s\S]*?\n}\n/)[0];
  assert.doesNotMatch(body, /OriginAttributes\(\)/, 'a socket control with default attributes is made');
  const hook = 'nullptr, StealthTransportContext(mSocketTransport), 0, 0, mFD,  ' + TAG + '\n';
  assert.ok(body.includes(hook), 'the socket control is not given the context of the transport');
  assert.equal(text.match(/StealthTransportContext\(/g).length, 1);
  assert.ok(body.indexOf('provider->AddToSocket(') < body.indexOf(hook), 'the context is not an argument of AddToSocket');
});

test('our functions keep the user context alone and read the transport, in the file of the context', () => {
  const header = read('netwerk/ipc/StealthTRRContext.h');
  const code = read('netwerk/ipc/StealthTRRContext.cpp');
  assert.match(header, /void StealthKeepRequestContext\(const OriginAttributes& aPage,\s*OriginAttributes& aRequest\);/);
  assert.match(header, /OriginAttributes StealthTransportContext\(nsISocketTransport\* aTransport\);/);
  assert.match(header, /\nclass nsISocketTransport;\n/);
  const keep = code.match(/void StealthKeepRequestContext\([\s\S]*?\n}\n/)[0];
  assert.match(keep, /aRequest\.mUserContextId = aPage\.mUserContextId;/);
  assert.doesNotMatch(keep, /mFirstPartyDomain|mPrivateBrowsingId/, 'first party domain and private browsing id stay Mozilla\'s');
  const transport = code.match(/OriginAttributes StealthTransportContext\([\s\S]*?\n}\n/)[0];
  assert.match(transport, /if \(aTransport\) \{/);
  assert.match(transport, /aTransport->GetOriginAttributes\(&attrs\)/);
  assert.match(code, /#include "nsISocketTransport\.h"/, 'the implementation uses nsISocketTransport and includes it');
});

test('the gtest covers the context of a page, the default context and the missing transport', () => {
  const text = read('netwerk/test/gtest/TestStealthTRRContext.cpp');
  assert.match(text, /TEST\(TestStealthTRRContext, TheOcspRequestKeepsTheContextOfThePage\)/);
  assert.match(text, /page\.mUserContextId = 7;/);
  assert.match(text, /EXPECT_EQ\(request\.mUserContextId, 7u\);/);
  assert.match(text, /TEST\(TestStealthTRRContext, TheOcspRequestOfTheDefaultContextIsUnchanged\)/);
  assert.match(text, /StealthTransportContext\(nullptr\)/);
});
