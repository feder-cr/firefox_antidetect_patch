/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "DNSServiceBase.h"

#include "DNS.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_network.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "nsIDNSService.h"
#include "nsIProtocolProxyService2.h"
#include "nsIPrefBranch.h"
#include "nsIProxyInfo.h"
#include "nsThreadUtils.h"
#include "mozilla/ClearOnShutdown.h"

#if defined(XP_WIN)
#  include <shlobj.h>
#endif

#include "DNSLogging.h"

namespace mozilla::net {

static const char kPrefProxyType[] = "network.proxy.type";
static const char kPrefDisablePrefetch[] = "network.dns.disablePrefetch";
static const char kPrefNetworkProxySOCKS[] = "network.proxy.socks";
static const char kPrefNetworkProxySOCKSVersion[] =
    "network.proxy.socks_version";

NS_IMPL_ISUPPORTS(DNSServiceBase, nsIObserver)

void DNSServiceBase::AddPrefObserver(nsIPrefBranch* aPrefs) {
  aPrefs->AddObserver(kPrefProxyType, this, false);
  aPrefs->AddObserver(kPrefDisablePrefetch, this, false);
  // Monitor these to see if there is a change in proxy configuration
  aPrefs->AddObserver(kPrefNetworkProxySOCKS, this, false);
  aPrefs->AddObserver(kPrefNetworkProxySOCKSVersion, this, false);
}

void DNSServiceBase::ReadPrefs(const char* aName) {
  if (!aName || !strcmp(aName, kPrefNetworkProxySOCKS) ||
      !strcmp(aName, kPrefNetworkProxySOCKSVersion)) {
    uint32_t socksVersion = Preferences::GetInt(kPrefNetworkProxySOCKSVersion);
    nsAutoCString socks;
    if (NS_SUCCEEDED(Preferences::GetCString(kPrefNetworkProxySOCKS, socks))) {
      mSocksProxyVersion = 0;
      if (!socks.IsEmpty()) {
        if (socksVersion == nsIProxyInfo::SOCKS_V4) {
          mSocksProxyVersion = nsIProxyInfo::SOCKS_V4;
        } else if (socksVersion == nsIProxyInfo::SOCKS_V5) {
          mSocksProxyVersion = nsIProxyInfo::SOCKS_V5;
        }
      }
    }
  }
  if (!aName || !strcmp(aName, kPrefDisablePrefetch) ||
      !strcmp(aName, kPrefProxyType)) {
    mDisablePrefetch = Preferences::GetBool(kPrefDisablePrefetch, false) ||
                       (StaticPrefs::network_proxy_type() ==
                        nsIProtocolProxyService::PROXYCONFIG_MANUAL);
  }
}

bool DNSServiceBase::DNSForbiddenByActiveProxy(const nsACString& aHostname,
                                               uint32_t aFlags) {
  if (aFlags & nsIDNSService::RESOLVE_IGNORE_SOCKS_DNS) {
    return false;
  }

  // Stealthfox: the check below can only see a proxy that lives in the
  // network.proxy.* prefs, and one of our two supported shapes does not. For an
  // HTTP/HTTPS endpoint the routing is done per-channel by the automation layer
  // - it is the only side that can answer the proxy's 407 - so network.proxy.type
  // stays PROXYCONFIG_SYSTEM and the guard below never arms. Channels are fine
  // either way (the channel filter marks them TRANSPARENT_PROXY_RESOLVES_HOST),
  // but three surfaces are NOT channels and call AsyncResolveNative directly:
  // NetworkConnectivityService, the DoH heuristics, and the ICE resolver. The
  // last one takes its hostname from the PAGE, via RTCPeerConnection's
  // iceServers, so a site can have the browser look up a name it chose and watch
  // which resolver asks - the user's own, while the HTTP goes out of the proxy.
  //
  // So the fact is declared by the side that knows it. The pref is false by
  // default: a build launched without our launcher behaves exactly as upstream.
  // This is the same TODO the comment below points at (Bug 1890542) approached
  // from the caller's side rather than from nsIProxyInfo.
  const bool proxyDeclaredByLauncher =
      StaticPrefs::zoom_stealth_dns_no_local_resolution();

  // TODO(Bug 1890542): use nsIProxyInfo object whether sending DNS request to
  //     local network is fine.
  // We should avoid doing DNS when a proxy is in use.
  if (proxyDeclaredByLauncher ||
      (StaticPrefs::network_proxy_type() ==
           nsIProtocolProxyService::PROXYCONFIG_MANUAL &&
       ((mSocksProxyVersion == nsIProxyInfo::SOCKS_V4 &&
         StaticPrefs::network_proxy_socks_remote_dns()) ||
        (mSocksProxyVersion == nsIProxyInfo::SOCKS_V5 &&
         StaticPrefs::network_proxy_socks5_remote_dns())))) {
    // Allow IP lookups through, but nothing else.
    if (!HostIsIPLiteral(aHostname)) {
      return true;
    }
  }
  return false;
}

void DNSServiceBase::DoReadEtcHostsFile(ParsingCallback aCallback) {
  MOZ_ASSERT(XRE_IsParentProcess());

  if (!StaticPrefs::network_trr_exclude_etc_hosts()) {
    return;
  }

  auto readHostsTask = [aCallback]() {
    MOZ_ASSERT(!NS_IsMainThread(), "Must not run on the main thread");
#if defined(XP_WIN)
    nsCString path;
    path.SetLength(MAX_PATH + 1);
    if (!SHGetSpecialFolderPathA(NULL, path.BeginWriting(), CSIDL_SYSTEM,
                                 false)) {
      LOG(("Calling SHGetSpecialFolderPathA failed"));
      return;
    }

    path.SetLength(strlen(path.get()));
    path.Append("\\drivers\\etc\\hosts");
#else
    nsAutoCString path("/etc/hosts"_ns);
#endif

    LOG(("Reading hosts file at %s", path.get()));
    rust_parse_etc_hosts(&path, aCallback);
  };

  (void)NS_DispatchBackgroundTask(
      NS_NewRunnableFunction("Read /etc/hosts file", readHostsTask),
      NS_DISPATCH_EVENT_MAY_BLOCK);
}

}  // namespace mozilla::net
