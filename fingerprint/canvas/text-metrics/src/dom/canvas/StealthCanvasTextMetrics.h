/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_CANVAS_STEALTHCANVASTEXTMETRICS_H_
#define DOM_CANVAS_STEALTHCANVASTEXTMETRICS_H_

// Header-only on purpose: the one includer is CanvasRenderingContext2D.cpp, so
// no source file has to be registered in dom/canvas/moz.build (bug 1745384:
// the unified chunks of this directory must not be disturbed), and the
// function is compiled in the same translation unit the stock code was.

#include <cmath>

#include "StealthDeclarationGate.h"
#include "gfxFont.h"
#include "mozilla/Assertions.h"
#include "mozilla/RefPtr.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/dom/CanvasRenderingContext2DBinding.h"
#include "mozilla/dom/TextMetrics.h"
#include "nsFontMetrics.h"

namespace mozilla::dom {

// The four actualBoundingBox* values the stock code computed from the glyph
// outlines (CanvasRenderingContext2D::DrawOrMeasureText). They are handed over
// unchanged and come back unchanged in the TextMetrics.
struct StealthInkBox {
  double mLeft;
  double mRight;
  double mAscent;
  double mDescent;
};

// measureText() on a horizontal run with the stealth engine on: the vertical
// metrics from the bundled font's own OS/2 table instead of the host's
// rasteriser, rounded the way a Windows Firefox rounds them. Returns nullptr
// when the engine is off, the run is vertical, or the font has no usable OS/2
// table: the caller then builds the stock TextMetrics.
//
// STEALTH: replace the host-engine vertical metrics with host-INDEPENDENT
// values from the font's OS/2 sfnt table, so every TextMetrics vertical field
// is byte-identical across hosts (DWrite vs FreeType vs CoreText) for a given
// bundled font + size. The width is host-independent because the bundled
// font's advances are; there is no jitter in it (the per-run jitter this
// comment used to cite was removed - it made widths non-additive, which
// retail's are not).
// Horizontal text only; falls back to the native path if no usable OS/2 table.
// Vertical text stays on the native path, because since Firefox 157 that
// path is no longer host-engine: fontMetrics is GetMetrics(fontOrientation),
// and for a vertical run that is gfxFont::CreateVerticalMetrics, built from
// the font's own OS/2, hhea and vhea tables plus one term taken from the
// horizontal metrics (internalLeading, split over ascent and descent).
// That term is host-independent for a bundled face because
// gfxFont::SanitizeMetrics imposes the manifest values on the horizontal
// metrics of every backend. The centre-baseline adjustment on the anchor is
// gone. Substituting the horizontal OS/2 values there would report numbers
// a real Firefox 157 does not. (On 151 vertical
// runs read the horizontal DWrite/FreeType metrics, and this condition had
// to cover them: measured 2026-08-08, 0.46px apart between Windows and
// Linux.)
inline UniquePtr<TextMetrics> StealthHostIndepTextMetrics(
    const RefPtr<gfxFont>& aFont, nsFontMetrics::FontOrientation aOrientation,
    CanvasTextBaseline aTextBaseline, float aTotalWidth,
    const StealthInkBox& aInk) {
  MOZ_ASSERT(aFont);
  gfxFloat emA, emD, maxA, maxD;
  // Same short-circuit order as the original condition: engine, orientation,
  // then the font table read.
  if (!mozilla::gfx::StealthEngineActive() ||
      aOrientation != nsFontMetrics::eHorizontal ||
      !aFont->GetStealthHostIndepVMetrics(emA, emD, maxA, maxD)) {
    return nullptr;
  }
  // Finish the job gfxDWriteFonts::ComputeMetrics does, which this branch
  // bypasses. GetStealthHostIndepVMetrics hands back
  // usWinAscent/usWinDescent scaled to px - the same quantity DWrite calls
  // fontMetrics.ascent/descent - but RAW, while a real Firefox rounds them
  // (gfxDWriteFonts.cpp:396-397) and then derives the em box from the rounded
  // pair (:400-403). Skipping that is why every TextMetrics vertical field
  // came back fractional here and whole in retail; the correction is to put
  // the missing steps back, not to round again somewhere downstream.
  //
  // This is the single lie a public fingerprinting test page reports
  // against this build. Its test is not about width and not about
  // additivity despite the name it prints ("metric noise"): it measures the
  // EMPTY string and flags any of
  // actualBoundingBox{Ascent,Descent,Left,Right} or
  // fontBoundingBox{Ascent,Descent} that is not whole (`const isFloat = (n)
  // => n % 1 !== 0`, read from the vendored source after an earlier guess
  // at additivity failed to clear it).
  //
  // Measured 2026-08-08, Arial, retail Firefox 152 against this build:
  //
  //     72px  fontBoundingBoxAscent   retail 65   ours 65.1796875
  //     20px  fontBoundingBoxAscent   retail 18   ours 18.10546875
  //     12px  fontBoundingBoxAscent   retail 11   ours 10.86328125
  //     10px  fontBoundingBoxDescent  retail  2   ours  2.119140625
  //
  // Rounding reproduces retail on all ten measurements (five sizes x ascent
  // and descent), which it must: it is literally the operation at :396.
  // The shipped firefox-18 returns the same fractional values, so this is
  // not a regression - it is a rounding we never did, present since
  // bundle-only shipped.
  maxA = std::round(maxA);
  maxD = std::round(maxD);
  const gfxFloat maxH = maxA + maxD;
  if (maxH > 0.0) {
    // gfxDWriteFonts.cpp:400-403. Note this is NOT sTypoAscender: DWrite
    // splits the em box in the ratio of the ROUNDED usWin pair, so emAscent
    // stays fractional in retail too (72px Arial: 72 * 65/80 = 58.5) and
    // must stay fractional here. Rounding it would be a new divergence, not
    // a fix - that page never looks at emHeightAscent.
    const gfxFloat emH = aFont->GetAdjustedSize();
    emA = emH * maxA / maxH;
    emD = emH - emA;
  }
  // Host-independent baseline anchor. This MIRRORS the baselineAnchor
  // switch in CanvasRenderingContext2D::DrawOrMeasureText and has to mirror
  // it case for case, including which cases
  // call GetBaseline: Hanging and Ideographic are synthesized by HarfBuzz
  // there, not derived from the em box. Approximating them as emA * 0.8 and
  // -emD is what this used to do, and it was wrong on every field that
  // subtracts the anchor, not just on the two baselines it names. Measured
  // 2026-08-08 over the full finite domain of measureText (6 textBaseline x
  // 5 textAlign x 2 direction x 3 fonts x 2 sizes x 3 strings = 1080 cases,
  // 12960 fields): 2520 divergences from retail traced to this switch, and
  // ZERO of them are visible at the alphabetic baseline, where the anchor is
  // 0 and the approximation cancels. A sweep of the sizes alone could not
  // have found it.
  gfxFloat hiAnchor;
  switch (aTextBaseline) {
    case CanvasTextBaseline::Hanging:
      hiAnchor = aFont->GetBaseline(gfxFont::kHanging, aOrientation);
      break;
    case CanvasTextBaseline::Top:
      hiAnchor = emA;
      break;
    case CanvasTextBaseline::Middle:
      hiAnchor = (emA - emD) * 0.5;
      break;
    case CanvasTextBaseline::Alphabetic:
      hiAnchor = aFont->GetBaseline(gfxFont::kAlphabetic, aOrientation);
      break;
    case CanvasTextBaseline::Ideographic:
      hiAnchor = aFont->GetBaseline(gfxFont::kIdeographicUnder, aOrientation);
      break;
    case CanvasTextBaseline::Bottom:
      hiAnchor = -emD;
      break;
    default:
      hiAnchor = 0.0;
      break;
  }
  // No ink, no ink box - and "no box" means what retail computes from an
  // empty one, NOT zero. The em-box substitution is what makes the actual
  // bounding box host-independent for text that draws something; applying
  // it to text that draws nothing invents a box a real Firefox does not
  // report. But retail's two values there are -mBoundingBox.Y() - anchor
  // and .YMost() + anchor with an EMPTY box, which is -anchor and +anchor,
  // and that is zero only when the anchor is zero, i.e. only at the
  // alphabetic baseline. A hardcoded 0.0 was measured against exactly that
  // baseline and agreed with retail by cancellation: at textBaseline
  // "bottom", 16px Arial, retail answers 2.8235294117647065 and the
  // hardcode answered 0.
  //
  // Left and Right are a different story again: they read X and XMost, not
  // emptiness, so retail reports them non-zero even with no ink (at 10px,
  // "   " gives -5.566666666666666 and +5.566666666666666). They stay on
  // the substituted box, anchored to offsetX the way retail anchors them,
  // so they move with textAlign. Before this they were the constants 0 and
  // totalWidth, which did not move with textAlign at all - a missing
  // dependency reads worse than a wrong number, because a page can find it
  // by changing one property and seeing nothing happen.
  //
  // Whether a string has ink is a property of the bundled font, not of the
  // host engine, so none of this costs host-independence.
  //
  // AND THE INK BOX IS THE REAL ONE AGAIN, since 2026-08-09. The four
  // actualBoundingBox* fields used to be replaced by the ADVANCE box
  // (offsetX and totalWidth - offsetX) and the EM box, on the stated
  // grounds that the ink box was host-dependent. That was asserted, never
  // measured, and it cost more than it could possibly buy:
  //
  //   * it made `actualBoundingBoxLeft + actualBoundingBoxRight == width`
  //     hold EXACTLY, for every string, font and size. No real Firefox
  //     produces that - ink is narrower than the advance - so it is a
  //     one-line detector check that needs no reference values, no second
  //     machine and no knowledge of our fonts;
  //   * with the default textAlign it made actualBoundingBoxLeft exactly
  //     0.0, where stock Firefox 151 answers -0.796875 for Arial 16px;
  //   * measured against stock Firefox 151 on the same machine over the
  //     whole finite domain, 1980 of 12960 fields differed and ALL 1980
  //     were these four. Every other field - width, the font box, the em
  //     box, all three baselines - already matched exactly.
  //
  // The ink box comes from glyph outlines in fonts we bundle, and the
  // outlines are byte-identical on both hosts (verified glyph by glyph for
  // the probe string: 0 of 15 differ between the system Arial 7.06 and the
  // bundled 7.04). So there was nothing host-dependent to protect against,
  // and the cross-OS diff after this change is what says so rather than
  // this comment.
  return MakeUnique<TextMetrics>(
      aTotalWidth,
      aInk.mLeft,  // the real ink box, not the advance
      aInk.mRight,
      maxA - hiAnchor,  // fontBoundingBoxAscent
      maxD + hiAnchor,  // fontBoundingBoxDescent
      aInk.mAscent, aInk.mDescent,
      emA - hiAnchor,  // emHeightAscent
      emD + hiAnchor,  // emHeightDescent
      // The hanging and ideographic baselines come from HarfBuzz's own
      // fallback synthesis, which is what the ordinary path in
      // CanvasRenderingContext2D::DrawOrMeasureText calls and therefore what
      // retail reports. This branch approximated them as
      // emA * 0.8 and -emD, and both were wrong at every size measured:
      // 72px Arial gives hanging 46.800000000000004 our way against
      // retail's 43.19999694824219, and ideographic -13.5 against
      // -15.2578125. The values are 16.16 fixed point (position / 65536.0),
      // which is where retail's .9999 tails come from. HarfBuzz reads the
      // bundled font's tables and GetAdjustedSize, so nothing host-specific
      // enters - and the cross-OS diff after this change is what says so,
      // not this comment.
      aFont->GetBaseline(gfxFont::kHanging, aOrientation) - hiAnchor,
      aFont->GetBaseline(gfxFont::kAlphabetic, aOrientation) - hiAnchor,
      aFont->GetBaseline(gfxFont::kIdeographicUnder, aOrientation) - hiAnchor);
}

}  // namespace mozilla::dom

#endif  // DOM_CANVAS_STEALTHCANVASTEXTMETRICS_H_
