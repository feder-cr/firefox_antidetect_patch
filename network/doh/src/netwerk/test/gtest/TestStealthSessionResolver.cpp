/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// A lookup is answered by DoH or by the host's resolver according to the user
// context it belongs to, with the global mode left at 0. The tables are those
// of the two pure functions: which contexts a value of
// network.trr.stealth.contexts marks as proxied, and which flags a lookup enters
// the host resolver with. The suite is TestStealthSessionResolver, which the
// candidate's gtest filter runs.

#include "StealthSessionResolver.h"
#include "gtest/gtest.h"
#include "nsIDNSService.h"
#include "nsIRequest.h"
#include "nsString.h"

using namespace mozilla::net;

namespace {

nsIDNSService::DNSFlags WithMode(nsIRequest::TRRMode aMode,
                                 nsIDNSService::DNSFlags aOther =
                                     nsIDNSService::RESOLVE_DEFAULT_FLAGS) {
  return static_cast<nsIDNSService::DNSFlags>(
      nsIDNSService::GetFlagsFromTRRMode(aMode) | aOther);
}

// The ids a list marks as proxied, among the ones the test asks about.
const uint32_t kIds[] = {0, 1, 6, 7, 60, 4294967295u};

struct ListCase {
  const char* mList;
  // Which of kIds are proxied, in the order of kIds.
  bool mProxied[sizeof(kIds) / sizeof(kIds[0])];
};

}  // namespace

TEST(TestStealthSessionResolver, ListParsing)
{
  const ListCase cases[] = {
      // Nothing declared: no proxy anywhere.
      {"", {false, false, false, false, false, false}},
      {" ", {false, false, false, false, false, false}},
      // The browser-wide proxy: every context without a proxy of its own.
      {"*", {true, true, true, true, true, true}},
      {" * ", {true, true, true, true, true, true}},
      {"*,6", {true, true, true, true, true, true}},
      {"6,*", {true, true, true, true, true, true}},
      // Contexts with their own proxy: exactly those ids.
      {"6", {false, false, true, false, false, false}},
      {"0", {true, false, false, false, false, false}},
      {"6,7", {false, false, true, true, false, false}},
      {" 6 , 7", {false, false, true, true, false, false}},
      {"\t6,\n7 ", {false, false, true, true, false, false}},
      {",,6,", {false, false, true, false, false, false}},
      {"4294967295", {false, false, false, false, false, true}},
      // Garbage is ignored and does not hide the ids around it.
      {"x,6", {false, false, true, false, false, false}},
      {"6,x,7", {false, false, true, true, false, false}},
      {"x", {false, false, false, false, false, false}},
      {"**", {false, false, false, false, false, false}},
      {"*6", {false, false, false, false, false, false}},
      {"6 7", {false, false, false, false, false, false}},
      {"+6", {false, false, false, false, false, false}},
      {"-6", {false, false, false, false, false, false}},
      {"6.0", {false, false, false, false, false, false}},
      {"0x6", {false, false, false, false, false, false}},
      // Leading zeros are not an id: "06" is neither 6 nor 0.
      {"06", {false, false, false, false, false, false}},
      {"00", {false, false, false, false, false, false}},
      {"007", {false, false, false, false, false, false}},
      // An id must fit 32 bits.
      {"4294967296", {false, false, false, false, false, false}},
      {"99999999999", {false, false, false, false, false, false}},
  };
  for (const ListCase& c : cases) {
    for (size_t i = 0; i < sizeof(kIds) / sizeof(kIds[0]); i++) {
      EXPECT_EQ(StealthContextHasProxy(kIds[i], nsDependentCString(c.mList)),
                c.mProxied[i])
          << "list [" << c.mList << "] context " << kIds[i];
    }
  }
}

TEST(TestStealthSessionResolver, ContextAgainstList)
{
  // The default context has the id 0 and is proxied by "*" or by "0" only.
  EXPECT_TRUE(StealthContextHasProxy(0, "*"_ns));
  EXPECT_TRUE(StealthContextHasProxy(0, "0"_ns));
  EXPECT_FALSE(StealthContextHasProxy(0, "6"_ns));
  EXPECT_FALSE(StealthContextHasProxy(0, ""_ns));
  // A context without a proxy of its own, next to one that has: the list names
  // the second only.
  EXPECT_TRUE(StealthContextHasProxy(6, "6"_ns));
  EXPECT_FALSE(StealthContextHasProxy(7, "6"_ns));
  EXPECT_FALSE(StealthContextHasProxy(0, "6"_ns));
  // With the browser-wide proxy every context is proxied, one with its own
  // proxy included.
  EXPECT_TRUE(StealthContextHasProxy(7, "*,6"_ns));
  EXPECT_TRUE(StealthContextHasProxy(6, "*,6"_ns));
}

TEST(TestStealthSessionResolver, ProxiedLookupIsDoHOnly)
{
  // Whatever mode the caller asked for, a proxied lookup is TRR_ONLY: the mask
  // value, both mode bits set.
  EXPECT_EQ(nsIDNSService::GetFlagsFromTRRMode(nsIRequest::TRR_ONLY_MODE),
            nsIDNSService::RESOLVE_TRR_MODE_MASK);
  for (auto request :
       {nsIRequest::TRR_DEFAULT_MODE, nsIRequest::TRR_DISABLED_MODE,
        nsIRequest::TRR_FIRST_MODE, nsIRequest::TRR_ONLY_MODE}) {
    EXPECT_EQ(StealthResolverFlags(WithMode(request), true, false),
              WithMode(nsIRequest::TRR_ONLY_MODE))
        << "request " << request;
  }
}

TEST(TestStealthSessionResolver, UnproxiedLookupIsNative)
{
  // The services upstream keeps on the host's resolver ask for "TRR disabled"
  // and keep it; a lookup without a mode and one that asks for DoH or for
  // TRR-first become native too, so a context without a proxy never reaches
  // DoH, whatever the other contexts do.
  for (auto request :
       {nsIRequest::TRR_DEFAULT_MODE, nsIRequest::TRR_DISABLED_MODE,
        nsIRequest::TRR_FIRST_MODE, nsIRequest::TRR_ONLY_MODE}) {
    EXPECT_EQ(StealthResolverFlags(WithMode(request), false, false),
              WithMode(nsIRequest::TRR_DISABLED_MODE))
        << "request " << request;
  }
  EXPECT_EQ(WithMode(nsIRequest::TRR_DISABLED_MODE),
            nsIDNSService::RESOLVE_TRR_DISABLED_MODE);
}

TEST(TestStealthSessionResolver, OtherFlagsStay)
{
  // RESOLVE_DISABLE_TRR is another bit: the main-thread synchronous resolve and
  // the retries that skip a TRR-only attempt keep their meaning. So do the
  // cache, priority and address-family bits.
  const nsIDNSService::DNSFlags other = static_cast<nsIDNSService::DNSFlags>(
      nsIDNSService::RESOLVE_DISABLE_TRR | nsIDNSService::RESOLVE_DISABLE_IPV6 |
      nsIDNSService::RESOLVE_BYPASS_CACHE |
      nsIDNSService::RESOLVE_PRIORITY_LOW |
      nsIDNSService::RESOLVE_REFRESH_NEGATIVE_CACHE);
  for (bool proxied : {true, false}) {
    const nsIRequest::TRRMode expected =
        proxied ? nsIRequest::TRR_ONLY_MODE : nsIRequest::TRR_DISABLED_MODE;
    for (auto request :
         {nsIRequest::TRR_DEFAULT_MODE, nsIRequest::TRR_DISABLED_MODE,
          nsIRequest::TRR_FIRST_MODE, nsIRequest::TRR_ONLY_MODE}) {
      EXPECT_EQ(StealthResolverFlags(WithMode(request, other), proxied, false),
                WithMode(expected, other))
          << "proxied " << proxied << " request " << request;
    }
  }
}

TEST(TestStealthSessionResolver, TheResultIsStableUnderASecondCall)
{
  // A lookup and its cancellation both go through the function with the flags
  // the caller gave; applying it to its own result changes nothing.
  const nsIDNSService::DNSFlags other = static_cast<nsIDNSService::DNSFlags>(
      nsIDNSService::RESOLVE_BYPASS_CACHE | nsIDNSService::RESOLVE_SPECULATE);
  for (bool proxied : {true, false}) {
    const nsIDNSService::DNSFlags once =
        StealthResolverFlags(WithMode(nsIRequest::TRR_FIRST_MODE, other),
                             proxied, false);
    EXPECT_EQ(StealthResolverFlags(once, proxied, false), once);
  }
}

TEST(TestStealthSessionResolver, TheDoHServerKeepsItsRequest)
{
  // The DoH request is a request for "TRR disabled" on purpose: resolving the
  // server's own name through DoH would wait for itself. Proxied or not.
  const nsIDNSService::DNSFlags flags =
      WithMode(nsIRequest::TRR_DISABLED_MODE,
               nsIDNSService::RESOLVE_REFRESH_NEGATIVE_CACHE);
  EXPECT_EQ(StealthResolverFlags(flags, true, true), flags);
  EXPECT_EQ(StealthResolverFlags(flags, false, true), flags);
}
