/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_STEALTHCANVASPROJECTION_H_
#define MOZILLA_GFX_STEALTHCANVASPROJECTION_H_

#include <stdint.h>

#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/gfx/Point.h"
#include "mozilla/gfx/Rect.h"
#include "mozilla/gfx/Types.h"

namespace mozilla {
namespace gfx {

class DataSourceSurface;

/**
 * The canvas noise: ONE pure function of the seed and of what the canvas holds,
 * applied wherever a canvas becomes bytes for the page or the source of another
 * drawing ("form B", 29-canvas-buffer-unico.md in the workbench docs).
 *
 * WHY ONE FUNCTION. Until firefox-50 the noise was three algorithms bolted onto
 * five read METHODS (getImageData, toDataURL/toBlob, readPixels, the WebGL
 * snapshot), each on its own buffer layout: every other road - drawImage onto
 * another canvas, createImageBitmap, VideoFrame, WebGPU, the PBO, the
 * transferControlToOffscreen placeholder - read the real pixels, and two reads
 * of the same canvas disagreed. A page saw it by reading the canvas twice.
 * With a function that depends only on the pixels, every reader that applies it
 * to the same pixels gets the same bytes, and there is no state to keep in
 * step: the "one image per canvas state" is computed, not stored.
 *
 * WHAT IT DOES, per pixel with alpha > 0:
 *   - every value is looked at with the low bit of R, G and B CLEARED (alpha is
 *     never touched, so it is read whole);
 *   - the pixel is touched only if it differs from one of its four neighbours:
 *     an edge (antialiasing, a gradient step), never a flat area. A white
 *     background stays white and a filled rectangle reads back the colour it
 *     was filled with, which is what a real machine's variation looks like;
 *   - the low bit of R, G and B is replaced by ONE bit, the same on the three
 *     channels so a grey stays grey, taken from a hash of the seed and of the
 *     3x3 neighbourhood. The neighbours enter through a SUM, so the key does not
 *     depend on the order rows are stored in (WebGL is bottom-up) or on the
 *     channel order (the 2D canvas is BGRA, WebGL RGBA): the same pixels give
 *     the same bit in every layout;
 *   - on a premultiplied buffer the bit is 1 only if every channel stays <=
 *     alpha, decided from the cleared values, so the result is always a valid
 *     premultiplied pixel.
 * Because the mask and the key only read cleared bits, and the function only
 * writes those bits, it is a PROJECTION: applying it twice changes nothing.
 * That is what makes getImageData -> putImageData -> getImageData give the same
 * bytes, and drawImage of a projected canvas onto another give, read back, the
 * same bytes as reading the first.
 *
 * NEIGHBOURS OUTSIDE THE BUFFER are the edge pixel itself (clamp to edge), so a
 * uniform canvas is never an "edge" at its border: a white canvas reads 255 in
 * every corner. A rectangle read is the whole read cropped as long as the
 * caller hands over the rectangle grown by one pixel inside the canvas (see
 * StealthProjectedRegion).
 */

/**
 * The seed is not read here: callers pass StealthCanvasNoiseSeed()
 * (gfx/thebes/StealthDeclarationGate.h), which is where "is the engine on"
 * lives. This layer stays a function of its arguments.
 *
 * Applies the projection to the pixels of aRect, reading neighbours from aSrc
 * and writing into aDst, whose first byte is the pixel at aRect's top-left and
 * which must already hold a copy of those pixels: only the low bit of R, G and
 * B of the touched pixels is written. aDst may be aSrc itself (in place): the
 * function reads only the bits it does not write. Formats: B8G8R8A8, B8G8R8X8, R8G8B8A8, R8G8B8X8 (X reads as 255).
 * Returns false, writing nothing, for any other format, a zero seed or an
 * empty rectangle.
 */
bool StealthProjectPixels(const uint8_t* aSrc, int32_t aSrcStride,
                          const IntSize& aSrcSize, SurfaceFormat aFormat,
                          const IntRect& aRect, uint8_t* aDst,
                          int32_t aDstStride, bool aPremultiplied,
                          int32_t aSeed);

/**
 * A new surface of aRect's size holding aRect of aSource with the projection
 * applied, its neighbours read from aSource; aSource is left untouched. Null if
 * aSource cannot be mapped. With a zero seed it is a plain copy.
 */
already_AddRefed<DataSourceSurface> StealthProjectedRegion(
    DataSourceSurface* aSource, const IntRect& aRect, bool aPremultiplied,
    int32_t aSeed);

}  // namespace gfx
}  // namespace mozilla

#endif  // MOZILLA_GFX_STEALTHCANVASPROJECTION_H_
