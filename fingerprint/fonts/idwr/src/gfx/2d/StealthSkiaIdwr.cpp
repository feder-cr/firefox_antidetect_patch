/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthSkiaIdwr.h"

#include <string.h>

#include <vector>

#include "2D.h"
#include "idwr/idwr.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "skia/include/core/SkData.h"
#include "skia/include/core/SkFontStyle.h"
#include "skia/include/ports/SkTypeface_idwr.h"

namespace mozilla {
namespace gfx {

namespace {

uint16_t BE16(const uint8_t* aData) { return uint16_t(aData[0] << 8 | aData[1]); }
uint32_t BE32(const uint8_t* aData) {
  return uint32_t(aData[0]) << 24 | uint32_t(aData[1]) << 16 |
         uint32_t(aData[2]) << 8 | uint32_t(aData[3]);
}

// The font files the unscaled fonts gave, kept by unscaled font so that a scaled font
// per size does not read the file again. A small FIFO: the strong reference keeps the
// pointer from being reused for another font while the entry is here.
struct FontBytes {
  RefPtr<UnscaledFont> mFont;
  sk_sp<SkData> mData;
  uint32_t mIndex = 0;
};
StaticMutex sBytesMutex;
std::vector<FontBytes>* sBytes = nullptr;
constexpr size_t kBytesCacheSize = 32;

struct ReadCtx {
  sk_sp<SkData> mData;
  uint32_t mIndex = 0;
};

sk_sp<SkData> BytesOf(UnscaledFont* aUnscaled, uint32_t* aIndex) {
  {
    StaticMutexAutoLock lock(sBytesMutex);
    if (sBytes) {
      for (const auto& e : *sBytes) {
        if (e.mFont == aUnscaled) {
          *aIndex = e.mIndex;
          return e.mData;
        }
      }
    }
  }
  ReadCtx ctx;
  bool ok = aUnscaled->GetFontFileData(
      [](const uint8_t* aData, uint32_t aLength, uint32_t aIndex, void* aBaton) {
        auto* c = static_cast<ReadCtx*>(aBaton);
        c->mData = SkData::MakeWithCopy(aData, aLength);
        c->mIndex = aIndex;
      },
      &ctx);
  if (!ok || !ctx.mData) {
    return nullptr;
  }
  StaticMutexAutoLock lock(sBytesMutex);
  if (!sBytes) {
    sBytes = new std::vector<FontBytes>();
  }
  if (sBytes->size() >= kBytesCacheSize) {
    sBytes->erase(sBytes->begin());
  }
  sBytes->push_back({aUnscaled, ctx.mData, ctx.mIndex});
  *aIndex = ctx.mIndex;
  return ctx.mData;
}

constexpr int32_t kQualityDefault = 0;
constexpr int32_t kQualityAntialiased = 4;

}  // namespace

bool StealthIdwrAAModeDeclared() {
  return StaticPrefs::zoom_stealth_fpp_hw_seed() > 0;
}

StealthIdwrText StealthIdwrCheckedText(int aRenderingMode, float aGamma,
                                       float aContrast, float aLevel) {
  StealthIdwrText text;
  text.renderingMode = aRenderingMode;
  // Skia doesn't support a gamma value outside of 0-4, so default to 2.2
  text.gamma = (aGamma < 0.0f || aGamma > 4.0f) ? 2.2f : aGamma;
  // Skia doesn't support a contrast value outside of 0-1, so default to 1.0
  text.contrast = (aContrast < 0.0f || aContrast > 1.0f) ? 1.0f : aContrast;
  text.clearTypeLevel = (aLevel < 0.0f || aLevel > 1.0f) ? 1.0f : aLevel;
  return text;
}

SkTypeface* StealthIdwrMakeTypeface(UnscaledFont* aUnscaled, uint32_t aFaceIndex,
                                    uint32_t aSimulations,
                                    const StealthIdwrAxis* aAxes,
                                    uint32_t aAxisCount, int aWeight, int aWidth,
                                    bool aItalic, const StealthIdwrText& aText) {
  uint32_t fileIndex = 0;
  sk_sp<SkData> bytes = BytesOf(aUnscaled, &fileIndex);
  if (!bytes) {
    return nullptr;
  }
  uint32_t index = aFaceIndex != UINT32_MAX ? aFaceIndex : fileIndex;
  std::vector<idwr_axis_value> axes(aAxisCount);
  for (uint32_t i = 0; i < aAxisCount; ++i) {
    axes[i] = {aAxes[i].mTag, aAxes[i].mValue};
  }
  SkFontStyle style(
      aWeight, aWidth,
      aItalic ? SkFontStyle::kItalic_Slant : SkFontStyle::kUpright_Slant);
  return SkCreateTypefaceFromIdwr(std::move(bytes), index, aSimulations, axes.data(),
                                  aAxisCount, style, aText.renderingMode, aText.gamma,
                                  aText.contrast, aText.clearTypeLevel);
}

bool StealthIdwrDoGrayscale(const uint8_t* aGasp, size_t aGaspLength, float aPpem) {
  // ScaledFontDWrite.cpp, DoGrayscale: the first range that reaches the size says
  // whether it grays (GASP_DOGRAY, 0x2); no table, a short one, or no such range:
  // gray. Its readShort is (*aBuf << 8) | *(aBuf + 1) on plain chars, signed on
  // Windows: a byte of 0x80 or more sign extends. The same here, whatever the OS or
  // CPU (a plain char is unsigned on Linux arm64).
  constexpr unsigned short kGaspDoGray = 0x2;
  auto readShort = [](const uint8_t* aBuf) -> unsigned short {
    return (unsigned short)((int(int8_t(aBuf[0])) << 8) | int(int8_t(aBuf[1])));
  };
  if (!aGasp || aGaspLength < 4) {
    return true;
  }
  unsigned short numRanges = readShort(aGasp + 2);
  if (aGaspLength < size_t(4) + size_t(numRanges) * 4) {
    return true;
  }
  for (int i = 0; i < numRanges; i++) {
    const uint8_t* range = aGasp + 4 + i * 4;
    if (readShort(range) >= aPpem) {
      if (!(readShort(range + 2) & kGaspDoGray)) {
        return false;
      }
      break;
    }
  }
  return true;
}

bool StealthIdwrIsCJK(const uint8_t* aOS2, size_t aOS2Length) {
  // gfxDWriteFontEntry::IsCJKFont: ulCodePageRange1 (offset 78) of the OS/2 table,
  // present when the table reaches sxHeight (offset 86).
  if (!aOS2 || aOS2Length < 86) {
    return false;
  }
  const uint32_t kCJKCodepageBits = (1 << 17) |  // 932 JIS/Japan
                                    (1 << 18) |  // 936 Chinese (simplified)
                                    (1 << 19) |  // 949 Korean Wansung
                                    (1 << 20) |  // 950 Chinese (traditional)
                                    (1 << 21);   // 1361 Korean Johab
  return (BE32(aOS2 + 78) & kCJKCodepageBits) != 0;
}

bool StealthIdwrHasBitmapStrike(const uint8_t* aEBLC, size_t aEBLCLength,
                                const uint8_t* aEBSC, size_t aEBSCLength,
                                uint32_t aSize) {
  // gfxDWriteFont::HasBitmapStrikeForSize. EBLC/EBSC header: version, numSizes (8
  // bytes). A BitmapSizeTable is 48 bytes (startGlyphIndex at 40, endGlyphIndex at
  // 42, ppemX at 44, ppemY at 45); a BitmapScaleTable 28 (ppemX at 24, ppemY at 25).
  if (aEBLC && aEBLCLength >= 8 && BE32(aEBLC) == 0x00020000) {
    uint32_t numSizes = BE32(aEBLC + 4);
    if (numSizes <= 0xffff && aEBLCLength >= 8 + size_t(numSizes) * 48) {
      for (uint32_t i = 0; i < numSizes; ++i) {
        const uint8_t* table = aEBLC + 8 + size_t(i) * 48;
        if (table[44] == aSize && table[45] == aSize) {
          // A strike of fewer than 4 glyphs is a font that has bitmaps only for the
          // shading characters (Courier New).
          if (uint16_t(BE16(table + 42)) >= uint16_t(BE16(table + 40) + 3)) {
            return true;
          }
          break;
        }
      }
    }
  }
  if (aEBSC && aEBSCLength >= 8 && BE32(aEBSC) == 0x00020000) {
    uint32_t numSizes = BE32(aEBSC + 4);
    if (numSizes <= 0xffff && aEBSCLength >= 8 + size_t(numSizes) * 28) {
      for (uint32_t i = 0; i < numSizes; ++i) {
        const uint8_t* table = aEBSC + 8 + size_t(i) * 28;
        if (table[24] == aSize && table[25] == aSize) {
          return true;
        }
      }
    }
  }
  return false;
}

AntialiasMode StealthIdwrDefaultAAMode(int32_t aQuality, float aLevel,
                                       bool aDoGrayscale) {
  // GetSystemDefaultAAMode: CLEARTYPE_QUALITY subpixel, ANTIALIASED_QUALITY gray,
  // DEFAULT_QUALITY none; then ScaledFontDWrite::GetDefaultAAMode: a ClearType level
  // of 0 makes subpixel gray, and a gray mode the gasp table does not allow is none.
  switch (aQuality) {
    case kQualityDefault:
      return AntialiasMode::NONE;
    case kQualityAntialiased:
      return aDoGrayscale ? AntialiasMode::GRAY : AntialiasMode::NONE;
    default:
      return aLevel == 0.0f ? AntialiasMode::GRAY : AntialiasMode::SUBPIXEL;
  }
}

}  // namespace gfx
}  // namespace mozilla
