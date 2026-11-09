/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Skia draws text with idwr, as the retail Firefox on Windows draws it with DirectWrite
// through Skia. Each case draws one glyph with DrawTargetSkia on an A8 target, through a
// ScaledFont of the platform, and compares the bytes with the A8 mask DirectWrite gave
// Skia's call pattern for it (StealthSkiaIdwrGolden.inc, from gen_skia_text_golden.py).
// A variable font's rows name the axes of the instance ("wght=700"): the scaled font is made
// with those variations, the way a page's font-variation-settings reach it, and the mask is
// the instance's. Before the variations were read from the scaled font's own face the instance
// was always the default one.
// The declared gamma is 1.0 and the contrast 0, so Skia's mask preblend is the identity and
// the target holds the mask itself (white on zero, source over, mask alpha).
//
// Before the port this fails: the glyph came from FreeType (Linux) or the host's
// DirectWrite (Windows).

#include <map>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "StealthTestFonts.h"
#include "gfxPlatform.h"
#include "mozilla/Preferences.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/webrender/WebRenderTypes.h"
#include "nsDirectoryServiceDefs.h"
#include "nsDirectoryServiceUtils.h"
#include "nsIFile.h"
#include "nsString.h"
#include "nsTArray.h"
#include "prio.h"
#ifdef XP_WIN
#  include "ScaledFontBase.h"
#  include "ScaledFontDWrite.h"
#  include "gfxDWriteFonts.h"
#  include "skia/include/core/SkData.h"
#  include "skia/include/core/SkTypeface.h"
#else
#  include "StealthIdwrTextVars.h"
#endif

using namespace mozilla;
using namespace mozilla::gfx;

namespace {

struct SkiaCase {
  const char* font;
  float em;
  uint16_t glyph;
  float subpixelX;
  int32_t left, top, right, bottom;
  uint64_t fnv;
  int mode, texture;
  const char* axes;  // "tag=value,tag=value" of a variable instance, null for the face as is
};

const SkiaCase kCases[] = {
#include "StealthSkiaIdwrGolden.inc"
};

constexpr int kSide = 160;
constexpr int kOriginX = 48;
constexpr int kOriginY = 96;

// "wght=700,wdth=75" as the variations a scaled font is made with (tags big-endian, as
// WebRender and both platforms' CreateScaledFont take them).
std::vector<wr::FontVariation> ParseAxes(const char* aAxes) {
  std::vector<wr::FontVariation> out;
  while (aAxes && *aAxes) {
    const uint32_t tag = (uint32_t(uint8_t(aAxes[0])) << 24) |
                         (uint32_t(uint8_t(aAxes[1])) << 16) |
                         (uint32_t(uint8_t(aAxes[2])) << 8) | uint32_t(uint8_t(aAxes[3]));
    char* end = nullptr;
    const float value = strtof(aAxes + 5, &end);
    out.push_back(wr::FontVariation{tag, value});
    aAxes = *end == ',' ? end + 1 : end;
  }
  return out;
}

uint64_t Fnv1a64(const uint8_t* aData, size_t aLen, uint64_t aHash = 0xcbf29ce484222325ULL) {
  for (size_t i = 0; i < aLen; i++) {
    aHash = (aHash ^ aData[i]) * 0x100000001b3ULL;
  }
  return aHash;
}

using stealth_test_fonts::ReadBundledFont;

void UpdateTextVars() {
#ifdef XP_WIN
  gfxDWriteFont::UpdateSystemTextVars();
#else
  StealthIdwrUpdateTextVars();
#endif
}

const char* const kPrefs[] = {
    "zoom.stealth.fpp.hw_seed",
    "zoom.stealth.text.system_quality",
    "gfx.font_rendering.cleartype_params.gamma",
    "gfx.font_rendering.cleartype_params.enhanced_contrast",
    "gfx.font_rendering.cleartype_params.cleartype_level",
    "gfx.font_rendering.cleartype_params.rendering_mode",
};

// Back to stock Firefox, and the text vars recomputed, whatever the test did.
struct RestoreStock {
  ~RestoreStock() {
    for (const char* name : kPrefs) {
      Preferences::ClearUser(name);
    }
    UpdateTextVars();
  }
};

struct FontEntry {
  nsTArray<uint8_t> bytes;
  RefPtr<NativeFontResource> resource;
  RefPtr<UnscaledFont> unscaled;
  std::map<std::pair<float, std::string>, RefPtr<ScaledFont>> scaled;
};

}  // namespace

TEST(StealthSkiaIdwr, GlyphsAreDirectWritesMasks)
{
  gfxPlatform::GetPlatform();
  RestoreStock restore;
  Preferences::SetInt("zoom.stealth.text.system_quality", 5);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.gamma", 1000);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.enhanced_contrast", 0);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.cleartype_level", 100);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.rendering_mode", 0);
  // The engine on, last: a change of the other prefs while it is on updates the text vars,
  // and a declaration without the rendering mode is refused.
  Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
  UpdateTextVars();

#ifdef XP_WIN
  const FontType fontType = FontType::DWRITE;
#else
  const FontType fontType = FontType::FONTCONFIG;
#endif

  std::map<std::string, FontEntry> fonts;
  size_t failures = 0, compared = 0;
  for (const SkiaCase& c : kCases) {
    FontEntry& entry = fonts[c.font];
    if (!entry.unscaled) {
      entry.bytes = ReadBundledFont(c.font);
      ASSERT_FALSE(entry.bytes.IsEmpty()) << "bundled font missing: " << c.font;
      entry.resource = Factory::CreateNativeFontResource(
          entry.bytes.Elements(), entry.bytes.Length(), fontType);
      ASSERT_TRUE(entry.resource) << c.font;
      entry.unscaled = entry.resource->CreateUnscaledFont(0, nullptr, 0);
      ASSERT_TRUE(entry.unscaled) << c.font;
    }
    const std::string axes = c.axes ? c.axes : "";
    RefPtr<ScaledFont>& font = entry.scaled[{c.em, axes}];
    if (!font) {
      wr::FontInstanceOptions options = {};
      wr::FontInstancePlatformOptions platform = {};
      const std::vector<wr::FontVariation> variations = ParseAxes(c.axes);
      font = entry.unscaled->CreateScaledFontFromWRFont(
          c.em, &options, &platform, variations.empty() ? nullptr : variations.data(),
          uint32_t(variations.size()));
      ASSERT_TRUE(font) << c.font << " em " << c.em << " " << axes;
    }

    RefPtr<DrawTarget> dt = Factory::CreateDrawTarget(
        BackendType::SKIA, IntSize(kSide, kSide), SurfaceFormat::A8);
    ASSERT_TRUE(dt);
    dt->ClearRect(Rect(0, 0, kSide, kSide));
    Glyph glyph;
    glyph.mIndex = c.glyph;
    glyph.mPosition = Point(float(kOriginX) + c.subpixelX, float(kOriginY));
    GlyphBuffer buffer = {&glyph, 1};
    dt->FillGlyphs(font, buffer, ColorPattern(DeviceColor(1, 1, 1, 1)),
                   DrawOptions(1.0f, CompositionOp::OP_OVER, AntialiasMode::GRAY));

    RefPtr<SourceSurface> snapshot = dt->Snapshot();
    ASSERT_TRUE(snapshot);
    RefPtr<DataSourceSurface> data = snapshot->GetDataSurface();
    ASSERT_TRUE(data);
    DataSourceSurface::ScopedMap map(data, DataSourceSurface::READ);
    ASSERT_TRUE(map.IsMapped());

    // The mask's rectangle, row by row, and nothing outside it.
    const int w = c.right - c.left, h = c.bottom - c.top;
    uint64_t hash = 0xcbf29ce484222325ULL;
    bool outsideClear = true;
    for (int y = 0; y < kSide; ++y) {
      const uint8_t* row = map.GetData() + size_t(y) * map.GetStride();
      for (int x = 0; x < kSide; ++x) {
        const bool inside = x >= kOriginX + c.left && x < kOriginX + c.right &&
                            y >= kOriginY + c.top && y < kOriginY + c.bottom;
        if (!inside && row[x]) {
          outsideClear = false;
        }
      }
      if (y >= kOriginY + c.top && y < kOriginY + c.bottom) {
        hash = Fnv1a64(row + kOriginX + c.left, size_t(w), hash);
      }
    }
    ++compared;
    if (hash != c.fnv || !outsideClear) {
      if (failures++ < 12) {
        ADD_FAILURE() << c.font << (c.axes ? " " : "") << (c.axes ? c.axes : "")
                      << " em " << c.em << " glyph " << c.glyph << " x +"
                      << c.subpixelX << ": mask hash " << std::hex << hash
                      << ", DirectWrite " << c.fnv << std::dec
                      << (outsideClear ? "" : ", ink outside the DirectWrite bounds")
                      << " (" << w << "x" << h << ", mode " << c.mode << ")";
      }
    }
  }
  EXPECT_EQ(failures, size_t(0)) << failures << " of " << compared << " masks differ";
  EXPECT_GT(compared, size_t(500));
}

// The variable rows prove something only if the instances differ: at every glyph, size and
// subpixel position the masks of wght 300, 400 and 700 are three different masks, so a
// scaled font that drew the default instance for all of them cannot pass the rows.
TEST(StealthSkiaIdwr, VariableInstancesOfTheGoldenDiffer)
{
  std::map<std::string, std::map<std::string, uint64_t>> byKey;
  for (const SkiaCase& c : kCases) {
    const std::string axes = c.axes ? c.axes : "";
    if (axes == "wght=300" || axes == "wght=400" || axes == "wght=700") {
      byKey[std::string(c.font) + " " + std::to_string(c.em) + " " +
            std::to_string(c.glyph) + " " + std::to_string(c.subpixelX)][axes] = c.fnv;
    }
  }
  size_t keys = 0;
  for (const auto& entry : byKey) {
    const auto& m = entry.second;
    ASSERT_EQ(m.size(), size_t(3)) << entry.first;
    EXPECT_NE(m.at("wght=300"), m.at("wght=400")) << entry.first;
    EXPECT_NE(m.at("wght=400"), m.at("wght=700")) << entry.first;
    EXPECT_NE(m.at("wght=300"), m.at("wght=700")) << entry.first;
    ++keys;
  }
  EXPECT_GT(keys, size_t(100));
}

#ifdef XP_WIN

// Windows: Skia's text is idwr's only with the engine on (zoom.stealth.fpp.hw_seed > 0).
// With it off ScaledFontDWrite gives Skia the host's DirectWrite typeface, as stock Firefox;
// with it on the typeface is idwr's, and a font idwr cannot take has none: the host's
// DirectWrite is never the fallback.

namespace {

constexpr uint32_t FourByteTag(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
         (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

// Whether the typeface serializes with this factory id (DirectWrite's 'dwrt', idwr's 'idwr'):
// the descriptor Skia writes names the class that made the typeface.
bool HasFactoryId(SkTypeface* aTypeface, uint32_t aId) {
  sk_sp<SkData> data =
      aTypeface->serialize(SkTypeface::SerializeBehavior::kDontIncludeData);
  if (!data || data->size() < sizeof(aId)) {
    return false;
  }
  const uint8_t* bytes = static_cast<const uint8_t*>(data->data());
  for (size_t i = 0; i + sizeof(aId) <= data->size(); ++i) {
    uint32_t value;
    memcpy(&value, bytes + i, sizeof(value));
    if (value == aId) {
      return true;
    }
  }
  return false;
}

constexpr uint32_t kDirectWrite = FourByteTag('d', 'w', 'r', 't');
constexpr uint32_t kIdwr = FourByteTag('i', 'd', 'w', 'r');

// What a launcher declares for the text with the engine on.
void DeclareEngineOn() {
  Preferences::SetInt("zoom.stealth.text.system_quality", 5);
  Preferences::SetInt("gfx.font_rendering.cleartype_params.rendering_mode", 0);
  Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
  UpdateTextVars();
}

struct DWriteFont {
  nsTArray<uint8_t> bytes;
  RefPtr<NativeFontResource> resource;
  RefPtr<UnscaledFont> unscaled;
  RefPtr<ScaledFont> scaled;
};

// A scaled font of a bundled font through the DirectWrite backend, as the golden test
// makes them. A new one each time: a scaled font keeps the typeface it first made.
void MakeDWriteFont(DWriteFont& aOut) {
  aOut.bytes = ReadBundledFont("arial7_04.ttf");
  ASSERT_FALSE(aOut.bytes.IsEmpty());
  aOut.resource = Factory::CreateNativeFontResource(
      aOut.bytes.Elements(), aOut.bytes.Length(), FontType::DWRITE);
  ASSERT_TRUE(aOut.resource);
  aOut.unscaled = aOut.resource->CreateUnscaledFont(0, nullptr, 0);
  ASSERT_TRUE(aOut.unscaled);
  wr::FontInstanceOptions options = {};
  wr::FontInstancePlatformOptions platform = {};
  aOut.scaled = aOut.unscaled->CreateScaledFontFromWRFont(12.0f, &options,
                                                          &platform, nullptr, 0);
  ASSERT_TRUE(aOut.scaled);
}

// An unscaled font whose bytes cannot be read, which idwr takes its typeface from.
class UnreadableUnscaledFont final : public UnscaledFont {
 public:
  MOZ_DECLARE_REFCOUNTED_VIRTUAL_TYPENAME(UnreadableUnscaledFont, override)
  FontType GetType() const override { return FontType::DWRITE; }
};

}  // namespace

TEST(StealthSkiaIdwrWin, WithTheEngineOffSkiaDrawsWithDirectWrite)
{
  gfxPlatform::GetPlatform();
  RestoreStock restore;
  Preferences::ClearUser("zoom.stealth.fpp.hw_seed");
  UpdateTextVars();

  DWriteFont font;
  ASSERT_NO_FATAL_FAILURE(MakeDWriteFont(font));
  SkTypeface* typeface = static_cast<ScaledFontBase*>(font.scaled.get())->GetSkTypeface();
  ASSERT_TRUE(typeface);
  EXPECT_TRUE(HasFactoryId(typeface, kDirectWrite));
  EXPECT_FALSE(HasFactoryId(typeface, kIdwr));
}

TEST(StealthSkiaIdwrWin, WithTheEngineOnSkiaDrawsWithIdwr)
{
  gfxPlatform::GetPlatform();
  RestoreStock restore;
  DeclareEngineOn();

  DWriteFont font;
  ASSERT_NO_FATAL_FAILURE(MakeDWriteFont(font));
  SkTypeface* typeface = static_cast<ScaledFontBase*>(font.scaled.get())->GetSkTypeface();
  ASSERT_TRUE(typeface);
  EXPECT_TRUE(HasFactoryId(typeface, kIdwr));
  EXPECT_FALSE(HasFactoryId(typeface, kDirectWrite));
}

// The engine on, idwr cannot take the font (its bytes cannot be read): there is no typeface.
// Before, ScaledFontDWrite went on to SkCreateTypefaceFromDWriteFont and the host's
// DirectWrite drew the glyphs. The scaled font here has a real face and an unscaled font
// that gives no bytes, so the only way to a typeface is DirectWrite's.
TEST(StealthSkiaIdwrWin, AFontIdwrRefusesHasNoTypefaceWithTheEngineOn)
{
  gfxPlatform::GetPlatform();
  RestoreStock restore;
  DeclareEngineOn();

  DWriteFont real;
  ASSERT_NO_FATAL_FAILURE(MakeDWriteFont(real));
  IDWriteFontFace* face = static_cast<ScaledFontDWrite*>(real.scaled.get())->mFontFace;
  ASSERT_TRUE(face);

  RefPtr<UnscaledFont> unreadable = new UnreadableUnscaledFont();
  RefPtr<ScaledFontDWrite> font = new ScaledFontDWrite(
      face, unreadable, 12.0f, /* aUseEmbeddedBitmap */ false,
      /* aUseMultistrikeBold */ false, /* aGDIForced */ false, nullptr);
  EXPECT_FALSE(font->GetSkTypeface());
}

#endif  // XP_WIN
