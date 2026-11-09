/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The average character width on the FreeType path with the stealth engine on
// is the one gfxDWriteFont::ComputeMetrics computes
// (StealthDWriteAveCharWidth, gfx/thebes/StealthDesignMetrics.h). FreeType
// rounds it to whole pixels and Windows does not, so a default textarea
// (monospace, Consolas, 13px) at DPR 1.5 was 146.667px wide on Linux against
// 143px on Windows.
//
// A real font: the bundled Consolas (1126 xAvgCharWidth and 'x' advance on a
// 2048 em) at 19.5 device px, which is 13px at DPR 1.5, loaded as a data font
// so every case gets a fresh entry and metrics computed under its own prefs.

#include "gtest/gtest.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <utility>

#include <fontconfig/fontconfig.h>

#include "FontData.h"
#include "StealthSyntheticBold.h"
#include "gfxFont.h"
#include "gfxFontEntry.h"
#include "gfxPlatform.h"
#include "gfxPlatformFontList.h"
#include "mozilla/FontPropertyTypes.h"
#include "mozilla/Preferences.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/StealthMultistrikeBold.h"
#include "mozilla/webrender/WebRenderTypes.h"
#include "nsCOMPtr.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsFontMetrics.h"
#include "nsIFile.h"
#include "nsIInputStream.h"
#include "nsMathUtils.h"
#include "nsNetUtil.h"
#include "nsStreamUtils.h"
#include "nsString.h"
#include "nsThreadUtils.h"

namespace {

const char* const kStealthFontMetricsPrefs[] = {
    "zoom.stealth.fpp.hw_seed",
    "zoom.stealth.text.system_quality",
    "gfx.font_rendering.cleartype_params.rendering_mode",
};

// Back to stock Firefox (nothing declared), whatever the test did.
struct RestoreStealthFontMetricsStock {
  ~RestoreStealthFontMetricsStock() {
    for (const char* name : kStealthFontMetricsPrefs) {
      mozilla::Preferences::ClearUser(name);
    }
  }
};

// The default textarea font at DPR 1.5: 13px * 1.5.
constexpr double kStealthConsolasSize = 19.5;
// 60 app units per CSS px / 1.5.
constexpr int32_t kStealthAppUnitsPerDevPixel = 40;

// The bundled file, as the product ships it in <GRE>/fonts. Its absence is a
// real failure (the font is part of the product), so the caller asserts; the
// places looked at go to aTried for the message.
RefPtr<FontData> StealthLoadBundledConsolas(nsCString& aTried) {
  for (const char* key : {NS_GRE_DIR, NS_XPCOM_CURRENT_PROCESS_DIR}) {
    nsCOMPtr<nsIFile> dir;
    if (NS_FAILED(NS_GetSpecialDirectory(key, getter_AddRefs(dir)))) {
      aTried.AppendPrintf(" [%s: no such directory]", key);
      continue;
    }
    // The directory itself, then its parent (a gtest libxul lives one level
    // below the application).
    for (int level = 0; level < 2 && dir; ++level) {
      nsCOMPtr<nsIFile> file;
      if (NS_FAILED(dir->Clone(getter_AddRefs(file))) ||
          NS_FAILED(file->Append(u"fonts"_ns)) ||
          NS_FAILED(file->Append(u"consola7_00.ttf"_ns))) {
        break;
      }
      nsAutoCString path;
      file->GetNativePath(path);
      aTried.AppendPrintf(" [%s]", path.get());
      nsCString bytes;
      nsCOMPtr<nsIInputStream> stream;
      if (NS_SUCCEEDED(
              NS_NewLocalFileInputStream(getter_AddRefs(stream), file))) {
        const nsresult rv = NS_ConsumeStream(stream, UINT32_MAX, bytes);
        stream->Close();
        if (NS_SUCCEEDED(rv) && !bytes.IsEmpty()) {
          // FontData owns a malloc'd buffer and frees it.
          auto* buffer = static_cast<uint8_t*>(malloc(bytes.Length()));
          if (!buffer) {
            return nullptr;
          }
          memcpy(buffer, bytes.get(), bytes.Length());
          const uint8_t* owned = buffer;
          return new FontData(std::move(owned), bytes.Length());
        }
      }
      nsCOMPtr<nsIFile> parent;
      dir->GetParent(getter_AddRefs(parent));
      dir = parent;
    }
  }
  return nullptr;
}

// A NEW entry on aData (main thread), so the font made from it next has its
// metrics computed under the prefs set now.
already_AddRefed<gfxFontEntry> StealthMakeConsolasEntry(FontData* aData) {
  gfxPlatformFontList* pfl = gfxPlatformFontList::PlatformFontList();
  if (!pfl) {
    return nullptr;
  }
  return pfl->MakePlatformFont(
      "StealthFontMetricsConsolas"_ns,
      mozilla::WeightRange(mozilla::FontWeight::NORMAL),
      mozilla::WidthRange(mozilla::FontWidth::NORMAL),
      mozilla::SlantStyleRange(mozilla::FontSlantStyle::NORMAL), aData);
}

// gfxFont::Metrics::aveCharWidth of the font at 19.5px on aEntry: making the
// font is what runs gfxFT2FontBase::InitMetrics, on the calling thread. -1 if
// no font could be made.
gfxFloat StealthMeasureConsolasAve(gfxFontEntry* aEntry) {
  if (!aEntry) {
    return -1.0;
  }
  gfxFontStyle style;
  style.size = kStealthConsolasSize;
  RefPtr<gfxFont> font = aEntry->FindOrMakeFont(&style);
  if (!font || !font->Valid()) {
    return -1.0;
  }
  return font->GetMetrics(nsFontMetrics::eHorizontal).aveCharWidth;
}

gfxFloat StealthConsolasAveCharWidth(FontData* aData) {
  RefPtr<gfxFontEntry> fe = StealthMakeConsolasEntry(aData);
  return StealthMeasureConsolasAve(fe);
}

// nsFontMetrics::AveCharWidth at 40 app units per device px: its own
// expression, CEIL_TO_TWIPS(aveCharWidth) (gfx/src/nsFontMetrics.cpp). A real
// nsFontMetrics needs a pres context, which a gfx gtest does not have.
nscoord StealthNsFontMetricsAveCharWidth(gfxFloat aAveCharWidth) {
  return nscoord(ceil(aAveCharWidth * kStealthAppUnitsPerDevPixel));
}

class StealthFontMetrics : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    // The platform and its font list, built with the engine off.
    gfxPlatform::GetPlatform();
  }
  void SetUp() override {
    nsCString tried;
    mData = StealthLoadBundledConsolas(tried);
    ASSERT_TRUE(mData) << "the bundled consola7_00.ttf could not be read; looked"
                       << " at:" << tried.get();
  }
  void DeclareEngine(int32_t aQuality, int32_t aRenderingMode) {
    mozilla::Preferences::SetInt("zoom.stealth.text.system_quality", aQuality);
    mozilla::Preferences::SetInt(
        "gfx.font_rendering.cleartype_params.rendering_mode", aRenderingMode);
    // The engine on, last: with it on a change of the text prefs updates the text vars, and a
    // declaration without the rendering mode is refused.
    mozilla::Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
  }
  RestoreStealthFontMetricsStock mRestore;
  RefPtr<FontData> mData;
};

}  // namespace

// ClearType (5) and a natural rendering mode: Windows measures in design space
// and takes int16(xAvgCharWidth) * float(size / upem), unrounded.
TEST_F(StealthFontMetrics, ClearTypeTakesTheOS2AverageUnrounded)
{
  // DWRITE_RENDERING_MODE_DEFAULT (0) and NATURAL_SYMMETRIC (5) both measure
  // natural.
  for (int32_t mode : {0, 5}) {
    DeclareEngine(5, mode);
    const gfxFloat ave = StealthConsolasAveCharWidth(mData);
    EXPECT_EQ(double(1126 * float(kStealthConsolasSize / 2048)), ave) << mode;
    EXPECT_EQ(429, StealthNsFontMetricsAveCharWidth(ave)) << mode;
  }
}

// Standard antialiasing (4): not ClearType, so Windows leaves design space and
// takes the 'x' advance, rounded to a whole pixel.
TEST_F(StealthFontMetrics, OutsideClearTypeTakesTheRoundedXAdvance)
{
  DeclareEngine(4, 0);
  const gfxFloat ave = StealthConsolasAveCharWidth(mData);
  EXPECT_EQ(double(NS_lround(1126 * float(kStealthConsolasSize / 2048))), ave);
  EXPECT_EQ(11.0, ave);
  EXPECT_EQ(440, StealthNsFontMetricsAveCharWidth(ave));
}

// ClearType with a GDI rendering mode (2, CLEARTYPE_GDI_CLASSIC) measures GDI
// classic: the rounded 'x' advance too.
TEST_F(StealthFontMetrics, GdiRenderingModeTakesTheRoundedXAdvance)
{
  DeclareEngine(5, 2);
  EXPECT_EQ(11.0, StealthConsolasAveCharWidth(mData));
}

// Engine off: stock Firefox, the declaration in the profile is not read and
// FreeType's whole-pixel value stays (1126 at ppem 20 is 10.996, rounded 11).
TEST_F(StealthFontMetrics, EngineOffKeepsMozillasValue)
{
  mozilla::Preferences::SetInt("zoom.stealth.text.system_quality", 5);
  mozilla::Preferences::SetInt(
      "gfx.font_rendering.cleartype_params.rendering_mode", 0);
  const gfxFloat ave = StealthConsolasAveCharWidth(mData);
  EXPECT_EQ(11.0, ave);
  EXPECT_EQ(440, StealthNsFontMetricsAveCharWidth(ave));
}

// Metrics are computed wherever a font is first measured, workers included
// (OffscreenCanvas measureText). The rendering mode reaches InitMetrics through
// the mirror StealthTextMeasuringInit registers (gfxFontCache::Init), never
// through Preferences, which is main-thread only (it asserts in debug builds).
// The pref is changed on the main thread between the two rounds: the mirror
// has to follow it.
TEST_F(StealthFontMetrics, MetricsOffTheMainThreadReadTheDeclaration)
{
  nsCOMPtr<nsIThread> thread;
  ASSERT_TRUE(NS_SUCCEEDED(
      NS_NewNamedThread("StealthFontMet", getter_AddRefs(thread))));
  auto shutdown = mozilla::MakeScopeExit([&] { thread->Shutdown(); });
  const struct {
    int32_t mRenderingMode;
    gfxFloat mExpected;
  } kRounds[] = {
      {0, double(1126 * float(kStealthConsolasSize / 2048))},  // natural
      {2, 11.0},  // CLEARTYPE_GDI_CLASSIC: the rounded 'x' advance
  };
  for (const auto& round : kRounds) {
    DeclareEngine(5, round.mRenderingMode);
    RefPtr<gfxFontEntry> fe = StealthMakeConsolasEntry(mData);
    ASSERT_TRUE(fe);
    gfxFontEntry* entry = fe;
    gfxFloat ave = -1.0;
    bool offMainThread = false;
    ASSERT_TRUE(NS_SUCCEEDED(NS_DispatchAndSpinEventLoopUntilComplete(
        "TestStealthFontMetrics"_ns, thread,
        NS_NewRunnableFunction("TestStealthFontMetrics", [&]() {
          offMainThread = !NS_IsMainThread();
          ave = StealthMeasureConsolasAve(entry);
        }))));
    EXPECT_TRUE(offMainThread);
    EXPECT_EQ(round.mExpected, ave) << round.mRenderingMode;
  }
}

// ---------------------------------------------------------------------------
// Synthetic bold on Linux (gfx/thebes/StealthSyntheticBold.h): with the engine
// on the decision is the one gfxDWriteFont takes on Windows retail with
// gfx.font_rendering.directwrite.bold_simulation = 1.
// ---------------------------------------------------------------------------

// The rule, from its facts.
TEST(StealthSyntheticBoldRule, WebfontOrColrOnlyWhenBoldIsSynthesised)
{
  using mozilla::gfx::StealthMultistrikeBoldRule;
  //                                 engine  needs  webfont colr
  EXPECT_TRUE(StealthMultistrikeBoldRule(true, true, true, false));  // webfont
  EXPECT_TRUE(StealthMultistrikeBoldRule(true, true, false, true));  // COLR
  EXPECT_TRUE(StealthMultistrikeBoldRule(true, true, true, true));
  // An installed font without COLR keeps the rasteriser's simulation.
  EXPECT_FALSE(StealthMultistrikeBoldRule(true, true, false, false));
  // No synthetic bold asked (the face has a bold, or the style is not bold).
  EXPECT_FALSE(StealthMultistrikeBoldRule(true, false, true, true));
  // Engine off: stock Linux, which never multi-strikes.
  EXPECT_FALSE(StealthMultistrikeBoldRule(false, true, true, true));
}

// The answer is left in the render pattern: no FC_EMBOLDEN (so neither FreeType
// nor idwr embolden the face) and the property the two readers look at.
TEST(StealthSyntheticBoldRule, PatternCarriesTheAnswer)
{
  FcPattern* pattern = FcPatternCreate();
  FcPatternAddBool(pattern, FC_EMBOLDEN, FcTrue);
  FcBool embolden = FcFalse;

  // Not multi-struck (installed font, or engine off): the pattern is as it was.
  mozilla::gfx::StealthDeclareMultistrikeBold(pattern, false);
  EXPECT_EQ(FcResultMatch, FcPatternGetBool(pattern, FC_EMBOLDEN, 0, &embolden));
  EXPECT_TRUE(embolden);
  EXPECT_FALSE(mozilla::gfx::StealthPatternMultistrikeBold(pattern));

  mozilla::gfx::StealthDeclareMultistrikeBold(pattern, true);
  EXPECT_EQ(FcResultNoMatch,
            FcPatternGetBool(pattern, FC_EMBOLDEN, 0, &embolden));
  EXPECT_TRUE(mozilla::gfx::StealthPatternMultistrikeBold(pattern));
  FcPatternDestroy(pattern);
}

namespace {

// ScaledFontFontconfig::InstanceData starts with its flags byte, whose bit 2 is
// EMBOLDEN (private to the class; the layout is what GetFontInstanceData hands
// to the other process).
constexpr uint8_t kStealthInstanceEmbolden = 1 << 2;

void StealthTakeFlagsByte(const uint8_t* aData, uint32_t aLength,
                          const mozilla::wr::FontVariation*, uint32_t,
                          void* aBaton) {
  *static_cast<int*>(aBaton) = aLength ? aData[0] : -1;
}

// The flags byte of the instance data of aFont's scaled font; -1 when there is
// none.
int StealthInstanceFlags(gfxFont* aFont, bool* aWrSyntheticBold) {
  RefPtr<mozilla::gfx::ScaledFont> scaled = aFont->GetScaledFont(nullptr);
  if (!scaled) {
    return -1;
  }
  int flags = -1;
  if (!scaled->GetFontInstanceData(StealthTakeFlagsByte, &flags)) {
    return -1;
  }
  mozilla::Maybe<mozilla::wr::FontInstanceOptions> options;
  mozilla::Maybe<mozilla::wr::FontInstancePlatformOptions> platformOptions;
  std::vector<mozilla::wr::FontVariation> variations;
  *aWrSyntheticBold =
      scaled->GetWRFontInstanceOptions(&options, &platformOptions,
                                       &variations) &&
      options.isSome() &&
      bool(options->flags & mozilla::wr::FontInstanceFlags::SYNTHETIC_BOLD);
  return flags;
}

already_AddRefed<gfxFont> StealthMakeConsolasAt(gfxFontEntry* aEntry,
                                                bool aBold) {
  gfxFontStyle style;
  style.size = kStealthConsolasSize;
  if (aBold) {
    style.weight = mozilla::FontWeight::BOLD;
  }
  RefPtr<gfxFont> font = aEntry->FindOrMakeFont(&style);
  return (font && font->Valid()) ? font.forget() : nullptr;
}

}  // namespace

// A webfont that has only a regular face, asked for bold: Gecko strikes it
// (mApplySyntheticBold), the metrics and the cluster advances grow by
// GetSyntheticBoldOffset() once, and the glyph advance itself is the regular
// one (stock Linux added the FreeType embolden strength to it, so that a bold
// webfont was wider than on Windows). The scaled font carries no embolden, which
// is what keeps FreeType and idwr's kSimulationBold off the same face.
TEST_F(StealthFontMetrics, WebfontBoldIsMultistruckAsOnWindows)
{
  DeclareEngine(5, 0);
  RefPtr<gfxFontEntry> fe = StealthMakeConsolasEntry(mData);
  ASSERT_TRUE(fe);
  RefPtr<gfxFont> regular = StealthMakeConsolasAt(fe, false);
  RefPtr<gfxFont> bold = StealthMakeConsolasAt(fe, true);
  ASSERT_TRUE(regular);
  ASSERT_TRUE(bold);
  ASSERT_NE(regular.get(), bold.get());

  EXPECT_FALSE(regular->ApplySyntheticBold());
  EXPECT_TRUE(bold->ApplySyntheticBold());

  const double delta = bold->GetSyntheticBoldOffset();
  EXPECT_GT(delta, 0.25);
  const auto& r = regular->GetMetrics(nsFontMetrics::eHorizontal);
  const auto& b = bold->GetMetrics(nsFontMetrics::eHorizontal);
  EXPECT_DOUBLE_EQ(r.spaceWidth + delta, b.spaceWidth);
  EXPECT_DOUBLE_EQ(r.aveCharWidth + delta, b.aveCharWidth);
  EXPECT_DOUBLE_EQ(r.maxAdvance + delta, b.maxAdvance);
  EXPECT_DOUBLE_EQ(r.zeroWidth + delta, b.zeroWidth);

  // The advance of a glyph is not widened by the font: Gecko widens the cluster.
  EXPECT_DOUBLE_EQ(regular->GetCharAdvance('H'), bold->GetCharAdvance('H'));

  bool wrBold = false;
  int flags = StealthInstanceFlags(bold, &wrBold);
  ASSERT_NE(-1, flags);
  EXPECT_EQ(0, flags & kStealthInstanceEmbolden);
  EXPECT_TRUE(wrBold);  // WebRender, with no multi-strike on FreeType, still bolds
  flags = StealthInstanceFlags(regular, &wrBold);
  ASSERT_NE(-1, flags);
  EXPECT_EQ(0, flags & kStealthInstanceEmbolden);
  EXPECT_FALSE(wrBold);
}

// Engine off: stock Linux. The rasteriser embolden is asked for and the font
// does not multi-strike.
TEST_F(StealthFontMetrics, EngineOffBoldStaysStock)
{
  RefPtr<gfxFontEntry> fe = StealthMakeConsolasEntry(mData);
  ASSERT_TRUE(fe);
  RefPtr<gfxFont> bold = StealthMakeConsolasAt(fe, true);
  ASSERT_TRUE(bold);
  EXPECT_FALSE(bold->ApplySyntheticBold());
  bool wrBold = false;
  const int flags = StealthInstanceFlags(bold, &wrBold);
  ASSERT_NE(-1, flags);
  EXPECT_EQ(kStealthInstanceEmbolden, flags & kStealthInstanceEmbolden);
  EXPECT_TRUE(wrBold);
}

// A webfont that does not need synthetic bold (the style is not bold) draws as
// always, engine on.
TEST_F(StealthFontMetrics, WebfontRegularIsNotMultistruck)
{
  DeclareEngine(5, 0);
  RefPtr<gfxFontEntry> fe = StealthMakeConsolasEntry(mData);
  ASSERT_TRUE(fe);
  RefPtr<gfxFont> regular = StealthMakeConsolasAt(fe, false);
  ASSERT_TRUE(regular);
  EXPECT_FALSE(regular->ApplySyntheticBold());
}
