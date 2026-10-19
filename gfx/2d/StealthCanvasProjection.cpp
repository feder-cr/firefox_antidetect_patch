/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/gfx/StealthCanvasProjection.h"

#include <string.h>

#include "mozilla/gfx/2D.h"
#include "nsTArray.h"

namespace mozilla {
namespace gfx {

namespace {

// Where R, G, B and A sit in a pixel of each supported format, and whether the
// fourth byte is a real alpha.
struct Layout {
  int r, g, b, a;
  bool hasAlpha;
};

bool LayoutOf(SurfaceFormat aFormat, Layout* aOut) {
  switch (aFormat) {
    case SurfaceFormat::B8G8R8A8:
      *aOut = {2, 1, 0, 3, true};
      return true;
    case SurfaceFormat::B8G8R8X8:
      *aOut = {2, 1, 0, 3, false};
      return true;
    case SurfaceFormat::R8G8B8A8:
      *aOut = {0, 1, 2, 3, true};
      return true;
    case SurfaceFormat::R8G8B8X8:
      *aOut = {0, 1, 2, 3, false};
      return true;
    default:
      return false;
  }
}

inline uint32_t Mix(uint32_t h) {
  h ^= h >> 16;
  h *= 0x85ebca6bu;
  h ^= h >> 13;
  h *= 0xc2b2ae35u;
  h ^= h >> 16;
  return h;
}

// A pixel as the projection sees it, in ONE canonical order whatever the
// storage: R G B A from the high byte down, the low bit of R, G and B cleared.
inline uint32_t Canonical(const uint8_t* aPx, const Layout& aL) {
  const uint32_t a = aL.hasAlpha ? aPx[aL.a] : 255u;
  return (uint32_t(aPx[aL.r] & 0xfe) << 24) |
         (uint32_t(aPx[aL.g] & 0xfe) << 16) |
         (uint32_t(aPx[aL.b] & 0xfe) << 8) | a;
}

}  // namespace

bool StealthProjectPixels(const uint8_t* aSrc, int32_t aSrcStride,
                          const IntSize& aSrcSize, SurfaceFormat aFormat,
                          const IntRect& aRect, uint8_t* aDst,
                          int32_t aDstStride, bool aPremultiplied,
                          int32_t aSeed) {
  Layout l;
  if (aSeed == 0 || !aSrc || !aDst || !LayoutOf(aFormat, &l)) {
    return false;
  }
  const IntRect rect = aRect.Intersect(IntRect(IntPoint(), aSrcSize));
  if (rect.IsEmpty()) {
    return false;
  }
  const int32_t w = aSrcSize.width;
  const int32_t h = aSrcSize.height;
  const uint32_t seed = Mix(uint32_t(aSeed) ^ 0x2545f491u);

  // Three rows of canonical values, each padded by one pixel on both sides
  // with the clamped neighbour, so the inner loop has no bounds tests.
  const int32_t x0 = rect.X();
  const int32_t rw = rect.Width();
  AutoTArray<uint32_t, 3 * 258> rows;
  rows.SetLength(3 * size_t(rw + 2));
  auto fill = [&](uint32_t* aRow, int32_t aY) {
    const int32_t y = aY < 0 ? 0 : (aY >= h ? h - 1 : aY);
    const uint8_t* line = aSrc + ptrdiff_t(y) * aSrcStride;
    for (int32_t i = 0; i < rw + 2; ++i) {
      int32_t x = x0 - 1 + i;
      x = x < 0 ? 0 : (x >= w ? w - 1 : x);
      aRow[i] = Canonical(line + 4 * x, l);
    }
  };
  uint32_t* up = rows.Elements();
  uint32_t* mid = up + (rw + 2);
  uint32_t* down = mid + (rw + 2);
  fill(up, rect.Y() - 1);
  fill(mid, rect.Y());

  for (int32_t y = rect.Y(); y < rect.YMost(); ++y) {
    fill(down, y + 1);
    uint8_t* out = aDst + ptrdiff_t(y - aRect.Y()) * aDstStride +
                   4 * ptrdiff_t(x0 - aRect.X());
    for (int32_t i = 1; i <= rw; ++i, out += 4) {
      const uint32_t c = mid[i];
      const uint32_t alpha = c & 0xffu;
      if (alpha == 0) {
        continue;
      }
      // An edge: a 4-neighbour differs. A flat area is left exactly as drawn.
      if (c == up[i] && c == down[i] && c == mid[i - 1] && c == mid[i + 1]) {
        continue;
      }
      // The 3x3 neighbourhood through a SUM: independent of row order and of
      // which neighbour is which, so a flipped or re-laid-out copy of the same
      // pixels gets the same key.
      const uint32_t around = Mix(up[i - 1]) + Mix(up[i]) + Mix(up[i + 1]) +
                              Mix(mid[i - 1]) + Mix(mid[i + 1]) +
                              Mix(down[i - 1]) + Mix(down[i]) +
                              Mix(down[i + 1]);
      const uint32_t key =
          Mix(seed ^ Mix(c * 0x9e3779b1u + 0x632be5abu) ^ (around * 0x27d4eb2fu));
      uint8_t bit = uint8_t(key >> 31);
      const uint8_t r0 = uint8_t(c >> 24);
      const uint8_t g0 = uint8_t(c >> 16);
      const uint8_t b0 = uint8_t(c >> 8);
      if (bit && aPremultiplied && l.hasAlpha &&
          ((r0 | 1u) > alpha || (g0 | 1u) > alpha || (b0 | 1u) > alpha)) {
        bit = 0;  // a premultiplied channel never exceeds its alpha
      }
      out[l.r] = uint8_t(r0 | bit);
      out[l.g] = uint8_t(g0 | bit);
      out[l.b] = uint8_t(b0 | bit);
    }
    uint32_t* t = up;
    up = mid;
    mid = down;
    down = t;
  }
  return true;
}

already_AddRefed<DataSourceSurface> StealthProjectedRegion(
    DataSourceSurface* aSource, const IntRect& aRect, bool aPremultiplied,
    int32_t aSeed) {
  if (!aSource || aRect.IsEmpty()) {
    return nullptr;
  }
  const SurfaceFormat format = aSource->GetFormat();
  RefPtr<DataSourceSurface> out =
      Factory::CreateDataSourceSurface(aRect.Size(), format);
  if (!out) {
    return nullptr;
  }
  DataSourceSurface::ScopedMap src(aSource, DataSourceSurface::READ);
  DataSourceSurface::ScopedMap dst(out, DataSourceSurface::WRITE);
  if (!src.IsMapped() || !dst.IsMapped()) {
    return nullptr;
  }
  const IntSize size = aSource->GetSize();
  const IntRect inside = aRect.Intersect(IntRect(IntPoint(), size));
  // The plain copy first, so every pixel the projection leaves alone (a flat
  // area, alpha 0, a part of aRect outside the surface) is the drawn one.
  const int32_t bpp = BytesPerPixel(format);
  for (int32_t y = inside.Y(); y < inside.YMost(); ++y) {
    memcpy(dst.GetData() + ptrdiff_t(y - aRect.Y()) * dst.GetStride() +
               bpp * ptrdiff_t(inside.X() - aRect.X()),
           src.GetData() + ptrdiff_t(y) * src.GetStride() +
               bpp * ptrdiff_t(inside.X()),
           size_t(inside.Width()) * bpp);
  }
  StealthProjectPixels(src.GetData(), src.GetStride(), size, format, aRect,
                       dst.GetData(), dst.GetStride(), aPremultiplied, aSeed);
  return out.forget();
}

}  // namespace gfx
}  // namespace mozilla
