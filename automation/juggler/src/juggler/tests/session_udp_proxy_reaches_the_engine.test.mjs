// The proxy the SESSION uses is the one the engine's UDP layer connects to.
//
// A Juggler session sets its proxy with Browser.setBrowserProxy /
// Browser.setContextProxy and never writes network.proxy.* (the type stays 5),
// so the engine's SOCKS5 UDP layer, which read network.proxy.*, never armed and
// WebRTC's UDP could not go through a proxy that relays it. SessionRoute.js now
// hands the session's SOCKS5 proxy to nsIStealthSessionUdpProxy (juggler/udp)
// when the core's verdict network.proxy.socks_remote_udp is true, and
// TargetRegistry.js does it wherever a proxy appears or goes. The service keeps
// the credentials in the parent and sends them to the socket process by IPC:
// they are in no pref (docs/decisions/motore-06), so no content process has them.
//
// Red on the old code: the functions did not exist, and no call site did.
//
// Run: node --test juggler/tests/*.test.mjs

import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const JUGGLER = path.join(path.dirname(fileURLToPath(import.meta.url)), '..');
const TREE = path.join(JUGGLER, '..');
const read = rel => fs.readFileSync(path.join(TREE, rel), 'utf8');

function loadSessionRoute() {
  let src = read('juggler/SessionRoute.js');
  src = src.replace(/^export /gm, '');
  src += '\nmodule.exports = { sessionUdpProxy, publishSessionUdpProxy };\n';
  const sandbox = { module: { exports: {} }, ChromeUtils: {}, Cc: {}, Ci: {} };
  vm.runInNewContext(src, sandbox);
  return sandbox.module.exports;
}

const { sessionUdpProxy, publishSessionUdpProxy } = loadSessionRoute();

const socks = (over = {}) => ({
  type: 'socks', host: '203.0.113.10', port: 1080, username: 'u', password: 'p', bypass: [], ...over,
});

// The service, remembering what it was handed.
function fakeService() {
  const calls = [];
  return {
    calls,
    set: (...args) => calls.push(['set', ...args]),
    clear: () => calls.push(['clear']),
    held() {
      const last = calls.at(-1);
      return last && last[0] === 'set' ? last.slice(1) : null;
    },
  };
}

test('the engine declares the bool a content process reads, and no pref holds the proxy', () => {
  const yaml = read('modules/libpref/init/StealthPrefList.yaml');
  for (const name of ['zoom.stealth.udp_proxy.armed', 'network.proxy.socks_remote_udp'])
    assert.ok(yaml.includes(`- name: ${name}\n`), `${name} is not declared in StealthPrefList.yaml`);
  assert.doesNotMatch(yaml, /- name: zoom\.stealth\.udp_proxy\.(host|port|user|pass)/);
  // Nothing of the session proxy is written to a pref by the Juggler.
  for (const file of ['juggler/SessionRoute.js', 'juggler/TargetRegistry.js']) {
    const text = read(file);
    assert.doesNotMatch(text, /udp_proxy\.(host|port|user|pass)/, file);
    assert.doesNotMatch(text, /getDefaultBranch/, file);
  }
});

test('a SOCKS5 browser proxy is handed over with its credentials', () => {
  const service = fakeService();
  assert.equal(publishSessionUdpProxy(service, true, socks(), []), true);
  assert.deepEqual(service.held(), ['203.0.113.10', 1080, 'u', 'p']);
});

test('without the core verdict nothing is held, and what was held goes', () => {
  const service = fakeService();
  publishSessionUdpProxy(service, true, socks(), []);
  assert.equal(publishSessionUdpProxy(service, false, socks(), []), false);
  assert.deepEqual(service.calls.at(-1), ['clear']);
});

test('HTTP, HTTPS and SOCKS4 have no UDP: nothing is held', () => {
  for (const type of ['http', 'https', 'socks4']) {
    const service = fakeService();
    assert.equal(publishSessionUdpProxy(service, true, socks({ type }), []), false, type);
    assert.deepEqual(service.calls, [['clear']], type);
  }
});

test('a proxy with no credentials is handed over with empty ones', () => {
  const service = fakeService();
  const proxy = socks();
  delete proxy.username;
  delete proxy.password;
  assert.equal(publishSessionUdpProxy(service, true, proxy, []), true);
  assert.deepEqual(service.held(), ['203.0.113.10', 1080, '', '']);
});

test('a context with no proxy of its own follows the browser proxy', () => {
  const service = fakeService();
  assert.equal(publishSessionUdpProxy(service, true, socks(), [null, null]), true);
  assert.deepEqual(service.held().slice(0, 2), ['203.0.113.10', 1080]);
});

test('contexts that all use the same proxy share it; a request with no context needs the browser proxy', () => {
  const service = fakeService();
  // With no browser proxy a request without a context goes direct: not one road.
  assert.equal(publishSessionUdpProxy(service, true, null, [socks(), socks()]), false);
  assert.equal(publishSessionUdpProxy(service, true, socks(), [socks(), null]), true);
});

test('two different proxies in one session cannot be told apart at a UDP socket: nothing is held', () => {
  const service = fakeService();
  publishSessionUdpProxy(service, true, socks(), []);
  assert.equal(publishSessionUdpProxy(service, true, socks(), [socks({ host: '10.0.0.9' })]), false);
  assert.deepEqual(service.calls.at(-1), ['clear']);
  assert.equal(publishSessionUdpProxy(service, true, socks(), [socks({ password: 'other' })]), false);
});

test('a context that goes direct next to one that does not holds nothing', () => {
  const service = fakeService();
  assert.equal(publishSessionUdpProxy(service, true, null, [socks(), null]), false);
});

test('a malformed endpoint is never handed over', () => {
  assert.equal(sessionUdpProxy([socks({ port: 0 })]), null);
  assert.equal(sessionUdpProxy([socks({ port: 70000 })]), null);
  assert.equal(sessionUdpProxy([socks({ host: '' })]), null);
  assert.equal(sessionUdpProxy([]), null);
});

test('the registry calls it wherever a proxy appears, changes or goes away', () => {
  const text = read('juggler/TargetRegistry.js');
  // The browser proxy, a context proxy, and the end of a context (its proxy goes with it).
  const calls = text.split('\n').filter(
      line => line.includes('_publishSessionUdpProxy()') && !line.includes('_publishSessionUdpProxy() {'));
  assert.equal(calls.length, 3, calls.join('\n'));
  const body = text.slice(text.indexOf('setBrowserProxy(proxy) {'));
  assert.ok(body.indexOf('_publishSessionUdpProxy()') > body.indexOf('this._browserProxy = proxy;'));
  const setProxy = text.slice(text.indexOf('  setProxy(proxy) {'));
  assert.ok(setProxy.indexOf('_publishSessionUdpProxy()') > setProxy.indexOf('this._proxy = proxy;'));
  const destroy = text.slice(text.indexOf('  async destroy() {'));
  assert.ok(destroy.indexOf('_publishSessionUdpProxy()') >
            destroy.indexOf('_browserContextIdToBrowserContext.delete(this.browserContextId)'));
  // The verdict is read where it is handed over, through the service, and a failure never prints a value.
  assert.match(text, /getBoolPref\('network\.proxy\.socks_remote_udp', false\)/);
  assert.match(text, /@mozilla\.org\/juggler\/stealthsessionudpproxy;1/);
  assert.doesNotMatch(text, /reportError\([^)]*(password|username)/);
});

test('no credential is ever printed by the module that hands them over', () => {
  const text = read('juggler/SessionRoute.js');
  const publisher = text.slice(text.indexOf('export function publishSessionUdpProxy'));
  assert.doesNotMatch(publisher, /console\.|dump\(|reportError|log\(/);
});

test('the service exists, is registered, and only the parent holds the proxy', () => {
  const idl = read('juggler/udp/nsIStealthSessionUdpProxy.idl');
  assert.match(idl, /interface nsIStealthSessionUdpProxy : nsISupports/);
  assert.match(idl, /void set\(in ACString aHost, in long aPort, in ACString aUser, in ACString aPass\);/);
  assert.match(idl, /void clear\(\);/);
  assert.match(read('juggler/udp/components.conf'), /@mozilla\.org\/juggler\/stealthsessionudpproxy;1/);
  assert.match(read('juggler/moz.build'), /DIRS \+= \["components", "pipe", "udp"\]/);
  const cpp = read('juggler/udp/StealthSessionUdpProxyService.cpp');
  assert.match(cpp, /XRE_IsParentProcess\(\)/);
  assert.doesNotMatch(cpp, /Preferences::|printf|MOZ_LOG|NS_WARNING|dump/);
});

test('the proxy reaches the socket process by IPC, now and after every (re)launch', () => {
  // ICE runs in the socket process, and the Juggler hands the proxy over after it has started: the message is
  // sent on every change, and again when a socket process (re)launches, which has none.
  const ipdl = read('netwerk/ipc/PSocketProcess.ipdl');
  const child = ipdl.slice(ipdl.indexOf('child:'));
  assert.match(child, /async StealthSessionUdpProxy\(nsCString aHost, int32_t aPort, nsCString aUser, nsCString aPass\);\s+\/\/ STEALTH-HOOK\(network\/proxy-udp\)/);
  assert.match(read('netwerk/ipc/SocketProcessChild.h'), /RecvStealthSessionUdpProxy\(.*\);\s+\/\/ STEALTH-HOOK\(network\/proxy-udp\)/);
  const io = read('netwerk/base/nsIOService.cpp');
  const launched = io.slice(io.indexOf('void nsIOService::OnProcessLaunchComplete('));
  const body = launched.slice(0, launched.indexOf('\n}\n'));
  const afterGuard = body.indexOf('return;', body.indexOf('!aSucceeded'));
  assert.ok(afterGuard > 0 && body.indexOf('StealthSessionUdpProxyResend();') > afterGuard,
            'the resend must come after the launch is known to have succeeded');
  // The sender and the receiver are in our file, and no pref carries the proxy to the socket process.
  const layer = read('netwerk/socket/nsSOCKSUDPIOLayer.cpp');
  assert.match(layer, /SendStealthSessionUdpProxy\(/);
  assert.match(layer, /SocketProcessChild::RecvStealthSessionUdpProxy\(/);
  assert.doesNotMatch(io, /zoom\.stealth\.udp_proxy/);
});

test('no credential is logged by the C++ that holds them', () => {
  const layer = read('netwerk/socket/nsSOCKSUDPIOLayer.cpp');
  for (const line of layer.split('\n').filter(l => /LOGE?\(\(|MOZ_LOG|printf/.test(l)))
    assert.doesNotMatch(line, /user\.get\(\)|pass\.get\(\)|aProxyPassword|aProxyUsername|mPass|mUser/, line);
});
