// With IPv6 declared off, a literal IPv6 destination fails the way connect()
// fails on a Windows machine that has the IPv6 stack and no IPv6 route.
//
// network.dns.disableIPv6 (the declared network) only keeps hostnames from
// resolving to AAAA; a literal such as http://[2001:db8::1]/ never goes through
// that: nsHostResolver::FromIPLiteral hands the address straight to the socket,
// so the page could load over a host IPv6 route the declaration says is not
// there. The one point every direct TCP connection passes is
// nsSocketTransport::InitiateSocket (ordinary connections, Happy Eyeballs,
// WebSocket, fetch, XHR, DoH, a connection to an HTTP proxy), and the one point
// a SOCKS connection reaches the network is nsSOCKSSocketInfo::ConnectToProxy.
// Both call the same wrapper in place of PR_Connect.
//
// The error is the real one: connect() gives WSAENETUNREACH, NSPR maps it to
// PR_NETWORK_UNREACHABLE_ERROR (win32_errors.c), ErrorAccordingToNSPR to
// NS_ERROR_CONNECTION_REFUSED (NS_ERROR_PROXY_CONNECTION_REFUSED when the
// socket goes to a proxy): about:neterror "connectionFailure", a TypeError in
// fetch, an error event in XHR and WebSocket. No delay and no new error.
//
// Source guards (the predicate and its table run in the candidate's gtest
// filter, suite StealthDeclaredNetwork).
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const TREE = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');
const HOOK = 'STEALTH-HOOK(network/ipv6-literal)';

// The text of a function from its signature to the closing brace at column 0.
function body(text, signature) {
  const from = text.indexOf(signature);
  assert.ok(from >= 0, signature);
  return text.slice(from, text.indexOf('\n}\n', from) + 3);
}

test('the errors are the Windows ones: WSAENETUNREACH and WSAEADDRNOTAVAIL, both NS_ERROR_CONNECTION_REFUSED', () => {
  const win = read('nsprpub/pr/src/md/windows/win32_errors.c');
  assert.match(win, /case WSAENETUNREACH:\s+case ERROR_NETWORK_UNREACHABLE:\s+prError = PR_NETWORK_UNREACHABLE_ERROR;/);
  assert.match(win, /case WSAEADDRNOTAVAIL:\s+case ERROR_INVALID_NETNAME:\s+prError = PR_ADDRESS_NOT_AVAILABLE_ERROR;/);
  const sock = read('netwerk/base/nsSocketTransport2.cpp');
  const map = body(sock, 'nsresult ErrorAccordingToNSPR(PRErrorCode errorCode) {');
  assert.match(map, /case PR_NETWORK_UNREACHABLE_ERROR:[\s\S]*case PR_ADDRESS_NOT_AVAILABLE_ERROR:[\s\S]*rv = NS_ERROR_CONNECTION_REFUSED;/);
  const impl = read('netwerk/base/StealthIPv6Literal.cpp');
  assert.match(impl, /PR_NETWORK_UNREACHABLE_ERROR/);
  assert.match(impl, /PR_ADDRESS_NOT_AVAILABLE_ERROR/);
});

test('the predicate is pure, gated on the declaration, and exempts what retail connects', () => {
  const header = read('netwerk/base/StealthIPv6Literal.h');
  assert.match(header, /PRErrorCode StealthIPv6ConnectError\(const NetAddr& aAddr, bool aIPv6Disabled\);/);
  const impl = read('netwerk/base/StealthIPv6Literal.cpp');
  const fn = body(impl, 'PRErrorCode StealthIPv6ConnectError(const NetAddr& aAddr, bool aIPv6Disabled) {');
  // Nothing without the declaration, nothing for IPv4 or a local socket.
  assert.match(fn, /if \(!aIPv6Disabled \|\| aAddr\.raw\.family != AF_INET6\) \{\s+return 0;/);
  // ::1 and link-local stay as they are.
  assert.match(fn, /IPv6ADDR_IS_LOOPBACK\(&ip\)/);
  assert.match(fn, /0xfe/);
  assert.match(fn, /0x80/);
  // The classes the host answers the same way on every route table.
  assert.match(fn, /if \(IPv6ADDR_IS_V4MAPPED\(&ip\)\) \{\s+return 0;/);
  assert.match(fn, /IPv6ADDR_IS_UNSPECIFIED\(&ip\)/);
  assert.match(fn, /ip\.u8\[0\] == 0xff/);
  // The predicate does not read prefs or the host.
  assert.doesNotMatch(fn, /StaticPrefs|Preferences::|getenv|PR_Connect|PR_SetError|PR_GetError/);
});

test('the connect wrapper reads the declaration, fails without touching the network, and otherwise is PR_Connect', () => {
  const impl = read('netwerk/base/StealthIPv6Literal.cpp');
  const fn = body(impl, 'PRStatus StealthIPv6Connect(PRFileDesc* aFd, const NetAddr& aAddr,');
  // A destination behind a proxy is the proxy's to reach: judged as if nothing were declared.
  assert.match(fn, /StealthIPv6ConnectError\(\s*aAddr, !aAddrIsBehindProxy && StaticPrefs::network_dns_disableIPv6\(\)\)/);
  assert.match(fn, /PR_SetError\(error, 0\);\s+return PR_FAILURE;/);
  assert.match(fn, /return PR_Connect\(aFd, aPrAddr, aTimeout\);/);
  assert.match(impl, /#include "mozilla\/StaticPrefs_network\.h"/);
});

test('nsSocketTransport::InitiateSocket connects through the wrapper; a SOCKS destination is exempt', () => {
  const sock = read('netwerk/base/nsSocketTransport2.cpp');
  assert.match(sock, /#include "StealthIPv6Literal\.h"\s+\/\/ STEALTH-HOOK\(network\/ipv6-literal\)/);
  const init = body(sock, 'nsresult nsSocketTransport::InitiateSocket() {');
  // The only connect of the function is the wrapper, on the line the hook marks.
  assert.equal(init.match(/= PR_Connect\(/g), null);
  const lines = init.split('\n').filter(l => l.includes('StealthIPv6Connect('));
  assert.equal(lines.length, 1);
  assert.ok(lines[0].includes(HOOK), 'the hook is on the line');
  // Behind a SOCKS proxy mNetAddr is the destination, which the proxy reaches, not this host.
  const at = init.indexOf('StealthIPv6Connect(');
  assert.match(init.slice(at, at + 300), /mProxyTransparent && !mProxyHost\.IsEmpty\(\)/);
  // The failure is read like a connect() failure, on the very next line.
  assert.match(init.slice(at, at + 400), /PRErrorCode code = PR_GetError\(\);/);
  assert.equal(sock.match(/StealthIPv6Connect\(/g).length, 1);
});

test('the SOCKS layer reaches its proxy through the wrapper, with no exemption', () => {
  const socks = read('netwerk/socket/nsSOCKSIOLayer.cpp');
  assert.match(socks, /#include "StealthIPv6Literal\.h"\s+\/\/ STEALTH-HOOK\(network\/ipv6-literal\)/);
  const fn = body(socks, 'PRStatus nsSOCKSSocketInfo::ConnectToProxy(PRFileDesc* fd) {');
  const lines = fn.split('\n').filter(l => l.includes('StealthIPv6Connect('));
  assert.equal(lines.length, 1);
  assert.ok(lines[0].includes(HOOK), 'the hook is on the line');
  assert.match(lines[0], /StealthIPv6Connect\(fd->lower, proxy, false, &prProxy, mTimeout\)/);
  assert.equal(fn.match(/methods->connect\(/g), null);
});

test('no other code decides: neither the HTTP channel, nor the DNS service, nor the resolver is hooked', () => {
  for (const rel of ['netwerk/protocol/http/nsHttpChannel.cpp', 'netwerk/dns/nsHostResolver.cpp',
    'netwerk/dns/nsDNSService2.cpp', 'netwerk/protocol/websocket/WebSocketChannel.cpp'])
    assert.doesNotMatch(read(rel), /StealthIPv6/, rel);
});

test('the build lists the source, and the gtest runs in a suite the candidate filter selects', () => {
  assert.match(read('netwerk/base/moz.build'), /UNIFIED_SOURCES \+= \["StealthIPv6Literal\.cpp"\]\s+# STEALTH-HOOK\(network\/ipv6-literal\)/);
  assert.match(read('netwerk/test/gtest/moz.build'), /SOURCES \+= \["TestStealthIPv6Literal\.cpp"\]\s+# STEALTH-HOOK\(network\/ipv6-literal\)/);
  const test = read('netwerk/test/gtest/TestStealthIPv6Literal.cpp');
  assert.match(test, /TEST\(StealthDeclaredNetwork, AnIPv6LiteralFailsLikeAHostWithNoIPv6Route\)/);
  assert.match(test, /TEST\(StealthDeclaredNetwork, NothingIsBlockedWhileIPv6IsDeclared\)/);
  assert.match(test, /TEST\(StealthDeclaredNetwork, TheConnectFailsWithTheRealErrorAndSendsNothing\)/);
  for (const row of ['2001:db8::1', '::1', 'fe80::1', 'ff02::1', '::ffff:1.2.3.4', '"::"', '1.2.3.4'])
    assert.ok(test.includes(row), row);
  assert.match(read('.github/workflows/release.yml'), /StealthDeclaredNetwork\.\*/);
  assert.match(read('scripts/release_plan.py'), /StealthDeclaredNetwork\.\*/);
});
