// A PeerConnection always has a proxy config. PeerConnectionImpl::GetProxyConfig returns nullptr when the window has no
// BrowserChild, no document, or when LoadInfo::Create fails, and then SetProxyConfig is never called: the UDP sockets are plain
// (NrSocket with no config is "left plain"), the TCP sockets open plain (WebrtcTCPSocket::Open with Nothing), and the names of the
// ICE servers are looked up in the default context. A page reaches it: new W.RTCPeerConnection() with W the window of an iframe
// that was removed, or of a popup that was closed. The constructor only asks IsCurrentInnerWindow, which a discarded browsing
// context still answers true for a window that was current; its docshell is gone (no BrowserChild) and its document has lost
// its ClientInfo (FreeInnerObjects), which LoadInfo::Create refuses for a load whose node is that document.
//
// The fix is at the call site, as one tagged line: the config is the one upstream made, or, when there is none, the config of the
// principal of the window (StealthProxyConfigOf, in StealthIceResolverContext.cpp). GetProxyConfig stays as Mozilla wrote it.
// Behaviour: dom/media/webrtc/transport/test/gtest/TestStealthPcProxyConfig.cpp. The whole PeerConnection of a removed iframe and of
// a closed popup is a test of the wrapper's fake web (it needs a browser); it is not here.
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
const PC = 'dom/media/webrtc/jsapi/PeerConnectionImpl.cpp';

test('the config the PeerConnection gives its ICE context goes through our function, in one tagged line', () => {
  const text = read(PC);
  // Mozilla's line stays as it is; ours is beside it, right after, and before the config is tested and used.
  const call = '  auto proxyConfig = GetProxyConfig();\n' +
      '  proxyConfig = StealthProxyConfigOf(std::move(proxyConfig), mWindow.get(), mForceProxy);  // STEALTH-HOOK(network/proxy-udp)\n';
  assert.ok(text.includes(call), 'SetConfiguration still uses GetProxyConfig() alone');
  assert.equal(text.match(/StealthProxyConfigOf\(/g).length, 1);
  // The null config is still tested, after the hook: nothing is sent to the transport without one.
  assert.ok(text.slice(text.indexOf(call) + call.length).startsWith('  if (proxyConfig) {'));
  assert.match(text, /\n#include "transport\/StealthIceResolverContext\.h"  \/\/ STEALTH-HOOK\(network\/proxy-udp\)\n/);
  // The new entry: two tagged lines, the include and the call.
  assert.equal(text.split('\n').filter(l => /STEALTH-HOOK\(/.test(l)).length, 2);
});

test('GetProxyConfig is as Mozilla wrote it: the fix is at the call, not in the function', () => {
  const text = read(PC);
  const body = text.match(/PeerConnectionImpl::GetProxyConfig\(\)\s+const \{[\s\S]*?\n}\n/)[0];
  assert.equal(body.match(/return nullptr;/g).length, 4, 'the function was edited');
  assert.match(body, /auto\* browserChild = BrowserChild::GetFrom\(mWindow\);/);
  assert.match(body, /Document\* doc = mWindow->GetExtantDoc\(\);/);
  assert.doesNotMatch(body, /Stealth/);
});

test('our function: upstream\'s config first, its opt-out next, then the principal of the window, a tab id of 0 and no node', () => {
  const header = read(T + 'StealthIceResolverContext.h');
  assert.match(header, /std::unique_ptr<NrSocketProxyConfig> StealthProxyConfigOf\(\n\s+std::unique_ptr<NrSocketProxyConfig> aUpstream, nsPIDOMWindowInner\* aWindow,\n\s+bool aForceProxy\);/);
  assert.match(header, /std::unique_ptr<NrSocketProxyConfig> StealthProxyConfigOfPrincipal\(\n\s+nsIPrincipal\* aPrincipal, uint64_t aTabId, bool aForceProxy\);/);
  const code = read(T + 'StealthIceResolverContext.cpp');
  const of = code.match(/std::unique_ptr<NrSocketProxyConfig> StealthProxyConfigOf\(\n[\s\S]*?\n}\n/)[0];
  const upstream = of.indexOf('if (aUpstream) {\n    return aUpstream;\n  }');
  const optOut = of.indexOf('Preferences::GetBool("media.peerconnection.disable_http_proxy", false)');
  const principal = of.indexOf('scriptPrincipal->GetPrincipal()');
  assert.ok(upstream >= 0 && optOut > upstream && principal > optOut, 'upstream, opt-out, principal, in that order');
  assert.match(of, /if \(!aForceProxy &&\n\s+Preferences::GetBool/);
  assert.match(of, /tabId = browserChild->GetTabId\(\);/);
  assert.match(of, /StealthProxyConfigOfPrincipal\(principal, tabId, aForceProxy\);/);
  // A config that cannot be made for the principal is made for none, which forces the proxy: never no config at all.
  assert.match(of, /if \(!config && principal\) \{[\s\S]*?config = StealthProxyConfigOfPrincipal\(nullptr, tabId, true\);\n\s+\}\n\s+return config;/);
  const core = code.match(/std::unique_ptr<NrSocketProxyConfig> StealthProxyConfigOfPrincipal\([\s\S]*?\n}\n/)[0];
  assert.match(core, /principal = NullPrincipal::CreateWithoutOriginAttributes\(\);\n\s+forceProxy = true;/);
  assert.match(core, /net::LoadInfo::Create\(\n\s+principal, principal, nullptr,/);
  assert.match(core, /nsIContentPolicy::TYPE_PROXIED_WEBRTC_MEDIA/);
  assert.match(core, /net::WebrtcProxyConfig\(dom::TabId\(aTabId\), loadInfoArgs, forceProxy\)/);
});

test('premises, so a new upstream that changes them is noticed: a discarded window is still "current", loses its ClientInfo, and a load with no node is not refused', () => {
  const win = read('dom/base/nsGlobalWindowInner.cpp');
  assert.match(win, /\/\/ still-current if we were current at the time it was discarded\.\n    return mOuterWindow && WasCurrentInnerWindow\(\);/);
  const free = win.slice(win.indexOf('void nsGlobalWindowInner::FreeInnerObjects()'));
  assert.ok(free.slice(0, 6000).includes('mClientSource.reset();'));
  const info = read('netwerk/base/LoadInfo.cpp');
  assert.match(info, /\/\/ No node means no document, so there is nothing to check\.\n  nsCOMPtr<nsINode> node = LoadingNode\(\);\n  if \(!node\) \{\n    return false;\n  \}/);
  assert.match(info, /if \(loadInfo->IsDocumentMissingClientInfo\(\)\) \{\n    return Err\(NS_ERROR_CONTENT_BLOCKED\);/);
  assert.match(read('dom/bindings/BindingUtils.cpp'), /if \(!window->IsCurrentInnerWindow\(\)\) \{\n\s+aRv\.ThrowInvalidStateError\("Window no longer active"\);/);
  // What a missing config meant: plain UDP and plain TCP.
  assert.match(read(T + 'nr_stealth_proxy_udp.h'), /A socket with no proxy config \(not of a\s+\* PeerConnection\) or of a PeerConnection that has no proxy is left plain/);
  assert.match(read(T + 'ipc/WebrtcTCPSocket.cpp'), /if \(!mProxyConfig\.isSome\(\)\) \{\n    OpenWithoutHttpProxy\(nullptr\);/);
});

test('the gtest is built and the sources of the new code are the ones already built', () => {
  assert.match(read(T + 'test/gtest/moz.build'), /\nSOURCES \+= \["TestStealthPcProxyConfig\.cpp"\]\n/);
  assert.match(read(T + 'common.build'), /\ntransport_lcppsrcs \+= \["StealthIceResolverContext\.cpp"\]  # STEALTH-HOOK\(network\/doh\)\n/);
  const test = read(T + 'test/gtest/TestStealthPcProxyConfig.cpp');
  assert.match(test, /StealthProxyConfigOfPrincipal\(PagePrincipalOfContext\(6\)\.get\(\), 0, false\)/);
  assert.match(test, /EXPECT_EQ\(6u, Of\(config\)\.loadInfoArgs\(\)\.originAttributes\(\)\.mUserContextId\)/);
});
