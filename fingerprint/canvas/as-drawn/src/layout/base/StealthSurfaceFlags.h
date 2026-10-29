/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StealthSurfaceFlags_h
#define StealthSurfaceFlags_h

#include <stdint.h>

// Our flag for nsLayoutUtils::SurfaceFromElement's aSurfaceFlags
// (fingerprint/canvas/as-drawn): the canvas as drawn, without the noise a
// page's readback gets. It is not in Mozilla's SFE_* enum because Mozilla
// appends its new flags there on the next free bit, and a flag of ours there
// would collide with one of Mozilla's in silence at a port (the merge sees two
// different lines). Bit 31 is far from Mozilla's (they end at bit 8 in 157 and
// 159); build/stealth_check_sources.py fails if an SFE_ flag takes it.
// STEALTH-FLAG-SPACE(layout/base/nsLayoutUtils.h, SFE_)
constexpr uint32_t kStealthSfeAsDrawn = 1u << 31;

// Who asks for it: nsImageRenderer::PrepareImage, because -moz-element SHOWS
// the element, like the screen, so a canvas is shown as drawn; and the
// chrome-only compareCanvases test hook (dom/base/StealthCanvasAsDrawn.h).

// nsLayoutUtils::SurfaceFromElement's snapshot of a canvas: as drawn when the
// caller set kStealthSfeAsDrawn, else what Mozilla takes. A template so that
// this header needs no canvas include.
template <class Canvas, class... Args>
auto StealthCanvasSnapshot(Canvas* aCanvas, uint32_t aSurfaceFlags,
                           Args... aArgs) {
  return (aSurfaceFlags & kStealthSfeAsDrawn)
             ? aCanvas->GetSurfaceSnapshotAsDrawn(aArgs...)
             : aCanvas->GetSurfaceSnapshot(aArgs...);
}

#endif  // StealthSurfaceFlags_h
