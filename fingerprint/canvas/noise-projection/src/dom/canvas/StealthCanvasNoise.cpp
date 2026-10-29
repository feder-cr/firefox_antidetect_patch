/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Definitions of the two canvas-noise entry points declared in CanvasUtils.h
// (their contract is documented below, at the definitions). Kept out of
// CanvasUtils.cpp so that file stays identical to Mozilla's.

#include "CanvasUtils.h"

#include "StealthDeclarationGate.h"
#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/RefPtr.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/Point.h"
#include "mozilla/gfx/Rect.h"
#include "mozilla/gfx/StealthCanvasProjection.h"
#include "nsIPrincipal.h"

namespace mozilla::CanvasUtils {

// The canvas noise (gfx::StealthProjectPixels) for a canvas owned by
// aCanvasPrincipal; a zero seed leaves its pixels as drawn. Zero for a canvas the
// browser itself owns - system, chrome or resource principal - because those
// are the automation's own canvases (a screenshot draws the page into one and
// reads it back) and never a page's: noise there buys nothing and corrupts the
// screenshot (wrapper issue #49). Decided by the canvas that HOLDS the pixels,
// never by who calls: the same canvas reads the same bytes for every reader.
gfx::StealthCanvasNoise StealthNoise(nsIPrincipal* aCanvasPrincipal) {
  // The browser's own canvases (system, chrome and resource principals) are
  // never noised: the automation draws a page into one and reads it back, and
  // noise there would corrupt the screenshot. The principal is the one of the
  // canvas that holds the pixels, so a page cannot opt out.
  if (aCanvasPrincipal && (aCanvasPrincipal->IsSystemPrincipal() ||
                           aCanvasPrincipal->SchemeIs("chrome") ||
                           aCanvasPrincipal->SchemeIs("resource"))) {
    return gfx::StealthCanvasNoise::Off();
  }
  return gfx::StealthCanvasNoiseDeclared();
}

// A snapshot of a WebGL or WebGPU canvas as a page reads it (a 2D canvas is
// never projected): the canvas noise applied to its
// stored pixels, a new data surface; the snapshot itself is left untouched,
// because it is also what the screen and -moz-element show. aSnapshot back
// unchanged when the seed is 0. aPremultiplied: the snapshot's alpha type is
// not NonPremult.
already_AddRefed<gfx::SourceSurface> StealthProjectSnapshot(
    gfx::SourceSurface* aSnapshot, bool aPremultiplied,
    nsIPrincipal* aCanvasPrincipal) {
  RefPtr<gfx::SourceSurface> result = aSnapshot;
  const gfx::StealthCanvasNoise noise = StealthNoise(aCanvasPrincipal);
  if (!aSnapshot || !noise) {
    return result.forget();
  }
  RefPtr<gfx::DataSourceSurface> data = aSnapshot->GetDataSurface();
  if (!data) {
    return result.forget();
  }
  RefPtr<gfx::DataSourceSurface> projected = gfx::StealthProjectedRegion(
      data, gfx::IntRect(gfx::IntPoint(), data->GetSize()), aPremultiplied,
      noise);
  if (projected) {
    result = projected;
  }
  return result.forget();
}

}  // namespace mozilla::CanvasUtils
