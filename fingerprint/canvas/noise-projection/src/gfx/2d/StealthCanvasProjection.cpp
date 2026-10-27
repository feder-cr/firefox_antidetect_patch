/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/gfx/StealthCanvasProjection.h"

#include <string.h>

#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/StealthRefuse.h"
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

// The squared distance between two canonical values, in RGBA channel units.
inline int64_t Dist2(uint32_t a, uint32_t b) {
  int64_t s = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const int64_t d = int64_t((a >> shift) & 0xffu) - int64_t((b >> shift) & 0xffu);
    s += d * d;
  }
  return s;
}

// Do these colours all lie on ONE straight segment, between the two of them
// farthest apart, within aTol channel units? That is what a pixel looks like
// when it is a blend of two colours: text (the ink over the background at a
// coverage level, at ANY contrast), the antialiased edge between two fills, a
// two-stop gradient. A real machine differs from another there in WHICH blend
// a pixel gets, never by a third colour, and a touched pixel would be a level
// that rasteriser does not produce (an 18th grey in DirectWrite text).
//
// Integer arithmetic only, so the content process, the GPU process and a
// worker, on Windows and on Linux, decide every pixel the same way: a float
// compared with a threshold could split them. The farthest pair is the first
// found in a fixed order; the distance from a colour r to the segment p-q is
// tested as |r-p|^2 * L - t^2 <= tol^2 * L, with t = (r-p).(q-p) and L = |q-p|^2,
// clamped to the end points.
bool OnOneSegment(const uint32_t (&aColours)[9], int64_t aTol) {
  int64_t len = -1;
  int pi = 0;
  int qi = 0;
  for (int i = 0; i < 9; ++i) {
    for (int j = i + 1; j < 9; ++j) {
      const int64_t d = Dist2(aColours[i], aColours[j]);
      if (d > len) {
        len = d;
        pi = i;
        qi = j;
      }
    }
  }
  if (len <= 0) {
    return true;  // one colour
  }
  const uint32_t p = aColours[pi];
  const uint32_t q = aColours[qi];
  const int64_t tol2 = aTol * aTol;
  for (uint32_t r : aColours) {
    int64_t t = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      const int64_t w = int64_t((r >> shift) & 0xffu) - int64_t((p >> shift) & 0xffu);
      const int64_t v = int64_t((q >> shift) & 0xffu) - int64_t((p >> shift) & 0xffu);
      t += w * v;
    }
    bool near;
    if (t <= 0) {
      near = Dist2(r, p) <= tol2;
    } else if (t >= len) {
      near = Dist2(r, q) <= tol2;
    } else {
      near = Dist2(r, p) * len - t * t <= tol2 * len;
    }
    if (!near) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool StealthProjectPixels(const uint8_t* aSrc, int32_t aSrcStride,
                          const IntSize& aSrcSize, SurfaceFormat aFormat,
                          const IntRect& aRect, uint8_t* aDst,
                          int32_t aDstStride, bool aPremultiplied,
                          const StealthCanvasNoise& aNoise) {
  Layout l;
  if (!aNoise || !aSrc || !aDst || !LayoutOf(aFormat, &l)) {
    return false;
  }
  const IntRect rect = aRect.Intersect(IntRect(IntPoint(), aSrcSize));
  if (rect.IsEmpty()) {
    return false;
  }
  const int32_t w = aSrcSize.width;
  const int32_t h = aSrcSize.height;
  const uint32_t seed = Mix(uint32_t(aNoise.mSeed) ^ 0x2545f491u);
  // A seed without its tolerance is a declaration only half carried: refused,
  // not read as some built-in tolerance.
  if (aNoise.mEdgeTolerance < 0 || aNoise.mEdgeTolerance > 255) {
    StealthRefuse(
        "stealth: the canvas noise has a seed but no declared edge tolerance "
        "(zoom.stealth.canvas.edge_tolerance is %d, not 0-255)",
        int(aNoise.mEdgeTolerance));
  }
  const int64_t tolerance = aNoise.mEdgeTolerance;

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
      // And not a blend of TWO colours: the 3x3 neighbourhood must hold a third,
      // independent one (a shadow over a fill and the background, an emoji, the
      // junction of three shapes). Text at any contrast and the edge between two
      // fills are left exactly as drawn. This replaced a band on the step to the
      // neighbours (zoom.stealth.canvas.edge_step), which no step value could
      // make separate low-contrast coloured text from a gradient: a commercial
      // fingerprinting service's own rgba(102,204,0,0.2) line has the steps of
      // a gradient on retail too.
      const uint32_t around9[9] = {up[i - 1],  up[i],  up[i + 1],
                                   mid[i - 1], c,      mid[i + 1],
                                   down[i - 1], down[i], down[i + 1]};
      if (OnOneSegment(around9, tolerance)) {
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
      // The LOW bit of the key. db967d50 took the top one; a version with
      // zoom.stealth.canvas.edge_share chose WHICH edges to touch from the top
      // eight bits, and the bit written had to stay independent of that choice,
      // so it moved down. edge_share is gone, and both bits of Mix are uniform;
      // the low one stays because the measured builds of 2026-10-19 used it.
      uint8_t bit = uint8_t(key & 1u);
      const uint8_t r0 = uint8_t(c >> 24);
      const uint8_t g0 = uint8_t(c >> 16);
      const uint8_t b0 = uint8_t(c >> 8);
      if (bit && aPremultiplied && l.hasAlpha &&
          ((r0 | 1u) > alpha || (g0 | 1u) > alpha || (b0 | 1u) > alpha)) {
        bit = 0;  // a premultiplied channel never exceeds its alpha
      }
      // WHICH channels carry the bit: only one whose value differs from that
      // channel in all eight neighbours. Values that differ are two apart once
      // the low bit is cleared, so a bit of noise on either side cannot swap
      // or level them, and every difference between neighbours keeps the sign
      // the drawing gave it: no new local extremum in any direction. The bit
      // on all three channels at once turned a channel constant across a shape
      // (the 0 of a red-green WebGL gradient) into isolated 1s, and a slow
      // gradient into +-1 spikes, the high-frequency signature that a page
      // reading back its own render can count (test_fingerprint_surface.py of
      // the wrapper, 117 spikes against 0). A channel left out keeps the drawn
      // value, low bit included. Channels equal to each other decide together,
      // so a grey stays grey.
      const uint8_t ch[3] = {r0, g0, b0};
      bool apart[3];
      for (int k = 0; k < 3; ++k) {
        const int shift = 24 - 8 * k;
        apart[k] = true;
        for (int n = 0; n < 9 && apart[k]; ++n) {
          apart[k] = n == 4 || uint8_t(around9[n] >> shift) != ch[k];
        }
      }
      bool carry[3];
      for (int k = 0; k < 3; ++k) {
        carry[k] = apart[k];
        for (int j = 0; j < 3; ++j) {
          carry[k] = carry[k] && (ch[j] != ch[k] || apart[j]);
        }
      }
      if (carry[0]) {
        out[l.r] = uint8_t(r0 | bit);
      }
      if (carry[1]) {
        out[l.g] = uint8_t(g0 | bit);
      }
      if (carry[2]) {
        out[l.b] = uint8_t(b0 | bit);
      }
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
    const StealthCanvasNoise& aNoise) {
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
                       dst.GetData(), dst.GetStride(), aPremultiplied, aNoise);
  return out.forget();
}

}  // namespace gfx
}  // namespace mozilla
