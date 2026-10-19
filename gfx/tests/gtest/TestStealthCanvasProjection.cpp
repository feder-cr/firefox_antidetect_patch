/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The properties gfx::StealthProjectPixels promises, each one a reason the
// canvas noise reads the same through every reader of a canvas
// (gfx/2d/StealthCanvasProjection.h). The browser-level proof is the canvas
// form B bench in the workbench; these pin the function itself.

#include <string.h>

#include <algorithm>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/StealthCanvasProjection.h"

using namespace mozilla;
using namespace mozilla::gfx;

namespace {

constexpr int32_t kW = 37;
constexpr int32_t kH = 23;
constexpr int32_t kSeed = 8;
// The tolerance invisible_core declares (zoom.stealth.canvas.edge_tolerance).
constexpr int32_t kTolerance = 4;

// A drawing with flat areas, hard edges, a ramp of greys (a blend of two
// colours, left alone) and, from row 18, independent colours (a third colour
// in every neighbourhood, the part the noise touches), as RGBA, opaque unless
// aAlpha.
std::vector<uint8_t> Drawing(bool aAlpha) {
  std::vector<uint8_t> px(kW * kH * 4);
  uint32_t state = 12345;
  for (int32_t y = 0; y < kH; ++y) {
    for (int32_t x = 0; x < kW; ++x) {
      uint8_t* p = &px[(y * kW + x) * 4];
      uint8_t r = 255, g = 255, b = 255;  // white background
      if (x > 5 && x < 20 && y > 3 && y < 12) {
        r = 30;  // a filled rectangle
        g = 120;
        b = 200;
      }
      if (y >= 14 && y < 18) {  // a ramp
        state = state * 1103515245u + 12345u;
        const uint8_t v = uint8_t((x * 7 + (state >> 28)) & 0xff);
        r = g = b = v;  // greys
      }
      if (y >= 18) {  // independent colours
        state = state * 1103515245u + 12345u;
        r = uint8_t(state >> 24);
        g = uint8_t(state >> 16);
        b = uint8_t(state >> 8);
      }
      uint8_t a = 255;
      if (aAlpha && x >= 25) {
        a = uint8_t(40 + x * 3);
        r = uint8_t(r * a / 255);  // premultiplied
        g = uint8_t(g * a / 255);
        b = uint8_t(b * a / 255);
      }
      if (aAlpha && x == kW - 1) {
        r = g = b = a = 0;  // transparent column
      }
      p[0] = r;
      p[1] = g;
      p[2] = b;
      p[3] = a;
    }
  }
  return px;
}

bool Project(std::vector<uint8_t>& aPx, SurfaceFormat aFormat,
             bool aPremult = true, int32_t aSeed = kSeed,
             int32_t aEdgeTolerance = kTolerance) {
  return StealthProjectPixels(aPx.data(), kW * 4, IntSize(kW, kH), aFormat,
                              IntRect(0, 0, kW, kH), aPx.data(), kW * 4,
                              aPremult,
                              StealthCanvasNoise{aSeed, aEdgeTolerance});
}

std::vector<uint8_t> SwapRB(const std::vector<uint8_t>& aPx) {
  std::vector<uint8_t> out = aPx;
  for (size_t i = 0; i < out.size(); i += 4) {
    std::swap(out[i], out[i + 2]);
  }
  return out;
}

std::vector<uint8_t> FlipY(const std::vector<uint8_t>& aPx) {
  std::vector<uint8_t> out(aPx.size());
  for (int32_t y = 0; y < kH; ++y) {
    memcpy(&out[(kH - 1 - y) * kW * 4], &aPx[y * kW * 4], kW * 4);
  }
  return out;
}

}  // namespace

TEST(StealthCanvasProjection, IsAProjection)
{
  std::vector<uint8_t> once = Drawing(true);
  ASSERT_TRUE(Project(once, SurfaceFormat::R8G8B8A8));
  std::vector<uint8_t> twice = once;
  ASSERT_TRUE(Project(twice, SurfaceFormat::R8G8B8A8));
  EXPECT_EQ(once, twice);
}

TEST(StealthCanvasProjection, TouchesEdgesOnly)
{
  const std::vector<uint8_t> drawn = Drawing(false);
  std::vector<uint8_t> noisy = drawn;
  ASSERT_TRUE(Project(noisy, SurfaceFormat::R8G8B8A8));
  EXPECT_NE(drawn, noisy) << "the noise must be there";
  // The white corner and the inside of the filled rectangle read as drawn.
  EXPECT_EQ(0, memcmp(&drawn[0], &noisy[0], 4));
  const size_t inside = (8 * kW + 12) * 4;
  EXPECT_EQ(0, memcmp(&drawn[inside], &noisy[inside], 4));
  // Only the low bit of R, G and B ever moves; alpha never.
  for (size_t i = 0; i < drawn.size(); ++i) {
    if (i % 4 == 3) {
      EXPECT_EQ(drawn[i], noisy[i]);
    } else {
      EXPECT_EQ(drawn[i] & 0xfe, noisy[i] & 0xfe);
    }
  }
}

TEST(StealthCanvasProjection, UniformCanvasIsLeftAlone)
{
  std::vector<uint8_t> white(kW * kH * 4, 255);
  const std::vector<uint8_t> drawn = white;
  Project(white, SurfaceFormat::R8G8B8A8);
  EXPECT_EQ(drawn, white) << "a white canvas reads 255 everywhere, corners too";
}

TEST(StealthCanvasProjection, SameBitsWhateverTheChannelOrder)
{
  std::vector<uint8_t> rgba = Drawing(true);
  std::vector<uint8_t> bgra = SwapRB(rgba);
  ASSERT_TRUE(Project(rgba, SurfaceFormat::R8G8B8A8));
  ASSERT_TRUE(Project(bgra, SurfaceFormat::B8G8R8A8));
  EXPECT_EQ(rgba, SwapRB(bgra));
}

TEST(StealthCanvasProjection, SameBitsWhateverTheRowOrder)
{
  std::vector<uint8_t> topDown = Drawing(true);
  std::vector<uint8_t> bottomUp = FlipY(topDown);
  ASSERT_TRUE(Project(topDown, SurfaceFormat::R8G8B8A8));
  ASSERT_TRUE(Project(bottomUp, SurfaceFormat::R8G8B8A8));
  EXPECT_EQ(topDown, FlipY(bottomUp));
}

TEST(StealthCanvasProjection, GreysStayGreyAndPremultipliedStaysValid)
{
  std::vector<uint8_t> px = Drawing(true);
  const std::vector<uint8_t> drawn = px;
  ASSERT_TRUE(Project(px, SurfaceFormat::R8G8B8A8));
  bool touched = false;
  for (size_t i = 0; i < px.size(); i += 4) {
    if (drawn[i] == drawn[i + 1] && drawn[i + 1] == drawn[i + 2]) {
      EXPECT_TRUE(px[i] == px[i + 1] && px[i + 1] == px[i + 2]);
    }
    // One bit on the three channels: every touched pixel moved R, G and B the
    // same way, which is what keeps a grey grey where a grey is touched.
    if (memcmp(&drawn[i], &px[i], 4) != 0) {
      touched = true;
      EXPECT_EQ(px[i] & 1, px[i + 1] & 1) << i;
      EXPECT_EQ(px[i + 1] & 1, px[i + 2] & 1) << i;
    }
    EXPECT_LE(px[i], px[i + 3]);
    EXPECT_LE(px[i + 1], px[i + 3]);
    EXPECT_LE(px[i + 2], px[i + 3]);
  }
  EXPECT_TRUE(touched) << "nothing was touched: the test proves nothing";
}

TEST(StealthCanvasProjection, SeedDecides)
{
  std::vector<uint8_t> a = Drawing(false);
  std::vector<uint8_t> b = a;
  std::vector<uint8_t> off = a;
  ASSERT_TRUE(Project(a, SurfaceFormat::R8G8B8A8, true, 8));
  ASSERT_TRUE(Project(b, SurfaceFormat::R8G8B8A8, true, 11));
  EXPECT_NE(a, b);
  EXPECT_FALSE(Project(off, SurfaceFormat::R8G8B8A8, true, 0));
  EXPECT_EQ(off, Drawing(false));
}

TEST(StealthCanvasProjection, ARectangleIsTheWholeCropped)
{
  const std::vector<uint8_t> drawn = Drawing(true);
  RefPtr<DataSourceSurface> surf = Factory::CreateDataSourceSurfaceWithStride(
      IntSize(kW, kH), SurfaceFormat::R8G8B8A8, kW * 4);
  ASSERT_TRUE(surf);
  {
    DataSourceSurface::ScopedMap map(surf, DataSourceSurface::WRITE);
    ASSERT_TRUE(map.IsMapped());
    for (int32_t y = 0; y < kH; ++y) {
      memcpy(map.GetData() + y * map.GetStride(), &drawn[y * kW * 4], kW * 4);
    }
  }
  std::vector<uint8_t> whole = drawn;
  ASSERT_TRUE(Project(whole, SurfaceFormat::R8G8B8A8));

  const IntRect rect(4, 2, 19, 15);  // its edges cut through the drawing
  RefPtr<DataSourceSurface> part =
      StealthProjectedRegion(surf, rect, true, StealthCanvasNoise{kSeed, kTolerance});
  ASSERT_TRUE(part);
  DataSourceSurface::ScopedMap map(part, DataSourceSurface::READ);
  ASSERT_TRUE(map.IsMapped());
  for (int32_t y = 0; y < rect.Height(); ++y) {
    EXPECT_EQ(0, memcmp(map.GetData() + y * map.GetStride(),
                        &whole[((rect.Y() + y) * kW + rect.X()) * 4],
                        rect.Width() * 4))
        << "row " << y;
  }
}

TEST(StealthCanvasProjection, ABlendOfTwoColoursIsLeftAlone)
{
  // Text at any contrast is the ink blended over the background: every pixel
  // on the segment between two colours. Coverage levels of a low-contrast
  // coloured ink (FingerprintJS's rgba(102,204,0,0.2) over white has steps of
  // 1-5 per channel), a black-on-white ramp, and the antialiased edge between
  // two fills: none of it may move, whatever the step between neighbours.
  // Each blend in a buffer of its own: two of them side by side would put a
  // third colour in the neighbourhoods along their border, which the noise
  // touches on purpose (two inks a pixel apart are three colours).
  auto blend = [](const uint8_t aFrom[3], const uint8_t aTo[3]) {
    std::vector<uint8_t> px(kW * kH * 4, 255);
    for (int32_t y = 0; y < kH; ++y) {
      for (int32_t x = 0; x < kW; ++x) {
        uint8_t* p = &px[(y * kW + x) * 4];
        const int32_t level = (x * 7 + y * 3) % 17;  // 17 coverage levels
        for (int k = 0; k < 3; ++k) {
          p[k] = uint8_t(aFrom[k] + (int32_t(aTo[k]) - aFrom[k]) * level / 16);
        }
      }
    }
    return px;
  };
  const uint8_t white[3] = {255, 255, 255};
  const uint8_t green02[3] = {224, 245, 204};  // the 0.2 green at full coverage
  const uint8_t black[3] = {0, 0, 0};
  const uint8_t red[3] = {200, 0, 0};
  const uint8_t blue[3] = {0, 0, 220};
  for (const auto& [from, to] : {std::pair{white, green02}, std::pair{white, black},
                                 std::pair{red, blue}}) {
    std::vector<uint8_t> px = blend(from, to);
    const std::vector<uint8_t> drawn = px;
    ASSERT_TRUE(Project(px, SurfaceFormat::R8G8B8A8));
    EXPECT_EQ(drawn, px) << int(to[0]) << "," << int(to[1]) << "," << int(to[2]);
  }
}

TEST(StealthCanvasProjection, AThirdColourIsTouched)
{
  // The independent colours of the test drawing: a third colour in every
  // neighbourhood. They carry the noise, and the two seeds disagree there.
  std::vector<uint8_t> a = Drawing(false);
  const std::vector<uint8_t> drawn = a;
  ASSERT_TRUE(Project(a, SurfaceFormat::R8G8B8A8));
  size_t moved = 0;
  for (int32_t y = 19; y < kH - 1; ++y) {
    for (int32_t x = 1; x < kW - 1; ++x) {
      const size_t i = (y * kW + x) * 4;
      moved += memcmp(&drawn[i], &a[i], 4) != 0;
    }
  }
  EXPECT_GT(moved, 0u);
}
