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
// An IPv4-mapped literal (::ffff:a.b.c.d, ::ffff:0:0/96 only) is a second rule
// of the same wrapper and does not depend on the declaration: a Firefox-style
// socket (IPV6_V6ONLY on) connecting to it fails on Windows with WSAEADDRNOTAVAIL,
// while Linux's dual-stack socket would connect over IPv4. Every OS now fails the
// Windows way (PR_ADDRESS_NOT_AVAILABLE_ERROR, nothing sent). Only a direct
// connection is judged: a destination behind a SOCKS proxy travels to the proxy,
// and the mapped addresses the SOCKS layer builds for its own IPv4 proxy
// (FixupAddressFamily, NSPR's IPv6-to-IPv4 emulation) are not a page's literal.
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
  // The IPv4-mapped literal is WSAEADDRNOTAVAIL too, on every OS.
  const mapped = body(impl, 'PRErrorCode StealthConnectError(');
  assert.match(mapped, /IPv6ADDR_IS_V4MAPPED\(&ip\)\) \{\s+return PR_ADDRESS_NOT_AVAILABLE_ERROR;/);
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
  // An IPv4-mapped address is not a route question: StealthConnectError judges it.
  assert.match(fn, /if \(IPv6ADDR_IS_V4MAPPED\(&ip\)\) \{\s+return 0;/);
  // The classes the host answers the same way on every route table.
  assert.match(fn, /IPv6ADDR_IS_UNSPECIFIED\(&ip\)/);
  assert.match(fn, /ip\.u8\[0\] == 0xff/);
  // The predicate does not read prefs or the host.
  assert.doesNotMatch(fn, /StaticPrefs|Preferences::|getenv|PR_Connect|PR_SetError|PR_GetError/);
});

test('the IPv4-mapped rule is pure, independent of the declaration, direct-only, and ::ffff:0:0/96 only', () => {
  const header = read('netwerk/base/StealthIPv6Literal.h');
  assert.match(header, /enum class StealthConnectTarget \{\s+kDirect,\s+kBehindSocksProxy,\s+kSocksProxyHop,?\s+\};/);
  assert.match(header, /PRErrorCode StealthConnectError\(const NetAddr& aAddr,\s+StealthConnectTarget aTarget,\s+bool aIPv6Disabled\);/);
  assert.doesNotMatch(header, /is IPv4's and is left alone/);
  const impl = read('netwerk/base/StealthIPv6Literal.cpp');
  assert.doesNotMatch(impl, /IPv4's, not ours/);
  const fn = body(impl, 'PRErrorCode StealthConnectError(');
  assert.doesNotMatch(fn, /StaticPrefs|Preferences::|getenv|PR_Connect|PR_SetError|PR_GetError/);
  // Only the direct connection to a page-chosen literal: both exemptions come first.
  const behind = fn.indexOf('StealthConnectTarget::kBehindSocksProxy');
  const direct = fn.indexOf('aTarget == StealthConnectTarget::kDirect');
  const mappedAt = fn.indexOf('IPv6ADDR_IS_V4MAPPED(&ip)');
  assert.ok(behind >= 0 && direct >= 0 && mappedAt >= 0);
  assert.ok(behind < mappedAt, 'a destination behind a proxy is never judged');
  assert.ok(direct < mappedAt, 'only a direct connection is judged as a mapped literal: the proxy hop of the SOCKS layer is not');
  // The mapped rule does not look at the declaration: it sits before the pref-gated route rule.
  assert.ok(mappedAt < fn.indexOf('StealthIPv6ConnectError('), 'mapped is judged before the route rule');
  // The macro is ::ffff:0:0/96: the first 80 bits zero, then ffff. Not ::ffff:0:a.b.c.d, not ::a.b.c.d.
  const dns = read('netwerk/dns/DNS.h');
  assert.match(dns, /#define IPv6ADDR_IS_V4MAPPED\(a\)\s+\\\s+\(\(\(a\)->u32\[0\] == 0\) && \(\(a\)->u32\[1\] == 0\) && \(\(a\)->u8\[8\] == 0\) && \\\s+\(\(a\)->u8\[9\] == 0\) && \(\(a\)->u8\[10\] == 0xff\) && \(\(a\)->u8\[11\] == 0xff\)\)/);
  // The family check keeps IPv4 destinations out: they are AF_INET, never mapped.
  assert.match(fn, /aAddr\.raw\.family == AF_INET6/);
});

test('a page literal reaches the hook as a mapped AF_INET6 address: the URL, the resolver and NSPR keep it', () => {
  // The URL host is serialized as hex groups (::ffff:102:304), never folded to dotted IPv4.
  const mozurl = read('netwerk/base/mozurl/src/lib.rs');
  assert.match(mozurl, /pub extern "C" fn rusturl_parse_ipv6addr[\s\S]*url::Host::parse\(ip6\)[\s\S]*write!\(addr, "\{host\}"\)/);
  const host = read('third_party/rust/url/src/host.rs');
  const w = body(host, 'fn write_ipv6(addr: &Ipv6Addr, f: &mut Formatter<\'_>) -> fmt::Result {');
  assert.match(w, /write!\(f, "\{:x\}", segments\[i as usize\]\)/);
  assert.doesNotMatch(w, /to_ipv4|octets\(\)/);
  // The resolver parses the host with NetAddr::InitFromString and hands the result over as it is.
  const resolver = read('netwerk/dns/nsHostResolver.cpp');
  assert.match(resolver, /NS_SUCCEEDED\(tempAddr\.InitFromString\(host\)\)[\s\S]{0,300}result = FromIPLiteral\(addrRec, tempAddr\);/);
  assert.match(resolver, /aAddrRec->addr = MakeUnique<NetAddr>\(aAddr\);/);
  const dns = read('netwerk/dns/DNS.cpp');
  assert.match(dns, /PR_StringToNetAddr\(PromiseFlatCString\(aString\)\.get\(\), &prAddr\)/);
  // NSPR's parser answers PR_AF_INET6 for a mapped literal.
  const nspr = read('nsprpub/pr/src/misc/prnetdb.c');
  assert.match(nspr, /rv = StringToV6Addr\(string, &addr->ipv6\.ip\);\s+if \(1 == rv\) \{\s+addr->raw\.family = PR_AF_INET6;/);
  // The socket takes that address from the resolver record, the family intact.
  const sock = read('netwerk/base/nsSocketTransport2.cpp');
  assert.match(sock, /mDNSRecord->GetNextAddr\(SocketPort\(\), &mNetAddr\)/);
});

test('an IPv4 destination is AF_INET, never mapped: nothing in the resolver or the socket layer builds a mapped address', () => {
  // getaddrinfo is not asked for mapped answers (no PR_AI_V4MAPPED / AI_V4MAPPED).
  assert.doesNotMatch(read('netwerk/dns/GetAddrInfo.cpp'), /V4MAPPED/);
  // The only mapped address the network code builds is the SOCKS layer's own proxy address.
  const socks = read('netwerk/socket/nsSOCKSIOLayer.cpp');
  const fix = body(socks, 'void nsSOCKSSocketInfo::FixupAddressFamily(PRFileDesc* fd, NetAddr* proxy) {');
  assert.match(fix, /memset\(proxyp \+ 10, 0xff, 2\);/);
  assert.match(fix, /\(char\*\)&mInternalProxyAddr\.inet\.ip/);
  const connect = body(socks, 'PRStatus nsSOCKSSocketInfo::ConnectToProxy(PRFileDesc* fd) {');
  assert.ok(connect.indexOf('FixupAddressFamily(fd, &proxy)') < connect.indexOf('StealthConnect('));
  assert.match(connect, /StealthConnect\(fd->lower, proxy, StealthConnectTarget::kSocksProxyHop,/);
  for (const rel of ['netwerk/dns/nsHostResolver.cpp', 'netwerk/dns/DNSUtils.cpp', 'netwerk/dns/nsDNSService2.cpp', 'netwerk/dns/TRR.cpp', 'netwerk/dns/DNSPacket.cpp'])
    assert.doesNotMatch(read(rel), /u8\[10\] = 0xff|memset\([^)]*0xff, 2\)|IPv6ADDR_IS_V4MAPPED|PR_IpAddrV4Mapped/, rel);
});

test('the connect wrapper reads the declaration, fails without touching the network, and otherwise is PR_Connect', () => {
  const impl = read('netwerk/base/StealthIPv6Literal.cpp');
  const fn = body(impl, 'PRStatus StealthConnect(PRFileDesc* aFd, const NetAddr& aAddr,');
  // The target (behind a proxy, the SOCKS hop, direct) and the declaration are the predicate's inputs.
  assert.match(fn, /StealthConnectError\(\s*aAddr, aTarget, StaticPrefs::network_dns_disableIPv6\(\)\)/);
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
  const lines = init.split('\n').filter(l => l.includes('StealthConnect('));
  assert.equal(lines.length, 1);
  assert.ok(lines[0].includes(HOOK), 'the hook is on the line');
  // Behind a SOCKS proxy mNetAddr is the destination, which the proxy reaches, not this host:
  // neither the IPv6 route rule nor the IPv4-mapped rule applies to it.
  const at = init.indexOf('StealthConnect(');
  assert.match(init.slice(Math.max(0, at - 500), at + 500),
    /mProxyTransparent && !mProxyHost\.IsEmpty\(\)\s*\?\s*StealthConnectTarget::kBehindSocksProxy\s*:\s*StealthConnectTarget::kDirect/);
  // The failure is read like a connect() failure, on the very next line.
  assert.match(init.slice(at, at + 400), /PRErrorCode code = PR_GetError\(\);/);
  assert.equal(sock.match(/StealthConnect\(/g).length, 1);
});

test('the SOCKS layer reaches its proxy through the wrapper: the route rule applies, the mapped rule does not', () => {
  const socks = read('netwerk/socket/nsSOCKSIOLayer.cpp');
  assert.match(socks, /#include "StealthIPv6Literal\.h"\s+\/\/ STEALTH-HOOK\(network\/ipv6-literal\)/);
  const fn = body(socks, 'PRStatus nsSOCKSSocketInfo::ConnectToProxy(PRFileDesc* fd) {');
  const lines = fn.split('\n').filter(l => l.includes('StealthConnect('));
  assert.equal(lines.length, 1);
  assert.ok(lines[0].includes(HOOK), 'the hook is on the line');
  assert.match(lines[0], /StealthConnect\(fd->lower, proxy, StealthConnectTarget::kSocksProxyHop, &prProxy, mTimeout\)/);
  assert.equal(fn.match(/methods->connect\(/g), null);
});

test('no other code decides: neither the HTTP channel, nor the DNS service, nor the resolver is hooked', () => {
  for (const rel of ['netwerk/protocol/http/nsHttpChannel.cpp', 'netwerk/dns/nsHostResolver.cpp',
    'netwerk/dns/nsDNSService2.cpp', 'netwerk/protocol/websocket/WebSocketChannel.cpp'])
    assert.doesNotMatch(read(rel), /StealthIPv6/, rel);
});

test('the build lists the source, and the gtest runs in a suite the candidate filter selects', () => {
  assert.match(read('netwerk/base/moz.build'), /UNIFIED_SOURCES \+= \["StealthIPv6Literal\.cpp"\]\s+# STEALTH-HOOK\(network\/ipv6-literal\)/);
  assert.match(read('netwerk/test/gtest/moz.build'), /include\("stealth\.build"\)  # STEALTH-HOOK\(network\/socks\)/);
  assert.match(read('netwerk/test/gtest/stealth.build'), /SOURCES \+= \[[^\]]*"TestStealthIPv6Literal\.cpp",/);
  const test = read('netwerk/test/gtest/TestStealthIPv6Literal.cpp');
  assert.match(test, /TEST\(StealthDeclaredNetwork, AnIPv6LiteralFailsLikeAHostWithNoIPv6Route\)/);
  assert.match(test, /TEST\(StealthDeclaredNetwork, NothingIsBlockedWhileIPv6IsDeclared\)/);
  assert.match(test, /TEST\(StealthDeclaredNetwork, TheConnectFailsWithTheRealErrorAndSendsNothing\)/);
  assert.match(test, /TEST\(StealthDeclaredNetwork, AnIPv4MappedLiteralFailsLikeWindowsWhateverTheDeclaration\)/);
  assert.match(test, /TEST\(StealthDeclaredNetwork, AnIPv4MappedAddressIsLeftAloneBehindAProxyAndOnTheProxyHop\)/);
  for (const row of ['2001:db8::1', '::1', 'fe80::1', 'ff02::1', '::ffff:192.0.2.4', '::ffff:127.0.0.1', '::ffff:0:192.0.2.4', '::192.0.2.4', '"::"', '192.0.2.4'])
    assert.ok(test.includes(row), row);
  assert.match(read('.github/workflows/release.yml'), /StealthDeclaredNetwork\.\*/);
  assert.match(read('scripts/release_plan.py'), /StealthDeclaredNetwork\.\*/);
});
