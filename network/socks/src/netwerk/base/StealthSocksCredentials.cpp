/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthSocksCredentials.h"

#include <string.h>

#include "mozilla/Assertions.h"
#include "nsIPrefBranch.h"
#include "nsString.h"

namespace mozilla::net {

namespace {

// Spelled out, not built with nsProtocolProxyService.cpp's PROXY_PREF macro:
// the two files can share one unified translation unit and a macro of that
// file must not be needed (or redefined) here.
constexpr char kStealthSocksUsernamePref[] = "network.proxy.socks_username";
constexpr char kStealthSocksPasswordPref[] = "network.proxy.socks_password";

// Same reading as proxy_GetStringPref in nsProtocolProxyService.cpp (a
// file-static there): a failed read empties the value, a successful one is
// assigned and stripped of whitespace. Another name on purpose: in a unified
// build both files can be one translation unit, and a second static function
// called proxy_GetStringPref would not compile.
void StealthSocksReadStringPref(nsIPrefBranch* aPrefBranch, const char* aPref,
                                nsCString& aResult) {
  nsAutoCString temp;
  nsresult rv = aPrefBranch->GetCharPref(aPref, temp);
  if (NS_FAILED(rv)) {
    aResult.Truncate();
  } else {
    aResult.Assign(temp);
    aResult.StripWhitespace();
  }
}

}  // namespace

void StealthSocksPrefsChanged(nsIPrefBranch* aPrefBranch, const char* aPref,
                              nsCString& aUsername, nsCString& aPassword) {
  MOZ_ASSERT(aPrefBranch);

  if (!aPref || !strcmp(aPref, kStealthSocksUsernamePref)) {
    StealthSocksReadStringPref(aPrefBranch, kStealthSocksUsernamePref,
                               aUsername);
  }

  if (!aPref || !strcmp(aPref, kStealthSocksPasswordPref)) {
    StealthSocksReadStringPref(aPrefBranch, kStealthSocksPasswordPref,
                               aPassword);
  }
}

}  // namespace mozilla::net
