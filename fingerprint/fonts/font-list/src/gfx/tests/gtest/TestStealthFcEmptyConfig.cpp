/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Fontconfig without the host (Linux only): the current configuration is an
// empty one, with no configuration file and no system font, and it stays so;
// the system font options are fixed (ClearType, RGB, level 100).
//
// In SOURCES, not UNIFIED_SOURCES: the system cairo's getters are declared
// here after the tree cairo's macros go, as StealthFcEmptyConfig.cpp does.

#include "gtest/gtest.h"

#include <cairo.h>
#include <fontconfig/fontconfig.h>

#include "StealthFcEmptyConfig.h"
#include "gfxPlatform.h"
#include "gfxPlatformFontList.h"
#include "mozilla/gfx/2D.h"
#include "nscore.h"

#undef cairo_font_options_destroy
#undef cairo_font_options_get_antialias
#undef cairo_font_options_get_subpixel_order

extern "C" {
NS_VISIBILITY_DEFAULT void cairo_font_options_destroy(cairo_font_options_t*);
NS_VISIBILITY_DEFAULT cairo_antialias_t
cairo_font_options_get_antialias(const cairo_font_options_t*);
NS_VISIBILITY_DEFAULT cairo_subpixel_order_t
cairo_font_options_get_subpixel_order(const cairo_font_options_t*);
}

namespace {

int SetCount(FcConfig* aConfig, FcSetName aSet) {
  FcFontSet* set = FcConfigGetFonts(aConfig, aSet);
  return set ? set->nfont : 0;
}

// Every font FcFontList finds on an empty pattern.
int ListedFonts(FcConfig* aConfig) {
  FcPattern* pattern = FcPatternCreate();
  FcObjectSet* objects = FcObjectSetBuild(FC_FILE, nullptr);
  FcFontSet* fonts = FcFontList(aConfig, pattern, objects);
  int n = fonts ? fonts->nfont : -1;
  if (fonts) {
    FcFontSetDestroy(fonts);
  }
  FcObjectSetDestroy(objects);
  FcPatternDestroy(pattern);
  return n;
}

int ConfigFiles(FcConfig* aConfig) {
  FcStrList* files = FcConfigGetConfigFiles(aConfig);
  int n = 0;
  if (files) {
    while (FcStrListNext(files)) {
      ++n;
    }
    FcStrListDone(files);
  }
  return n;
}

}  // namespace

// First on purpose: no call to StealthFcEmptyConfig here, only the hooks. The
// platform font list's constructor (and nsLookAndFeel's, before FcInit) must
// already have installed the empty configuration.
TEST(StealthFcEmptyConfig, HookedBeforeTheFontList)
{
  gfxPlatform::GetPlatform();
  ASSERT_TRUE(gfxPlatformFontList::PlatformFontList());
  FcConfig* current = FcConfigGetCurrent();
  ASSERT_TRUE(current);
  EXPECT_EQ(0, SetCount(current, FcSetSystem));
  EXPECT_EQ(0, ConfigFiles(current));
  EXPECT_EQ(0, FcConfigGetRescanInterval(current));
  EXPECT_EQ(SetCount(current, FcSetApplication), ListedFonts(current));
}

TEST(StealthFcEmptyConfig, EmptyAndCurrent)
{
  FcConfig* config = StealthFcEmptyConfig();
  ASSERT_TRUE(config);
  EXPECT_EQ(config, FcConfigGetCurrent());
  EXPECT_EQ(0, SetCount(config, FcSetSystem));
  EXPECT_EQ(0, ConfigFiles(config));
  EXPECT_EQ(SetCount(config, FcSetApplication), ListedFonts(config));
}

TEST(StealthFcEmptyConfig, IdempotentAndStaysEmpty)
{
  FcConfig* config = StealthFcEmptyConfig();
  ASSERT_TRUE(config);
  int appFonts = SetCount(config, FcSetApplication);

  EXPECT_EQ(config, StealthFcEmptyConfig());
  EXPECT_EQ(config, FcConfigGetCurrent());
  EXPECT_EQ(appFonts, SetCount(config, FcSetApplication));

  EXPECT_TRUE(FcInit());
  EXPECT_TRUE(FcInitBringUptoDate());
  EXPECT_TRUE(FcConfigUptoDate(nullptr));
  EXPECT_EQ(config, FcConfigGetCurrent());
  EXPECT_EQ(0, SetCount(FcConfigGetCurrent(), FcSetSystem));
  EXPECT_EQ(0, ConfigFiles(FcConfigGetCurrent()));
  EXPECT_EQ(appFonts, ListedFonts(FcConfigGetCurrent()));
}

TEST(StealthFcEmptyConfig, FixedFontOptions)
{
  cairo_font_options_t* options = nullptr;
  int32_t lcdFilter = -1;
  EXPECT_TRUE(StealthFixedFontOptions(options, lcdFilter));
  ASSERT_TRUE(options);
  EXPECT_EQ(CAIRO_ANTIALIAS_SUBPIXEL, cairo_font_options_get_antialias(options));
  EXPECT_EQ(CAIRO_SUBPIXEL_ORDER_RGB,
            cairo_font_options_get_subpixel_order(options));
  EXPECT_EQ(FC_LCD_DEFAULT, lcdFilter);
  EXPECT_EQ(mozilla::gfx::SubpixelOrder::RGB,
            mozilla::gfx::Factory::GetSubpixelOrder());

  // Unchanged the second time, as UpdateSystemFontOptions reports.
  cairo_font_options_t* first = options;
  EXPECT_FALSE(StealthFixedFontOptions(options, lcdFilter));
  EXPECT_EQ(first, options);
  cairo_font_options_destroy(options);
}
