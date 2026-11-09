/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The fontconfig scaled font on Linux as ScaledFontDWrite is on Windows, for Skia: the
// typeface is idwr's, the options are the ones DirectWrite's scaled font writes. Every
// value comes from the font's own tables or the declared gfxVars, never from the
// FcPattern or the host.
//
// With the engine on (zoom.stealth.fpp.hw_seed > 0) the text of every scaled font is
// idwr's, and a font idwr cannot take has no typeface: the host's FreeType is never asked
// for a glyph. With the engine off nothing here applies and Skia draws as stock Firefox.

#include <cmath>
#include <vector>

#include "Logging.h"
#include "ScaledFontFontconfig.h"
#include "StealthSkiaIdwr.h"
#include "UnscaledFontFreeType.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/webrender/WebRenderTypes.h"
#include "skia/include/core/SkFont.h"

#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

namespace mozilla {
namespace gfx {

namespace {

constexpr uint32_t kSimulationBold = 1;

// The bytes of an sfnt table, empty when the face has none.
std::vector<uint8_t> LoadTable(FT_Face aFace, FT_ULong aTag) {
  FT_ULong length = 0;
  if (FT_Load_Sfnt_Table(aFace, aTag, 0, nullptr, &length) != 0 || !length) {
    return {};
  }
  std::vector<uint8_t> bytes(length);
  if (FT_Load_Sfnt_Table(aFace, aTag, 0, bytes.data(), &length) != 0) {
    return {};
  }
  return bytes;
}

const uint8_t* Data(const std::vector<uint8_t>& aBytes) {
  return aBytes.empty() ? nullptr : aBytes.data();
}

FT_Face FaceOf(ScaledFontFontconfig* aFont) {
  auto* unscaled =
      static_cast<UnscaledFontFreeType*>(aFont->GetUnscaledFont().get());
  return unscaled->GetFace()->GetFace();
}

// The instance's design coordinates, as the axis values CreateFontFace takes. They are
// the scaled font's own (the variation settings live on its cloned face, not on the
// unscaled font's shared one), read the way Gecko reads them for WebRender: its
// FreeType entry points are resolved there, since a system FreeType may not export
// them. Axes at their defaults give none, and idwr then opens the default instance.
std::vector<StealthIdwrAxis> AxesOf(ScaledFontFontconfig* aFont) {
  Maybe<wr::FontInstanceOptions> options;
  Maybe<wr::FontInstancePlatformOptions> platformOptions;
  std::vector<wr::FontVariation> variations;
  aFont->GetWRFontInstanceOptions(&options, &platformOptions, &variations);
  std::vector<StealthIdwrAxis> axes;
  axes.reserve(variations.size());
  for (const wr::FontVariation& v : variations) {
    axes.push_back({uint32_t(v.tag), v.value});
  }
  return axes;
}

}  // namespace

SkTypeface* StealthIdwrSkTypeface(ScaledFontFontconfig* aFont, bool aEmbolden) {
  auto* unscaled =
      static_cast<UnscaledFontFreeType*>(aFont->GetUnscaledFont().get());
  FT_Face face = unscaled->GetFace()->GetFace();

  std::vector<StealthIdwrAxis> axes;
  if (aFont->HasVariationSettings()) {
    axes = AxesOf(aFont);
  }
  StealthIdwrText text = StealthIdwrCheckedText(
      gfxVars::SystemTextRenderingMode(), gfxVars::SystemTextGamma(),
      gfxVars::SystemTextEnhancedContrast(), gfxVars::SystemTextClearTypeLevel());
  const bool italic = face->style_flags & FT_STYLE_FLAG_ITALIC;
  const bool bold = face->style_flags & FT_STYLE_FLAG_BOLD;
  SkTypeface* typeface = StealthIdwrMakeTypeface(
      unscaled, unscaled->GetIndex(), aEmbolden ? kSimulationBold : 0, axes.data(),
      uint32_t(axes.size()), bold ? 700 : 400, 5, italic, text);
  if (!typeface) {
    // The bytes cannot be read, or idwr refuses the font (as CreateFontFace refuses a
    // face whose COLR or CPAL are invalid). On Windows that is a DirectWrite failure and
    // the font draws nothing; here it is the same, never FreeType.
    gfxCriticalNote << "stealth: idwr takes no typeface of this font, it draws nothing";
  }
  return typeface;
}

bool StealthIdwrSetupSkFont(ScaledFontFontconfig* aFont, SkFont& aSkFont) {
  if (!StealthIdwrAAModeDeclared()) {
    return false;
  }
  // ScaledFontDWrite::SetupSkFontDrawOptions with GDI not forced (StealthIdwrTextVars
  // refuses a missing rendering_mode, the case Windows forces it in): embedded bitmaps by
  // gfxDWriteFont::GetScaledFont's rule (rendering mode default, a CJK font with a
  // bitmap strike at the rounded size), subpixel positioning always.
  bool embedded = false;
  if (gfxVars::SystemTextRenderingMode() == 0) {
    FT_Face face = FaceOf(aFont);
    std::vector<uint8_t> os2 = LoadTable(face, TTAG_OS2);
    if (StealthIdwrIsCJK(Data(os2), os2.size())) {
      std::vector<uint8_t> eblc =
          LoadTable(face, FT_MAKE_TAG('E', 'B', 'L', 'C'));
      std::vector<uint8_t> ebsc =
          LoadTable(face, FT_MAKE_TAG('E', 'B', 'S', 'C'));
      embedded = StealthIdwrHasBitmapStrike(Data(eblc), eblc.size(), Data(ebsc),
                                            ebsc.size(),
                                            uint32_t(std::lround(aFont->GetSize())));
    }
  }
  aSkFont.setEmbeddedBitmaps(embedded);
  aSkFont.setSubpixel(true);
  return true;
}

AntialiasMode StealthIdwrFontconfigAAMode(ScaledFontFontconfig* aFont) {
  const int32_t quality = gfxVars::SystemTextQuality();
  bool doGray = true;
  if (quality == 4) {
    std::vector<uint8_t> gasp = LoadTable(FaceOf(aFont), TTAG_gasp);
    doGray = StealthIdwrDoGrayscale(Data(gasp), gasp.size(), aFont->GetSize());
  }
  return StealthIdwrDefaultAAMode(quality, gfxVars::SystemTextClearTypeLevel(),
                                  doGray);
}

}  // namespace gfx
}  // namespace mozilla
