/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/gfx/StealthGlyphCoverage.h"

#include <stdlib.h>

#include "mozilla/StaticPrefs_zoom.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla {
namespace gfx {

void StealthQuantiseGlyphCoverage(uint8_t* aImage, int32_t aWidth,
                                  int32_t aHeight, size_t aRowBytes) {
  if (!aImage || aWidth <= 0 || aHeight <= 0) {
    return;
  }

  // DataMutexString: take the lock, copy out, drop it. Same shape as
  // SpeechSynthesis.cpp does for zoom.stealth.voices.list.
  nsAutoCString spec;
  {
    auto lock = StaticPrefs::zoom_stealth_text_coverage_ladder();
    spec = *lock;
  }
  if (spec.IsEmpty()) {
    return;  // not configured: leave the rasteriser's own coverage alone
  }

  AutoTArray<uint8_t, 32> ladder;
  for (const nsACString& part : spec.Split(',')) {
    nsresult rv;
    const int32_t v = PromiseFlatCString(part).ToInteger(&rv);
    if (NS_FAILED(rv) || v < 0 || v > 255) {
      return;  // malformed -> no snap at all, never a partial one
    }
    ladder.AppendElement(uint8_t(v));
  }
  if (ladder.Length() < 2) {
    return;
  }

  // 256-entry lookup, built on the stack per call and deliberately not cached.
  //
  // Two earlier versions of this code (in the canvas readback path it replaces)
  // were caches and both were wrong: a one-shot `static bool sMapReady`, correct
  // only while the ladder was a process constant, and then a pair of
  // function-local statics keyed on the pref string, which fixed the staleness
  // and introduced a DATA RACE - glyph rasterisation runs on the content main
  // thread, on WebRender's font thread and in the GPU process, and two of them
  // writing an unsynchronised static is undefined behaviour that would surface
  // as wrong pixels, rarely, and never reproducibly.
  //
  // The cache was also saving the wrong half of the work. This function runs on
  // a glyph-cache MISS, not per draw and not per pixel of the page: a few
  // hundred times for a page that uses a few hundred distinct glyph/size pairs,
  // against a per-pixel loop that is bounded by one glyph's box. 256 * 17
  // comparisons is not a cost worth a shared mutable byte.
  uint8_t map[256];
  for (uint32_t a = 0; a < 256; ++a) {
    uint8_t best = ladder[0];
    int32_t bestErr = 256;
    for (uint8_t lv : ladder) {
      const int32_t err = abs(int32_t(a) - int32_t(lv));
      if (err < bestErr) {
        bestErr = err;
        best = lv;
      }
    }
    map[a] = best;
  }

  for (int32_t y = 0; y < aHeight; ++y) {
    uint8_t* row = aImage + size_t(y) * aRowBytes;
    for (int32_t x = 0; x < aWidth; ++x) {
      row[x] = map[row[x]];
    }
  }
}

}  // namespace gfx
}  // namespace mozilla
