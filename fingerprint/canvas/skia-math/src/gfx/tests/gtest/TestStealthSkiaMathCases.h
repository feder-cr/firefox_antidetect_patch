/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The 2D canvas battery of the measurement canvas-blend/skia-cpu-dispatch-m149 of the
// study repo invisible_skia_cpu (driver.cpp, copied as it is from its helpers to the end
// of build(): 182 deterministic SkCanvas cases on a 256x256 kBGRA_8888 premul bitmap, the
// bitmap of DrawTargetSkia B8G8R8A8). The hashes TestStealthSkiaMath.cpp compares are the
// ones that driver printed. Included by that file only.

#ifndef TEST_STEALTH_SKIA_MATH_CASES_H_
#define TEST_STEALTH_SKIA_MATH_CASES_H_

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

#include "skia/include/core/SkBitmap.h"
#include "skia/include/core/SkBlurTypes.h"
#include "skia/include/core/SkCanvas.h"
#include "skia/include/core/SkImage.h"
#include "skia/include/core/SkImageInfo.h"
#include "skia/include/core/SkMaskFilter.h"
#include "skia/include/core/SkPaint.h"
#include "skia/include/core/SkPath.h"
#include "skia/include/core/SkPathBuilder.h"
#include "skia/include/core/SkSamplingOptions.h"
#include "skia/include/core/SkSurface.h"
#include "skia/include/effects/SkGradient.h"
#include "skia/include/effects/SkImageFilters.h"

namespace StealthSkiaMathCases {

// ---------------------------------------------------------------- helpers
static const int W = 256;
static uint32_t gSeed;
static uint32_t rnd() { gSeed = gSeed * 1664525u + 1013904223u; return gSeed >> 8; }

static uint8_t mulDiv255Round(unsigned a, unsigned b) { unsigned p = a * b + 128; return (p + (p >> 8)) >> 8; }
static uint32_t premulBGRA(unsigned a, unsigned r, unsigned g, unsigned b) {
  return (uint32_t)a << 24 | (uint32_t)mulDiv255Round(r, a) << 16 | (uint32_t)mulDiv255Round(g, a) << 8 | mulDiv255Round(b, a);
}
// Deterministic non-trivial backdrop: quadrants of opaque, random alpha, half alpha ramp, transparent+noise.
static void fillBackdrop(SkBitmap& bm, int kind) {
  gSeed = 12345u + kind;
  for (int y = 0; y < W; y++) {
    uint32_t* row = (uint32_t*)bm.getAddr32(0, y);
    for (int x = 0; x < W; x++) {
      unsigned r = rnd() & 255, g = rnd() & 255, b = rnd() & 255, a;
      if (kind == 0) { a = 255; }
      else if (kind == 1) { a = (x < 128 && y < 128) ? 255 : (x >= 128 && y < 128) ? (rnd() & 255) : (x < 128) ? (x * 2 & 255) : ((x + y) & 1 ? 0 : 128); }
      else { a = 0; r = g = b = 0; }
      if (kind == 3) { r = x; g = y; b = (x ^ y) & 255; a = 255; }
      row[x] = premulBGRA(a, r, g, b);
    }
  }
}
static sk_sp<SkImage> makeImage(int n, SkAlphaType at, uint32_t seed, bool a8 = false) {
  gSeed = seed;
  if (a8) {
    SkBitmap bm; bm.allocPixels(SkImageInfo::MakeA8(n, n));
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) *bm.getAddr8(x, y) = rnd() & 255;
    bm.setImmutable(); return bm.asImage();
  }
  SkBitmap bm; bm.allocPixels(SkImageInfo::Make(n, n, kBGRA_8888_SkColorType, at));
  for (int y = 0; y < n; y++) {
    uint32_t* row = (uint32_t*)bm.getAddr32(0, y);
    for (int x = 0; x < n; x++) {
      unsigned r = rnd() & 255, g = rnd() & 255, b = rnd() & 255, a = rnd() & 255;
      if ((rnd() & 15) == 0) {
        a = 255;
      }
      if ((rnd() & 31) == 0) {
        a = 0;
      }
      row[x] = at == kPremul_SkAlphaType ? premulBGRA(a, r, g, b) : ((uint32_t)a << 24 | r << 16 | g << 8 | b);
    }
  }
  bm.setImmutable(); return bm.asImage();
}

using Draw = std::function<void(SkCanvas*)>;
struct Case { std::string name; int backdrop; Draw draw; };
static std::vector<Case> gCases;
static void add(const std::string& name, int backdrop, Draw d) { gCases.push_back({name, backdrop, d}); }

static SkPaint P(SkColor c, SkBlendMode m = SkBlendMode::kSrcOver, bool aa = true) { SkPaint p; p.setColor(c); p.setBlendMode(m); p.setAntiAlias(aa); return p; }
static const SkRect kR = {20, 20, 236, 236};
static const SkRect kRf = {20.3f, 20.6f, 235.45f, 235.8f};

// The cosine and sine of the five angles -pi/2 + i * 4pi/5 as glibc's cosf and sinf give them
// (the driver called them): bits, so that the star is the same path on every libm.
static float bitsToFloat(uint32_t u) { float f; memcpy(&f, &u, sizeof f); return f; }
static const uint32_t kStarCosSin[5][2] = {
  {0xb33bbd2e, 0xbf800000}, {0x3f167918, 0x3f4f1bbd}, {0xbf737871, 0xbe9e3779}, {0x3f737871, 0xbe9e3778}, {0xbf167913, 0x3f4f1bc0}};
static SkPath star(float cx, float cy, float r) {
  SkPathBuilder b; for (int i = 0; i < 5; i++) { float x = cx + r * bitsToFloat(kStarCosSin[i][0]), y = cy + r * bitsToFloat(kStarCosSin[i][1]); if (i == 0) b.moveTo(x, y); else b.lineTo(x, y); }
  b.close(); return b.detach();
}
static SkPath blob(float cx, float cy, float r) {
  SkPathBuilder b; b.moveTo(cx - r, cy); b.cubicTo(cx - r, cy - r * 1.3f, cx + r, cy - r * 0.7f, cx + r, cy); b.quadTo(cx + r * 0.5f, cy + r * 1.2f, cx, cy + r * 0.6f); b.cubicTo(cx - r * .4f, cy + r * 1.3f, cx - r, cy + r * .8f, cx - r, cy); b.close(); return b.detach();
}

static const SkBlendMode kModes[] = {
  SkBlendMode::kClear, SkBlendMode::kSrc, SkBlendMode::kDst, SkBlendMode::kSrcOver, SkBlendMode::kDstOver, SkBlendMode::kSrcIn,
  SkBlendMode::kDstIn, SkBlendMode::kSrcOut, SkBlendMode::kDstOut, SkBlendMode::kSrcATop, SkBlendMode::kDstATop, SkBlendMode::kXor,
  SkBlendMode::kPlus, SkBlendMode::kModulate, SkBlendMode::kScreen, SkBlendMode::kOverlay, SkBlendMode::kDarken, SkBlendMode::kLighten,
  SkBlendMode::kColorDodge, SkBlendMode::kColorBurn, SkBlendMode::kHardLight, SkBlendMode::kSoftLight, SkBlendMode::kDifference,
  SkBlendMode::kExclusion, SkBlendMode::kMultiply, SkBlendMode::kHue, SkBlendMode::kSaturation, SkBlendMode::kColor, SkBlendMode::kLuminosity};
static const char* kModeNames[] = {"clear","src","dst","srcover","dstover","srcin","dstin","srcout","dstout","srcatop","dstatop","xor","plus","modulate","screen","overlay","darken","lighten","colordodge","colorburn","hardlight","softlight","difference","exclusion","multiply","hue","saturation","color","luminosity"};

static sk_sp<SkShader> lin(SkPoint a, SkPoint b, std::vector<SkColor4f> cs, std::vector<float> pos, SkTileMode tm = SkTileMode::kClamp, bool premul = false, const SkMatrix* m = nullptr) {
  SkPoint pts[2] = {a, b};
  return SkShaders::LinearGradient(pts, SkGradient(SkGradient::Colors(SkSpan(cs.data(), cs.size()), SkSpan(pos.data(), pos.size()), tm),
      SkGradient::Interpolation{premul ? SkGradient::Interpolation::InPremul::kYes : SkGradient::Interpolation::InPremul::kNo}), m);
}
static SkColor4f C4(SkColor c) { return SkColor4f::FromColor(c); }

static void build() {
  // ---- fills, source-over with partial alpha (legacy blitter) ----
  const int alphas[] = {1, 16, 32, 64, 100, 128, 170, 200, 240, 254, 255};
  for (int bd : {0, 1}) for (int a : alphas) {
    add("fill.srcover.a" + std::to_string(a) + ".bd" + std::to_string(bd), bd, [=](SkCanvas* c) {
      c->drawRect(kR, P(SkColorSetARGB(a, 200, 60, 20))); c->drawRect({40, 40, 100, 100}, P(SkColorSetARGB(a, 30, 220, 130)));
    });
  }
  add("fill.srcover.aarect", 1, [](SkCanvas* c) { c->drawRect(kRf, P(SkColorSetARGB(77, 10, 200, 90))); });
  add("fill.srcover.aarect_noaa", 1, [](SkCanvas* c) { c->drawRect(kRf, P(SkColorSetARGB(77, 10, 200, 90), SkBlendMode::kSrcOver, false)); });
  add("fill.srcover.paintalpha", 1, [](SkCanvas* c) { SkPaint p = P(SkColorSetARGB(255, 10, 200, 90)); p.setAlpha(131); c->drawRect(kR, p); });
  add("fill.srcover.color4f", 1, [](SkCanvas* c) { SkPaint p; p.setColor4f({0.31f, 0.62f, 0.93f, 0.37f}); p.setAntiAlias(true); c->drawRect(kR, p); });
  add("fill.srcover.transparent_bd", 2, [](SkCanvas* c) { c->drawRect(kR, P(SkColorSetARGB(77, 255, 128, 3))); });
  add("fill.srcover.overlapping6", 0, [](SkCanvas* c) { for (int i = 0; i < 6; i++) c->drawRect({10.f + i * 30, 10.f + i * 20, 130.f + i * 20, 130.f + i * 30}, P(SkColorSetARGB(60 + i * 20, 255 - i * 40, 30 * i, 90 + i * 25))); });
  // ---- blend modes: solid colour, semi-transparent, over varied backdrop ----
  for (int i = 0; i < 29; i++) {
    SkBlendMode m = kModes[i]; std::string n = kModeNames[i];
    add("blend.solid." + n, 1, [=](SkCanvas* c) { c->drawRect(kR, P(SkColorSetARGB(150, 220, 90, 40), m)); c->drawRect({60, 60, 150, 200.5f}, P(SkColorSetARGB(200, 30, 140, 230), m)); });
  }
  for (int i : {3, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 12, 11}) {
    SkBlendMode m = kModes[i]; std::string n = kModeNames[i];
    add("blend.lingrad." + n, 1, [=](SkCanvas* c) { SkPaint p = P(0xFF000000, m); p.setShader(lin({20, 20}, {236, 236}, {C4(0xCCFF3010), C4(0x80207AFF), C4(0xF0FFFF40)}, {0, .5f, 1})); c->drawRect(kR, p); });
  }
  for (int i : {3, 14, 15, 20, 21, 24, 25, 28}) {
    SkBlendMode m = kModes[i]; std::string n = kModeNames[i];
    add("blend.image." + n, 1, [=](SkCanvas* c) {
      auto im = makeImage(33, kPremul_SkAlphaType, 99); SkPaint p = P(0xFF000000, m);
      c->drawImageRect(im, SkRect::MakeWH(33, 33), {30, 30, 230, 230}, SkSamplingOptions(SkFilterMode::kLinear), &p, SkCanvas::kStrict_SrcRectConstraint);
    });
  }
  add("blend.bd0.multiply.aa", 0, [](SkCanvas* c) { c->drawPath(star(128, 128, 100), P(SkColorSetARGB(160, 255, 80, 20), SkBlendMode::kMultiply)); });
  add("blend.bd0.softlight.aa", 0, [](SkCanvas* c) { c->drawPath(star(128, 128, 100), P(SkColorSetARGB(160, 255, 80, 20), SkBlendMode::kSoftLight)); });
  add("blend.bd0.hue.aa", 0, [](SkCanvas* c) { c->drawPath(star(128, 128, 100), P(SkColorSetARGB(160, 255, 80, 20), SkBlendMode::kHue)); });
  add("blend.bd3.colordodge.aa", 3, [](SkCanvas* c) { c->drawPath(blob(128, 128, 90), P(SkColorSetARGB(150, 255, 80, 20), SkBlendMode::kColorDodge)); });
  // ---- gradients ----
  add("grad.lin.2stop.opaque", 0, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 0}, {236, 0}, {C4(0xFF2040E0), C4(0xFFF0D010)}, {0, 1})); c->drawRect(kR, p); });
  add("grad.lin.2stop.opaque.diag", 2, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 200}, {C4(0xFF2040E0), C4(0xFFF0D010)}, {0, 1})); c->drawRect(kR, p); });
  add("grad.lin.2stop.alpha", 1, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 100}, {C4(0x802040E0), C4(0xF0F0D010)}, {0, 1})); c->drawRect(kR, p); });
  add("grad.lin.2stop.alpha.premul", 1, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 100}, {C4(0x802040E0), C4(0xF0F0D010)}, {0, 1}, SkTileMode::kClamp, true)); c->drawRect(kR, p); });
  add("grad.lin.2stop.toTransparent", 0, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 20}, {C4(0xFFFF0000), C4(0x00FF0000)}, {0, 1})); c->drawRect(kR, p); });
  add("grad.lin.2stop.toTransparent.premul", 0, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 20}, {C4(0xFFFF0000), C4(0x0000FF00)}, {0, 1}, SkTileMode::kClamp, true)); c->drawRect(kR, p); });
  add("grad.lin.2stop.paintalpha", 1, [](SkCanvas* c) { SkPaint p = P(0); p.setAlpha(120); p.setShader(lin({20, 20}, {236, 236}, {C4(0xFF2040E0), C4(0xFFF0D010)}, {0, 1})); c->drawRect(kR, p); });
  add("grad.lin.2stop.aarect", 1, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 236}, {C4(0xFF2040E0), C4(0xFFF0D010)}, {0, 1})); c->drawRect(kRf, p); });
  add("grad.lin.2stop.aapath", 1, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 236}, {C4(0xC02040E0), C4(0xFFF0D010)}, {0, 1})); c->drawPath(star(128, 128, 105), p); });
  add("grad.lin.2stop.dither", 0, [](SkCanvas* c) { SkPaint p = P(0); p.setDither(true); p.setShader(lin({20, 0}, {236, 0}, {C4(0xFF303030), C4(0xFF404040)}, {0, 1})); c->drawRect(kR, p); });
  add("grad.lin.2stop.gray.subtle", 0, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 0}, {236, 0}, {C4(0xFF303030), C4(0xFF404040)}, {0, 1})); c->drawRect(kR, p); });
  add("grad.lin.3stop", 1, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 90}, {C4(0xFFFF0000), C4(0xA000FF00), C4(0xFF0000FF)}, {0, .4f, 1})); c->drawRect(kR, p); });
  add("grad.lin.5stop", 1, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 100}, {C4(0xFFFF0000), C4(0xFFFFFF00), C4(0x8000FF00), C4(0xF000FFFF), C4(0xFF0000FF)}, {0, .25f, .5f, .75f, 1})); c->drawRect(kR, p); });
  add("grad.lin.5stop.premul", 1, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 100}, {C4(0xFFFF0000), C4(0xFFFFFF00), C4(0x8000FF00), C4(0xF000FFFF), C4(0xFF0000FF)}, {0, .25f, .5f, .75f, 1}, SkTileMode::kClamp, true)); c->drawRect(kR, p); });
  for (auto tm : {SkTileMode::kRepeat, SkTileMode::kMirror, SkTileMode::kDecal})
    add("grad.lin.tile" + std::to_string((int)tm), 1, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({80, 80}, {140, 110}, {C4(0xFFFF2010), C4(0xB020F0A0)}, {0, 1}, tm)); c->drawRect(kR, p); });
  add("grad.lin.rotated.matrix", 1, [](SkCanvas* c) { SkMatrix m = SkMatrix::RotateDeg(33, {128, 128}); SkPaint p = P(0); p.setShader(lin({60, 60}, {200, 60}, {C4(0xFF2040E0), C4(0xA0F0D010), C4(0xFF30C060)}, {0, .5f, 1}, SkTileMode::kClamp, false, &m)); c->drawRect(kR, p); });
  auto radial = [](SkPoint c0, float r0, SkPoint c1, float r1, std::vector<SkColor4f> cs, std::vector<float> pos, SkTileMode tm, bool premul) {
    return SkShaders::TwoPointConicalGradient(c0, r0, c1, r1, SkGradient(SkGradient::Colors(SkSpan(cs.data(), cs.size()), SkSpan(pos.data(), pos.size()), tm),
        SkGradient::Interpolation{premul ? SkGradient::Interpolation::InPremul::kYes : SkGradient::Interpolation::InPremul::kNo}));
  };
  add("grad.rad.2stop", 1, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(radial({128, 128}, 0, {128, 128}, 110, {C4(0xFFFFFF00), C4(0xFF2020D0)}, {0, 1}, SkTileMode::kClamp, false)); c->drawRect(kR, p); });
  add("grad.rad.2stop.alpha", 1, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(radial({128, 128}, 0, {128, 128}, 110, {C4(0xF0FFFF00), C4(0x302020D0)}, {0, 1}, SkTileMode::kClamp, false)); c->drawRect(kR, p); });
  add("grad.rad.2stop.premul", 1, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(radial({128, 128}, 0, {128, 128}, 110, {C4(0xF0FFFF00), C4(0x302020D0)}, {0, 1}, SkTileMode::kClamp, true)); c->drawRect(kR, p); });
  add("grad.rad.3stop.offcenter", 1, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(radial({100, 100}, 10, {140, 150}, 100, {C4(0xFFFF0000), C4(0x9000FF00), C4(0xFF0000FF)}, {0, .5f, 1}, SkTileMode::kClamp, false)); c->drawRect(kR, p); });
  add("grad.rad.focal.mirror", 1, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(radial({90, 100}, 0, {150, 140}, 60, {C4(0xFFFF8000), C4(0xC0008080)}, {0, 1}, SkTileMode::kMirror, false)); c->drawRect(kR, p); });
  add("grad.rad.twocircle.repeat", 0, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(radial({90, 100}, 20, {160, 140}, 50, {C4(0xFFFF8000), C4(0xC0008080), C4(0xFF8020F0)}, {0, .6f, 1}, SkTileMode::kRepeat, false)); c->drawRect(kR, p); });
  auto sweep = [](std::vector<SkColor4f> cs, std::vector<float> pos, float a0, float a1, SkTileMode tm, bool premul) {
    return SkShaders::SweepGradient({128, 128}, a0, a1, SkGradient(SkGradient::Colors(SkSpan(cs.data(), cs.size()), SkSpan(pos.data(), pos.size()), tm),
        SkGradient::Interpolation{premul ? SkGradient::Interpolation::InPremul::kYes : SkGradient::Interpolation::InPremul::kNo}));
  };
  add("grad.conic.2stop", 1, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(sweep({C4(0xFFFF0000), C4(0xFF0000FF)}, {0, 1}, 0, 360, SkTileMode::kClamp, false)); c->drawRect(kR, p); });
  add("grad.conic.5stop.alpha", 1, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(sweep({C4(0xFFFF0000), C4(0xC0FFFF00), C4(0x8000FF00), C4(0xF000FFFF), C4(0xFFFF00FF)}, {0, .25f, .5f, .75f, 1}, 0, 360, SkTileMode::kClamp, false)); c->drawRect(kR, p); });
  add("grad.conic.partial.premul", 1, [=](SkCanvas* c) { SkPaint p = P(0); p.setShader(sweep({C4(0xFFFF0000), C4(0x300000FF)}, {0, 1}, 45, 270, SkTileMode::kClamp, true)); c->drawRect(kR, p); });
  add("grad.lin.5stop.multiply", 1, [](SkCanvas* c) { SkPaint p = P(0, SkBlendMode::kMultiply); p.setShader(lin({20, 20}, {236, 100}, {C4(0xFFFF0000), C4(0xFFFFFF00), C4(0x8000FF00), C4(0xF000FFFF), C4(0xFF0000FF)}, {0, .25f, .5f, .75f, 1})); c->drawRect(kR, p); });
  // ---- images ----
  auto img = [](bool unpremul = false) { return makeImage(33, unpremul ? kUnpremul_SkAlphaType : kPremul_SkAlphaType, 4242); };
  struct S { const char* n; SkSamplingOptions s; };
  const S samplers[] = {{"nearest", SkSamplingOptions(SkFilterMode::kNearest)}, {"bilinear", SkSamplingOptions(SkFilterMode::kLinear)},
      {"mitchell", SkSamplingOptions(SkCubicResampler::Mitchell())}, {"catmull", SkSamplingOptions(SkCubicResampler::CatmullRom())},
      {"mip", SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kLinear)}};
  for (auto& s : samplers) {
    SkSamplingOptions so = s.s;
    add(std::string("image.scale2.7.") + s.n, 1, [=](SkCanvas* c) { auto im = img(); SkPaint p = P(0xFF000000); c->drawImageRect(im, SkRect::MakeWH(33, 33), {20, 20, 20 + 33 * 2.7f, 20 + 33 * 2.7f}, so, &p, SkCanvas::kStrict_SrcRectConstraint); });
    add(std::string("image.scale0.4.") + s.n, 1, [=](SkCanvas* c) { auto im = img(); SkPaint p = P(0xFF000000); c->drawImageRect(im, SkRect::MakeWH(33, 33), {20.5f, 20.25f, 20.5f + 33 * .4f, 20.25f + 33 * .4f}, so, &p, SkCanvas::kStrict_SrcRectConstraint); });
    add(std::string("image.rot30.") + s.n, 1, [=](SkCanvas* c) { auto im = img(); SkPaint p = P(0xFF000000); c->save(); c->translate(128, 128); c->rotate(30); c->scale(3.1f, 3.1f); c->drawImage(im, -16.5f, -16.5f, so, &p); c->restore(); });
    add(std::string("image.frac.paintalpha.") + s.n, 1, [=](SkCanvas* c) { auto im = img(); SkPaint p = P(0xFF000000); p.setAlpha(139); c->drawImageRect(im, SkRect::MakeWH(33, 33), {20.37f, 21.61f, 20.37f + 99.f, 21.61f + 99.f}, so, &p, SkCanvas::kStrict_SrcRectConstraint); });
  }
  add("image.unpremul.bilinear", 1, [=](SkCanvas* c) { auto im = img(true); SkPaint p = P(0xFF000000); c->drawImageRect(im, SkRect::MakeWH(33, 33), {20, 20, 20 + 33 * 2.7f, 20 + 33 * 2.7f}, SkSamplingOptions(SkFilterMode::kLinear), &p, SkCanvas::kStrict_SrcRectConstraint); });
  add("image.unit.1to1.nearest", 1, [=](SkCanvas* c) { auto im = img(); SkPaint p = P(0xFF000000); c->drawImage(im, 40, 40, SkSamplingOptions(), &p); });
  add("image.unit.1to1.paintalpha", 1, [=](SkCanvas* c) { auto im = img(); SkPaint p = P(0xFF000000); p.setAlpha(100); c->drawImage(im, 40.f, 40.f, SkSamplingOptions(), &p); });
  add("image.srcrect.sub", 1, [=](SkCanvas* c) { auto im = img(); SkPaint p = P(0xFF000000); c->drawImageRect(im, {4.25f, 3.5f, 20.75f, 25.5f}, {30, 30, 220, 200}, SkSamplingOptions(SkFilterMode::kLinear), &p, SkCanvas::kStrict_SrcRectConstraint); });
  add("image.shader.repeat.rot", 1, [=](SkCanvas* c) { auto im = img(); SkPaint p = P(0xFF000000); SkMatrix m = SkMatrix::RotateDeg(17); m.postScale(2.2f, 2.2f); p.setShader(im->makeShader(SkTileMode::kRepeat, SkTileMode::kMirror, SkSamplingOptions(SkFilterMode::kLinear), &m)); c->drawRect(kR, p); });
  add("image.shader.clamp.bicubic", 1, [=](SkCanvas* c) { auto im = img(); SkPaint p = P(0xFF000000); SkMatrix m = SkMatrix::Scale(3.3f, 3.3f); m.postTranslate(40, 40); p.setShader(im->makeShader(SkTileMode::kClamp, SkTileMode::kClamp, SkSamplingOptions(SkCubicResampler::Mitchell()), &m)); c->drawRect(kR, p); });
  add("image.a8.color.bilinear", 1, [=](SkCanvas* c) { auto im = makeImage(33, kPremul_SkAlphaType, 77, true); SkPaint p = P(SkColorSetARGB(200, 240, 30, 100)); c->drawImageRect(im, SkRect::MakeWH(33, 33), {20, 20, 20 + 33 * 5.1f, 20 + 33 * 5.1f}, SkSamplingOptions(SkFilterMode::kLinear), &p, SkCanvas::kStrict_SrcRectConstraint); });
  add("image.a8.color.nearest.1to1", 1, [=](SkCanvas* c) { auto im = makeImage(33, kPremul_SkAlphaType, 77, true); SkPaint p = P(SkColorSetARGB(200, 240, 30, 100)); c->drawImage(im, 50, 60, SkSamplingOptions(), &p); });
  // ---- A8 masks: paths (AA coverage), strokes, blur mask filters, clips, layers ----
  add("mask.path.star.opaque", 1, [](SkCanvas* c) { c->drawPath(star(128, 128, 110), P(0xFF3060F0)); });
  add("mask.path.star.alpha", 1, [](SkCanvas* c) { c->drawPath(star(128, 128, 110), P(0x903060F0)); });
  add("mask.path.blob.alpha", 1, [](SkCanvas* c) { c->drawPath(blob(128, 128, 80), P(0x70F06030)); });
  add("mask.path.circle.alpha", 0, [](SkCanvas* c) { c->drawCircle(128.3f, 127.7f, 90.2f, P(0xB0206080)); });
  add("mask.path.stroke.wide", 1, [](SkCanvas* c) { SkPaint p = P(0xA0F0A020); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(13.3f); p.setStrokeJoin(SkPaint::kRound_Join); c->drawPath(star(128, 128, 100), p); });
  add("mask.path.stroke.hair", 1, [](SkCanvas* c) { SkPaint p = P(0xFF102030); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(0); c->drawPath(star(128, 128, 100), p); });
  add("mask.path.stroke.thin", 1, [](SkCanvas* c) { SkPaint p = P(0xC0102030); p.setStyle(SkPaint::kStroke_Style); p.setStrokeWidth(1.0f); c->drawPath(blob(128, 128, 100), p); });
  for (auto st : {kNormal_SkBlurStyle, kSolid_SkBlurStyle, kOuter_SkBlurStyle, kInner_SkBlurStyle}) for (float sg : {2.0f, 6.5f, 14.f}) {
    add("mask.blur.style" + std::to_string((int)st) + ".sigma" + std::to_string((int)(sg * 10)), 1, [=](SkCanvas* c) { SkPaint p = P(0xC0A02080); p.setMaskFilter(SkMaskFilter::MakeBlur(st, sg)); c->drawPath(star(128, 128, 80), p); });
  }
  add("mask.blur.rect.opaque", 0, [](SkCanvas* c) { SkPaint p = P(0xFF204080); p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 5.f)); c->drawRect({60, 60, 200, 190}, p); });
  add("mask.blur.rect.gradient", 1, [](SkCanvas* c) { SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 236}, {C4(0xFFFF0000), C4(0x800000FF)}, {0, 1})); p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 7.f)); c->drawRect({60, 60, 200, 190}, p); });
  add("mask.blur.multiply", 1, [](SkCanvas* c) { SkPaint p = P(0xFFA03030, SkBlendMode::kMultiply); p.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 9.f)); c->drawCircle(128, 128, 70, p); });
  add("imagefilter.blur.sigma4", 1, [](SkCanvas* c) { SkPaint p = P(0xFFE04020); p.setImageFilter(SkImageFilters::Blur(4, 4, nullptr)); c->drawPath(star(128, 128, 90), p); });
  add("imagefilter.blur.sigma9.alpha", 1, [](SkCanvas* c) { SkPaint p = P(0x90E04020); p.setImageFilter(SkImageFilters::Blur(9, 3, nullptr)); c->drawPath(blob(128, 128, 90), p); });
  add("shadow.manual.maskblur", 1, [](SkCanvas* c) { SkPaint sp = P(SkColorSetARGB(160, 0, 0, 0)); sp.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 5.f)); c->drawPath(star(118, 120, 70), sp); c->drawPath(star(110, 110, 70), P(0xFF2080E0)); });
  add("shadow.manual.maskblur.alpha", 0, [](SkCanvas* c) { SkPaint sp = P(SkColorSetARGB(200, 120, 0, 40)); sp.setMaskFilter(SkMaskFilter::MakeBlur(kNormal_SkBlurStyle, 8.f)); c->drawCircle(122, 122, 60, sp); c->drawCircle(110, 110, 60, P(0x802080E0)); });
  add("shadow.manual.imageblur", 1, [](SkCanvas* c) { SkPaint sp = P(SkColorSetARGB(160, 0, 0, 0)); sp.setImageFilter(SkImageFilters::Blur(5, 5, nullptr)); c->drawPath(star(118, 120, 70), sp); c->drawPath(star(110, 110, 70), P(0xFF2080E0)); });
  add("clip.aa.circle.fill", 1, [](SkCanvas* c) { c->save(); c->clipPath(SkPath::Circle(128, 128, 100), SkClipOp::kIntersect, true); c->drawRect(kR, P(SkColorSetARGB(180, 220, 40, 90))); c->restore(); });
  add("clip.aa.circle.gradient", 1, [](SkCanvas* c) { c->save(); c->clipPath(SkPath::Circle(128, 128, 100), SkClipOp::kIntersect, true); SkPaint p = P(0); p.setShader(lin({20, 20}, {236, 236}, {C4(0xFFFF0000), C4(0xB00000FF)}, {0, 1})); c->drawRect(kR, p); c->restore(); });
  add("clip.aa.rectfrac", 1, [](SkCanvas* c) { c->save(); c->clipRect(kRf, SkClipOp::kIntersect, true); c->drawRect({0, 0, 256, 256}, P(SkColorSetARGB(180, 220, 40, 90))); c->restore(); });
  add("clip.aa.star.multiply", 1, [](SkCanvas* c) { c->save(); c->clipPath(star(128, 128, 110), SkClipOp::kIntersect, true); c->drawRect(kR, P(SkColorSetARGB(200, 90, 180, 40), SkBlendMode::kMultiply)); c->restore(); });
  add("clip.aa.nested", 1, [](SkCanvas* c) { c->save(); c->clipPath(SkPath::Circle(110, 128, 90), SkClipOp::kIntersect, true); c->clipPath(SkPath::Circle(150, 128, 90), SkClipOp::kIntersect, true); c->drawRect(kR, P(SkColorSetARGB(200, 40, 90, 200))); c->restore(); });
  add("layer.alpha.srcover", 1, [](SkCanvas* c) { SkPaint lp; lp.setAlpha(140); c->saveLayer(nullptr, &lp); c->drawRect({30, 30, 160, 160}, P(0xFFE02010)); c->drawRect({90, 90, 220, 220}, P(0xFF1020E0)); c->restore(); });
  add("layer.alpha.nonsrcover", 1, [](SkCanvas* c) { SkPaint lp; lp.setAlpha(140); lp.setBlendMode(SkBlendMode::kMultiply); c->saveLayer(nullptr, &lp); c->drawRect({30, 30, 160, 160}, P(0xFFE02010)); c->drawRect({90, 90, 220, 220}, P(0xFF1020E0)); c->restore(); });
  add("layer.alpha.screen.aa", 1, [](SkCanvas* c) { SkPaint lp; lp.setAlpha(200); lp.setBlendMode(SkBlendMode::kScreen); c->saveLayer(nullptr, &lp); c->drawPath(star(128, 128, 100), P(0xB0E02010)); c->restore(); });
  add("layer.mozilla.group.xor", 1, [](SkCanvas* c) { SkPaint lp; lp.setBlendMode(SkBlendMode::kXor); lp.setAlpha(170); c->saveLayer(SkCanvas::SaveLayerRec(nullptr, &lp, SkCanvas::kPreserveLCDText_SaveLayerFlag)); c->drawRect(kR, P(0xFF30A040)); c->restore(); });
  add("layer.srcin.rect", 1, [](SkCanvas* c) { SkPaint lp; lp.setBlendMode(SkBlendMode::kSrcIn); lp.setAlpha(170); c->saveLayer(nullptr, &lp); c->drawRect(kR, P(0xFF30A040)); c->restore(); });
  // ---- everything colour-managed-free but through the other surface kind ----
  add("draw.srcover.paintalpha.on.layer.bd2", 2, [](SkCanvas* c) { c->drawRect({0, 0, 256, 256}, P(0x80FFFFFF)); c->drawPath(star(128, 128, 100), P(0x60FF0000)); });
}

}  // namespace StealthSkiaMathCases

#endif  // TEST_STEALTH_SKIA_MATH_CASES_H_
