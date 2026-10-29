/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "../../base/StealthSocksCredentials.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/SpinEventLoopUntil.h"
#include "nsCOMPtr.h"
#include "nsContentUtils.h"
#include "nsIChannel.h"
#include "nsIContentPolicy.h"
#include "nsILoadInfo.h"
#include "nsIPrefBranch.h"
#include "nsIProtocolProxyCallback.h"
#include "nsIProtocolProxyService.h"
#include "nsIProxyInfo.h"
#include "nsNetUtil.h"
#include "nsServiceManagerUtils.h"
#include "nsString.h"

namespace mozilla::net {

namespace {

constexpr char kUser[] = "network.proxy.socks_username";
constexpr char kPass[] = "network.proxy.socks_password";

// Puts every proxy pref the tests touch back to its default.
class ProxyPrefsReset {
 public:
  ~ProxyPrefsReset() {
    for (const char* pref :
         {"network.proxy.type", "network.proxy.socks",
          "network.proxy.socks_port", "network.proxy.socks_version",
          "network.proxy.http", "network.proxy.http_port", kUser, kPass}) {
      Preferences::ClearUser(pref);
    }
  }
};

class CapturingCallback final : public nsIProtocolProxyCallback {
 public:
  NS_DECL_ISUPPORTS
  NS_IMETHOD OnProxyAvailable(nsICancelable*, nsIChannel*, nsIProxyInfo* aInfo,
                              nsresult aStatus) override {
    mDone = true;
    mStatus = aStatus;
    mInfo = aInfo;
    return NS_OK;
  }
  bool mDone = false;
  nsresult mStatus = NS_OK;
  nsCOMPtr<nsIProxyInfo> mInfo;

 private:
  ~CapturingCallback() = default;
};
NS_IMPL_ISUPPORTS(CapturingCallback, nsIProtocolProxyCallback)

}  // namespace

// The helper alone: guards, whitespace, which member a pref name reaches.
TEST(TestStealthSocksCredentials, PrefsChanged)
{
  ProxyPrefsReset reset;
  nsCOMPtr<nsIPrefBranch> branch = do_GetService(NS_PREFSERVICE_CONTRACTID);
  ASSERT_TRUE(branch);
  ASSERT_EQ(Preferences::SetCString(kUser, " us er\t"), NS_OK);
  ASSERT_EQ(Preferences::SetCString(kPass, "p a\nss "), NS_OK);

  nsCString user("keep-user"), pass("keep-pass");

  // A pref that is neither of ours: nothing is read.
  StealthSocksPrefsChanged(branch, "network.proxy.socks", user, pass);
  EXPECT_TRUE(user.EqualsLiteral("keep-user"));
  EXPECT_TRUE(pass.EqualsLiteral("keep-pass"));

  // Only the username changed: only the username is read, whitespace removed.
  StealthSocksPrefsChanged(branch, kUser, user, pass);
  EXPECT_TRUE(user.EqualsLiteral("user"));
  EXPECT_TRUE(pass.EqualsLiteral("keep-pass"));

  // Only the password changed.
  user.AssignLiteral("keep-user");
  StealthSocksPrefsChanged(branch, kPass, user, pass);
  EXPECT_TRUE(user.EqualsLiteral("keep-user"));
  EXPECT_TRUE(pass.EqualsLiteral("pass"));

  // nullptr (startup): both are read.
  user.AssignLiteral("x");
  pass.AssignLiteral("y");
  StealthSocksPrefsChanged(branch, nullptr, user, pass);
  EXPECT_TRUE(user.EqualsLiteral("user"));
  EXPECT_TRUE(pass.EqualsLiteral("pass"));

  // Back to the default (empty string): the members are emptied.
  Preferences::ClearUser(kUser);
  Preferences::ClearUser(kPass);
  StealthSocksPrefsChanged(branch, nullptr, user, pass);
  EXPECT_TRUE(user.IsEmpty());
  EXPECT_TRUE(pass.IsEmpty());
}

// End to end through the public service: the pref observer feeds the members
// and the resolved proxy info carries them.
TEST(TestStealthSocksCredentials, ReachResolvedProxyInfo)
{
  ProxyPrefsReset reset;
  nsCOMPtr<nsIProtocolProxyService> ps =
      do_GetService(NS_PROTOCOLPROXYSERVICE_CONTRACTID);
  ASSERT_TRUE(ps);

  auto Resolve = [&](const char* aSpec) -> nsCOMPtr<nsIProxyInfo> {
    nsCOMPtr<nsIURI> uri;
    EXPECT_EQ(NS_NewURI(getter_AddRefs(uri), aSpec), NS_OK);
    nsCOMPtr<nsIChannel> channel;
    EXPECT_EQ(
        NS_NewChannel(getter_AddRefs(channel), uri,
                      nsContentUtils::GetSystemPrincipal(),
                      nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_SEC_CONTEXT_IS_NULL,
                      nsIContentPolicy::TYPE_OTHER),
        NS_OK);
    RefPtr<CapturingCallback> cb = new CapturingCallback();
    nsCOMPtr<nsICancelable> cancel;
    EXPECT_EQ(ps->AsyncResolve(channel, 0, cb, nullptr, getter_AddRefs(cancel)),
              NS_OK);
    EXPECT_TRUE(SpinEventLoopUntil("TestStealthSocksCredentials"_ns,
                                   [&] { return cb->mDone; }));
    return cb->mInfo;
  };

  ASSERT_EQ(Preferences::SetInt("network.proxy.type", 1), NS_OK);
  ASSERT_EQ(Preferences::SetCString("network.proxy.socks", "127.0.0.1"), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.socks_port", 1080), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.socks_version", 5), NS_OK);
  ASSERT_EQ(Preferences::SetCString(kUser, "alice"), NS_OK);
  ASSERT_EQ(Preferences::SetCString(kPass, " s3 cret "), NS_OK);

  nsCOMPtr<nsIProxyInfo> info = Resolve("http://example.com/");
  ASSERT_TRUE(info);
  nsAutoCString type, user, pass;
  info->GetType(type);
  info->GetUsername(user);
  info->GetPassword(pass);
  EXPECT_TRUE(type.EqualsLiteral("socks"));
  EXPECT_TRUE(user.EqualsLiteral("alice"));
  EXPECT_TRUE(pass.EqualsLiteral("s3cret"));

  // The single NewProxyInfo_Internal call in Resolve_Internal serves the HTTP
  // branch too: with no SOCKS proxy the stored credentials ride on the http
  // proxy info.
  ASSERT_EQ(Preferences::SetCString("network.proxy.socks", ""), NS_OK);
  ASSERT_EQ(Preferences::SetCString("network.proxy.http", "127.0.0.1"), NS_OK);
  ASSERT_EQ(Preferences::SetInt("network.proxy.http_port", 8080), NS_OK);
  info = Resolve("http://example.com/");
  ASSERT_TRUE(info);
  info->GetType(type);
  info->GetUsername(user);
  EXPECT_TRUE(type.EqualsLiteral("http"));
  EXPECT_TRUE(user.EqualsLiteral("alice"));
}

}  // namespace mozilla::net
