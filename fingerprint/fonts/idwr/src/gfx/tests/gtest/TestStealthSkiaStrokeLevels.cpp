/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The coverage of a glyph is what Skia and idwr compute, with nothing after them.
//
// Until now every A8 glyph mask was snapped, in SkScalerContext::getImage, to the 17
// levels a launcher declared in zoom.stealth.text.coverage_ladder. The snap copied the
// effect of ClearType's gamma and contrast from the time the text was FreeType's; the
// masks now come from idwr, which applies the declared gamma and contrast itself, and the
// snap was wrong wherever the mask is not an idwr mask: the stroke of a glyph (strokeText)
// is Skia's own path rasteriser, with the 256 levels of a retail Firefox on Windows.
//
// This test draws the stroke of six glyphs of the bundled Arial, 16 px, one device pixel
// wide, with the engine on and the ladder of the old core still declared, and counts the
// distinct values of the A8 target (the mask itself: white on zero, source over). With the
// snap they were 17 at most; without it the edges of a stroke take dozens of levels.
//
// Before the removal this fails. The count of levels needs the real engine: the build of
// the series runs it, it is not checked outside it.

#include <stdint.h>

#include <vector>

#include "gtest/gtest.h"

#include "StealthTestFonts.h"
#include "gfxPlatform.h"
#include "mozilla/Preferences.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/webrender/WebRenderTypes.h"
#include "nsTArray.h"
#ifdef XP_WIN
#  include "gfxDWriteFonts.h"
#else
#  include "StealthIdwrTextVars.h"
#endif

using namespace mozilla;
using namespace mozilla::gfx;

namespace {

// The ladder an old core declares: the 17 levels the snap used.
const char kOldLadder[] = "0,18,35,53,70,87,104,121,138,153,169,185,200,215,230,243,255";

const char* const kPrefs[] = {
    "zoom.stealth.fpp.hw_seed",
    "zoom.stealth.text.system_quality",
    "zoom.stealth.text.coverage_ladder",
    "gfx.font_rendering.cleartype_params.gamma",
    "gfx.font_rendering.cleartype_params.enhanced_contrast",
    "gfx.font_rendering.cleartype_params.cleartype_level",
    "gfx.font_rendering.cleartype_params.rendering_mode",
};

void UpdateTextVars() {
#ifdef XP_WIN
  gfxDWriteFont::UpdateSystemTextVars();
#else
  StealthIdwrUpdateTextVars();
#endif
}

// Back to stock Firefox, and the text vars recomputed, whatever the test did.
struct RestoreStock {
  ~RestoreStock() {
    for (const char* name : kPrefs) {
      Preferences::ClearUser(name);
    }
    UpdateTextVars();
  }
};

constexpr int kWidth = 160;
constexpr int kHeight = 40;

// Arial's glyph ids of "SaOge0" (id = code point - 29 in the bundled file).
const uint16_t kGlyphs[] = {54, 68, 50, 74, 72, 19};

}  // namespace

TEST(StealthSkiaStrokeLevels, StrokeOfAGlyphHasMoreLevelsThanTheOldLadder)
{
  gfxPlatform::GetPlatform();
  RestoreStock restore;
  Preferences::SetInt("zoom.stealth.text.system_quality", 5);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.gamma", 1800);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.enhanced_contrast", 100);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.cleartype_level", 100);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.rendering_mode", 0);
  // A launcher with the old core still declares the ladder: it is ignored now.
  Preferences::SetCString("zoom.stealth.text.coverage_ladder", kOldLadder);
  // The engine on, last: a change of the other prefs while it is on updates the text vars,
  // and a declaration without the rendering mode is refused.
  Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
  UpdateTextVars();

#ifdef XP_WIN
  const FontType fontType = FontType::DWRITE;
#else
  const FontType fontType = FontType::FONTCONFIG;
#endif

  nsTArray<uint8_t> bytes = stealth_test_fonts::ReadBundledFont("arial7_04.ttf");
  ASSERT_FALSE(bytes.IsEmpty()) << "bundled font missing: arial7_04.ttf";
  RefPtr<NativeFontResource> resource =
      Factory::CreateNativeFontResource(bytes.Elements(), bytes.Length(), fontType);
  ASSERT_TRUE(resource);
  RefPtr<UnscaledFont> unscaled = resource->CreateUnscaledFont(0, nullptr, 0);
  ASSERT_TRUE(unscaled);
  wr::FontInstanceOptions options = {};
  wr::FontInstancePlatformOptions platform = {};
  RefPtr<ScaledFont> font =
      unscaled->CreateScaledFontFromWRFont(16.0f, &options, &platform, nullptr, 0);
  ASSERT_TRUE(font);

  RefPtr<DrawTarget> dt = Factory::CreateDrawTarget(
      BackendType::SKIA, IntSize(kWidth, kHeight), SurfaceFormat::A8);
  ASSERT_TRUE(dt);
  dt->ClearRect(Rect(0, 0, kWidth, kHeight));

  // One glyph per call, each at its own subpixel position, so that the edges fall on the
  // pixel grid in six different ways.
  std::vector<Glyph> glyphs;
  for (size_t i = 0; i < sizeof(kGlyphs) / sizeof(kGlyphs[0]); ++i) {
    Glyph glyph;
    glyph.mIndex = kGlyphs[i];
    glyph.mPosition = Point(8.0f + 22.0f * float(i) + 0.17f * float(i), 28.0f);
    glyphs.push_back(glyph);
  }
  GlyphBuffer buffer = {glyphs.data(), uint32_t(glyphs.size())};
  dt->StrokeGlyphs(font, buffer, ColorPattern(DeviceColor(1, 1, 1, 1)),
                   StrokeOptions(1.0f),
                   DrawOptions(1.0f, CompositionOp::OP_OVER, AntialiasMode::GRAY));

  RefPtr<SourceSurface> snapshot = dt->Snapshot();
  ASSERT_TRUE(snapshot);
  RefPtr<DataSourceSurface> data = snapshot->GetDataSurface();
  ASSERT_TRUE(data);
  DataSourceSurface::ScopedMap map(data, DataSourceSurface::READ);
  ASSERT_TRUE(map.IsMapped());

  bool seen[256] = {};
  int levels = 0;
  int inked = 0;
  for (int y = 0; y < kHeight; ++y) {
    const uint8_t* row = map.GetData() + size_t(y) * map.GetStride();
    for (int x = 0; x < kWidth; ++x) {
      inked += row[x] != 0;
      if (!seen[row[x]]) {
        seen[row[x]] = true;
        ++levels;
      }
    }
  }
  // The test is not vacuous: the strokes are drawn.
  ASSERT_GT(inked, 100) << "the stroke drew no ink";
  EXPECT_GT(levels, 40) << "the A8 mask of a stroke has " << levels
                        << " distinct values: still the 17 levels of the old ladder?";
}
