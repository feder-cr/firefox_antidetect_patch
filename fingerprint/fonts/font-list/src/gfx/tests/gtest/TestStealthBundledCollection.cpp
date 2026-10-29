/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Under bundle-only, a content process does not build the platform's
// collection of the bundled font files: it never reads it, and building it
// opened all 121 files twice in every content process at every launch
// (StealthBundleFontList::NeedsPlatformBundledCollection).

#include "gtest/gtest.h"

#include "StealthBundleFontList.h"

TEST(StealthBundledCollection, ContentProcessUnderBundleOnlyBuildsNone)
{
  EXPECT_FALSE(StealthBundleFontList::NeedsPlatformBundledCollection(
      /* aBundleOnly */ true, /* aIsParentProcess */ false));
}

TEST(StealthBundledCollection, ParentKeepsItForTheFontInfoLoader)
{
  EXPECT_TRUE(StealthBundleFontList::NeedsPlatformBundledCollection(true, true));
}

TEST(StealthBundledCollection, WithoutBundleOnlyEveryProcessBuildsItAsUpstream)
{
  EXPECT_TRUE(StealthBundleFontList::NeedsPlatformBundledCollection(false, false));
  EXPECT_TRUE(StealthBundleFontList::NeedsPlatformBundledCollection(false, true));
}
