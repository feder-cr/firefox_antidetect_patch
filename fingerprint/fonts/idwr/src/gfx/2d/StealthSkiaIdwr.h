/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_STEALTHSKIAIDWR_H_
#define MOZILLA_GFX_STEALTHSKIAIDWR_H_

#include <stddef.h>
#include <stdint.h>

#include "mozilla/Maybe.h"
#include "mozilla/gfx/Types.h"

class SkFont;
class SkTypeface;

namespace mozilla {
namespace gfx {

class ScaledFontDWrite;
class ScaledFontFontconfig;
class UnscaledFont;

// Skia draws text with idwr (gfx/skia SkTypeface_idwr, SkScalerContext_idwr) where the
// retail Firefox on Windows draws it with DirectWrite through Skia: blobs, canvas 2D,
// SVG text, snapshots. These are the ScaledFont ports' halves of it, from the declared
// text values (gfxVars SystemText*), never from the host.

// What ScaledFontDWrite::CreateSkTypeface passes SkCreateTypefaceFromDWriteFont, with
// its range checks: gamma outside 0-4 is 2.2, contrast and ClearType level outside 0-1
// are 1.0.
struct StealthIdwrText {
  int renderingMode = 0;  // DWRITE_RENDERING_MODE
  float gamma = 2.2f;
  float contrast = 1.0f;
  float clearTypeLevel = 1.0f;
};
StealthIdwrText StealthIdwrCheckedText(int aRenderingMode, float aGamma,
                                       float aContrast, float aLevel);

struct StealthIdwrAxis {
  uint32_t mTag;  // IDWR_TAG: 'wght' is 0x77676874
  float mValue;
};

// The typeface over the font's whole sfnt file as the unscaled font gives it (kept by
// unscaled font). aFaceIndex UINT32_MAX: the index the unscaled font reports.
// aSimulations is DWRITE_FONT_SIMULATIONS (bold 1, oblique 2). Null when the font
// cannot be read or idwr refuses it.
SkTypeface* StealthIdwrMakeTypeface(UnscaledFont* aUnscaled, uint32_t aFaceIndex,
                                    uint32_t aSimulations,
                                    const StealthIdwrAxis* aAxes,
                                    uint32_t aAxisCount, int aWeight, int aWidth,
                                    bool aItalic, const StealthIdwrText& aText);

// The gasp and strike rules of ScaledFontDWrite and gfxDWriteFont over sfnt tables (a
// missing table is nullptr). DoGrayscale reads the gasp bytes as signed chars, as the
// Windows build does, on every OS and CPU.
bool StealthIdwrDoGrayscale(const uint8_t* aGasp, size_t aGaspLength, float aPpem);
bool StealthIdwrIsCJK(const uint8_t* aOS2, size_t aOS2Length);
bool StealthIdwrHasBitmapStrike(const uint8_t* aEBLC, size_t aEBLCLength,
                                const uint8_t* aEBSC, size_t aEBSCLength,
                                uint32_t aSize);

// ScaledFontDWrite::GetDefaultAAMode from a declared text quality (0 default, 4
// antialiased, 5 ClearType), the ClearType level and the font's gasp table.
AntialiasMode StealthIdwrDefaultAAMode(int32_t aQuality, float aLevel,
                                       bool aDoGrayscale);

#ifdef MOZ_WIDGET_GTK
// ScaledFontFontconfig on Linux, as ScaledFontDWrite behaves on Windows. aEmbolden is
// InstanceData::EMBOLDEN (private to the scaled font): Gecko's NeedsSyntheticBold,
// which is DirectWrite's bold simulation here. Null is a refusal (the font cannot be read,
// or idwr rejects it), logged: the caller returns it, it does not try another engine.
// Called only where StealthIdwrAAModeDeclared().
SkTypeface* StealthIdwrSkTypeface(ScaledFontFontconfig* aFont, bool aEmbolden);
// SetupSkFontDrawOptions as ScaledFontDWrite writes it when GDI is not forced (the engine
// refuses to start without a declared rendering mode, so the text is never GDI forced
// here). False, nothing done, with the engine off.
bool StealthIdwrSetupSkFont(ScaledFontFontconfig* aFont, SkFont& aSkFont);
AntialiasMode StealthIdwrFontconfigAAMode(ScaledFontFontconfig* aFont);
#endif

// True with the engine on (zoom.stealth.fpp.hw_seed > 0): the text of every scaled font is
// idwr's, and a font idwr refuses has no typeface (null, never the host's FreeType or
// DirectWrite). With the engine off it is false and the hooks leave Mozilla's lines as
// they are: Skia draws through FreeType on Linux and DirectWrite on Windows, as stock
// Firefox. A function defined in another unit, so that the hooks read as conditions
// Mozilla's lines stay under.
bool StealthIdwrAAModeDeclared();

#ifdef XP_WIN
// ScaledFontDWrite::CreateSkTypeface's typeface with the engine on. Null is a refusal (the
// font cannot be read, or idwr rejects it), logged: the caller returns it, it does not ask
// DirectWrite. Called only where StealthIdwrAAModeDeclared().
SkTypeface* StealthIdwrSkTypeface(ScaledFontDWrite* aFont);
#endif

}  // namespace gfx
}  // namespace mozilla

#endif  // MOZILLA_GFX_STEALTHSKIAIDWR_H_
