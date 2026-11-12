/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthSessionResolver.h"

#include "TRRService.h"
#include "mozilla/OriginAttributes.h"
#include "mozilla/StaticPrefs_network.h"
#include "nsCOMPtr.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsIRequest.h"
#include "nsIStandardURL.h"
#include "nsIURIMutator.h"
#include "nsIURI.h"
#include "nsNetCID.h"
#include "nsNetUtil.h"
#include "nsString.h"

namespace mozilla::net {

// A context id written in decimal digits: no sign, no leading zero ("0" alone
// is the default context), and small enough for 32 bits.
static bool ParseContextId(const nsACString& aToken, uint32_t* aId) {
  if (aToken.IsEmpty() || aToken.Length() > 10 ||
      (aToken.Length() > 1 && aToken[0] == '0')) {
    return false;
  }
  uint64_t id = 0;
  for (uint32_t i = 0; i < aToken.Length(); i++) {
    if (aToken[i] < '0' || aToken[i] > '9') {
      return false;
    }
    id = id * 10 + (aToken[i] - '0');
  }
  if (id > UINT32_MAX) {
    return false;
  }
  *aId = static_cast<uint32_t>(id);
  return true;
}

bool StealthContextHasProxy(uint32_t aUserContextId, const nsACString& aList) {
  // Whitespace around a token is dropped, whitespace inside one is kept: "6 7"
  // is one token, which is no id.
  for (const nsACString& token :
       nsCCharSeparatedTokenizer(aList, ',').ToRange()) {
    uint32_t id;
    if (token.EqualsLiteral("*") ||
        (ParseContextId(token, &id) && id == aUserContextId)) {
      return true;
    }
  }
  return false;
}

nsIDNSService::DNSFlags StealthResolverFlags(nsIDNSService::DNSFlags aFlags,
                                             bool aContextHasProxy,
                                             bool aHostIsTRRServer) {
  if (aHostIsTRRServer) {
    return aFlags;
  }
  // The request mode lives in two bits of the flags. The IDL names the value
  // "disabled" (RESOLVE_TRR_DISABLED_MODE); TRR_ONLY is 3, both bits, which is
  // also RESOLVE_TRR_MODE_MASK, so it is built from the request mode.
  const nsIDNSService::DNSFlags mode =
      aContextHasProxy
          ? nsIDNSService::GetFlagsFromTRRMode(nsIRequest::TRR_ONLY_MODE)
          : nsIDNSService::RESOLVE_TRR_DISABLED_MODE;
  return static_cast<nsIDNSService::DNSFlags>(
      (aFlags & ~nsIDNSService::RESOLVE_TRR_MODE_MASK) | mode);
}

// The host name of the DoH server, as TRRService::MaybeBootstrap reads it from
// the service's URI.
static bool HostIsTRRServer(TRRService* aService, const nsACString& aHost) {
  nsAutoCString uri;
  aService->GetURI(uri);
  if (uri.IsEmpty()) {
    return false;
  }
  nsCOMPtr<nsIURI> url;
  nsresult rv =
      NS_MutateURI(NS_STANDARDURLMUTATOR_CONTRACTID)
          .Apply(&nsIStandardURLMutator::Init, nsIStandardURL::URLTYPE_STANDARD,
                 443, uri, nullptr, nullptr, nullptr)
          .Finalize(url);
  if (NS_FAILED(rv) || !url) {
    return false;
  }
  nsAutoCString serverHost;
  return NS_SUCCEEDED(url->GetAsciiHost(serverHost)) &&
         aHost.Equals(serverHost, nsCaseInsensitiveCStringComparator);
}

nsIDNSService::DNSFlags StealthFlagsFollowSession(
    const nsACString& aHost, const OriginAttributes& aAttrs,
    nsIDNSService::DNSFlags aFlags) {
  TRRService* service = TRRService::Get();
  if (!service) {
    return aFlags;
  }
  nsAutoCString contexts;
  {
    auto declared = StaticPrefs::network_trr_stealth_contexts();
    contexts = *declared;
  }
  // Only the DoH request itself asks for "TRR disabled" for the name of its
  // server, so any other request is not worth reading the server's URI for.
  const bool hostIsTRRServer =
      (aFlags & nsIDNSService::RESOLVE_TRR_MODE_MASK) ==
          nsIDNSService::RESOLVE_TRR_DISABLED_MODE &&
      HostIsTRRServer(service, aHost);
  return StealthResolverFlags(
      aFlags, StealthContextHasProxy(aAttrs.mUserContextId, contexts),
      hostIsTRRServer);
}

}  // namespace mozilla::net
