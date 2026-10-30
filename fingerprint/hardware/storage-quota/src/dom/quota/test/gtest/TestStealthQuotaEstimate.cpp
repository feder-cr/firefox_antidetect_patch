/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The limit navigator.storage.estimate() reports (dom/quota/
// StealthQuotaEstimate.h). Since 159 EstimateOp::GetResponse answers two
// scopes: the origin scope, which a page reads, and the group scope
// (estimateGroupUsage, parent process only, the ServiceWorker quota
// mitigation), which a page never sees. Only the first may be rewritten; the
// second keeps Mozilla's limit so the mitigation decides as in retail.
//
// StealthEstimateLimit(aLimit) used to take no scope and rewrote both; the
// tests below document the new two-argument contract (they do not compile
// against the old header, and the group-scope case is the one it got wrong).

#include <stdint.h>

#include "StealthQuotaEstimate.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"

using namespace mozilla;
using mozilla::dom::quota::StealthEstimateLimit;

namespace {

constexpr char kQuotaPref[] = "zoom.stealth.storage.quota_mb";

// A limit Mozilla would compute, deliberately unlike any declared quota.
constexpr uint64_t kMozillaLimit = 123456789;

// Sets the pref, puts it back at the end of the test.
class QuotaEstimatePref {
 public:
  explicit QuotaEstimatePref(int32_t aMb)
      : mOld(Preferences::GetInt(kQuotaPref, -1)) {
    Preferences::SetInt(kQuotaPref, aMb);
  }
  ~QuotaEstimatePref() { Preferences::SetInt(kQuotaPref, mOld); }

 private:
  int32_t mOld;
};

}  // namespace

TEST(StealthQuotaEstimate, OriginScopeReportsThePrefInBytes)
{
  QuotaEstimatePref pref(2048);
  EXPECT_EQ(StealthEstimateLimit(kMozillaLimit, /* aOriginScope */ true),
            uint64_t(2048) * 1024 * 1024);
}

TEST(StealthQuotaEstimate, GroupScopeKeepsMozillasLimit)
{
  QuotaEstimatePref pref(2048);
  EXPECT_EQ(StealthEstimateLimit(kMozillaLimit, /* aOriginScope */ false),
            kMozillaLimit);
}

TEST(StealthQuotaEstimate, UnsetPrefChangesNeitherScope)
{
  QuotaEstimatePref pref(-1);
  EXPECT_EQ(StealthEstimateLimit(kMozillaLimit, true), kMozillaLimit);
  EXPECT_EQ(StealthEstimateLimit(kMozillaLimit, false), kMozillaLimit);
}
