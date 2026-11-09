/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthFcEmptyConfig.h"
#include "StealthIdwrTextVars.h"  // STEALTH-HOOK(fingerprint/fonts/idwr)

#include <cairo.h>
#include <fontconfig/fontconfig.h>

#include "mozilla/Assertions.h"
#include "nscore.h"

// mSystemFontOptions belongs to the SYSTEM cairo (GTK's), as in
// gfxFcPlatformFontList.cpp: the tree cairo's symbols have other names, so
// their macros go and the system functions are declared here, as that file
// does. This file is in SOURCES, not UNIFIED_SOURCES, for that reason.
#undef cairo_font_options_create
#undef cairo_font_options_destroy
#undef cairo_font_options_equal
#undef cairo_font_options_set_antialias
#undef cairo_font_options_set_subpixel_order

extern "C" {
NS_VISIBILITY_DEFAULT cairo_font_options_t* cairo_font_options_create();
NS_VISIBILITY_DEFAULT void cairo_font_options_destroy(cairo_font_options_t*);
NS_VISIBILITY_DEFAULT cairo_bool_t cairo_font_options_equal(
    const cairo_font_options_t*, const cairo_font_options_t*);
NS_VISIBILITY_DEFAULT void cairo_font_options_set_antialias(
    cairo_font_options_t*, cairo_antialias_t);
NS_VISIBILITY_DEFAULT void cairo_font_options_set_subpixel_order(
    cairo_font_options_t*, cairo_subpixel_order_t);
}

static FcConfig* CreateEmptyConfig() {
  // FcConfigCreate parses nothing: no fonts.conf, no FONTCONFIG_FILE, no
  // directory. Everything the host would put in is simply never asked for.
  //
  // No way back to the host: if fontconfig cannot give us an empty
  // configuration, the next FcInit() would load the system one and the
  // browser would run on the host's fonts. The process stops instead.
  FcConfig* config = FcConfigCreate();
  MOZ_RELEASE_ASSERT(config, "fontconfig: no empty configuration");
  // A configuration starts with a 30 s rescan interval. With 0,
  // FcInitBringUptoDate returns before FcInitReinitialize (which would load
  // the system configuration), and gfxFcPlatformFontList starts no timer.
  MOZ_RELEASE_ASSERT(FcConfigSetRescanInterval(config, 0),
                     "fontconfig: rescan interval not set");
  MOZ_RELEASE_ASSERT(FcConfigSetCurrent(config),
                     "fontconfig: empty configuration not installed");
  // The reference FcConfigCreate gave stays ours: the configuration lives as
  // long as the process, whatever FcConfigSetCurrent does with its count.
  return config;
}

FcConfig* StealthFcEmptyConfig() {
  // Initialised once (thread-safe static): later calls must not replace the
  // configuration, or the fonts already added to it would go.
  static FcConfig* const sConfig = CreateEmptyConfig();
  return sConfig;
}

bool StealthFixedFontOptions(cairo_font_options_t*& aOptions,
                             int32_t& aLcdFilter) {
  // ClearType on, the declared pixel structure's stripes (RGB unless the
  // declaration says BGR): the antialias and subpixel order cairo hands
  // to fontconfig (cairo_ft_font_options_substitute sets FC_ANTIALIAS and
  // FC_RGBA only where the pattern has none). The hint style stays
  // CAIRO_HINT_STYLE_DEFAULT, so the declared one (StealthDeclareTextOptions)
  // is the only one.
  const bool bgr = mozilla::gfx::StealthIdwrPixelGeometryIsBGR();
  cairo_font_options_t* fixed = cairo_font_options_create();
  cairo_font_options_set_antialias(fixed, CAIRO_ANTIALIAS_SUBPIXEL);
  cairo_font_options_set_subpixel_order(
      fixed, bgr ? CAIRO_SUBPIXEL_ORDER_BGR : CAIRO_SUBPIXEL_ORDER_RGB);

  // ClearType level 100 is the full-strength filter: FreeType's default LCD
  // filter (FC_LCD_DEFAULT).
  const int32_t lcdFilter = FC_LCD_DEFAULT;

  // The order the surfaces draw LCD text in is the declared one as well: this runs
  // when the font list is made, after the constructor of the platform, and at every
  // UpdateSystemFontOptions.
  mozilla::gfx::StealthIdwrApplyPixelGeometry();

  if (aOptions && cairo_font_options_equal(aOptions, fixed) &&
      aLcdFilter == lcdFilter) {
    cairo_font_options_destroy(fixed);
    return false;
  }
  if (aOptions) {
    cairo_font_options_destroy(aOptions);
  }
  aOptions = fixed;
  aLcdFilter = lcdFilter;
  return true;
}
