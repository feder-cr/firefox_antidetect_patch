/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_StealthSocksCredentials_h
#define mozilla_net_StealthSocksCredentials_h

#include "nsStringFwd.h"

class nsIPrefBranch;

namespace mozilla::net {

/**
 * The SOCKS proxy credentials, read from network.proxy.socks_username and
 * network.proxy.socks_password.
 *
 * nsProtocolProxyService keeps them in two members and hands them to every
 * proxy it resolves in manual configuration (Resolve_Internal passes them as
 * the username and password of NewProxyInfo_Internal). This is the pref-reading
 * half: nsProtocolProxyService::PrefsChanged calls it with the pref branch, the
 * name of the pref that changed (nullptr = read everything, as at startup) and
 * the two members. A credential is read when aPref is nullptr or names it;
 * a failed read empties it; a successful read is stripped of whitespace.
 * The semantics are those of the file-static proxy_GetStringPref in
 * nsProtocolProxyService.cpp, which cannot be called from here.
 */
void StealthSocksPrefsChanged(nsIPrefBranch* aPrefBranch, const char* aPref,
                              nsCString& aUsername, nsCString& aPassword);

}  // namespace mozilla::net

#endif  // mozilla_net_StealthSocksCredentials_h
