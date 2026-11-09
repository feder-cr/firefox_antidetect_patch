/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// ScaledFontDWrite's typeface for Skia, drawn by idwr instead of DirectWrite. The
// values are the ones ScaledFontDWrite::CreateSkTypeface hands
// SkCreateTypefaceFromDWriteFont: DWriteSettings (declared gfxVars), the face's
// simulations and axis values, the font's bytes. Called only with the engine on
// (StealthIdwrAAModeDeclared); with it off ScaledFontDWrite is Mozilla's, DirectWrite's.

#include <vector>

#include "DWriteSettings.h"
#include "Logging.h"
#include "ScaledFontDWrite.h"
#include "StealthSkiaIdwr.h"
#include "UnscaledFontDWrite.h"
#include "dwrite_3.h"
#include "skia/include/core/SkFontStyle.h"

namespace mozilla {
namespace gfx {

SkTypeface* StealthIdwrSkTypeface(ScaledFontDWrite* aFont) {
  auto& settings = aFont->DWriteSettings();
  StealthIdwrText text = StealthIdwrCheckedText(
      (int)settings.RenderingMode(), settings.Gamma(), settings.EnhancedContrast(),
      settings.ClearTypeLevel());

  IDWriteFontFace* face = aFont->mFontFace;
  std::vector<StealthIdwrAxis> axes;
  RefPtr<IDWriteFontFace5> face5;
  if (SUCCEEDED(face->QueryInterface(__uuidof(IDWriteFontFace5),
                                     getter_AddRefs(face5))) &&
      face5->HasVariations()) {
    std::vector<DWRITE_FONT_AXIS_VALUE> values(face5->GetFontAxisValueCount());
    if (!values.empty() &&
        SUCCEEDED(face5->GetFontAxisValues(values.data(), values.size()))) {
      for (const auto& v : values) {
        // DirectWrite's axis tags read little-endian: idwr's are big-endian.
        uint32_t tag = (uint32_t(v.axisTag) << 24) |
                       ((uint32_t(v.axisTag) & 0xff00u) << 8) |
                       ((uint32_t(v.axisTag) >> 8) & 0xff00u) |
                       (uint32_t(v.axisTag) >> 24);
        axes.push_back({tag, v.value});
      }
    }
  }
  const bool italic = aFont->mStyle.slant() != SkFontStyle::kUpright_Slant;
  SkTypeface* typeface = StealthIdwrMakeTypeface(
      aFont->GetUnscaledFont().get(), face->GetIndex(),
      (uint32_t)face->GetSimulations(), axes.data(), uint32_t(axes.size()),
      aFont->mStyle.weight(), aFont->mStyle.width(), italic, text);
  if (!typeface) {
    // The bytes cannot be read, or idwr refuses the font (as CreateFontFace refuses a
    // face whose COLR or CPAL are invalid). With the engine on the host's DirectWrite is
    // never asked for the glyphs: the font draws nothing, as on Linux.
    gfxCriticalNote << "stealth: idwr takes no typeface of this font, it draws nothing";
  }
  return typeface;
}

}  // namespace gfx
}  // namespace mozilla
