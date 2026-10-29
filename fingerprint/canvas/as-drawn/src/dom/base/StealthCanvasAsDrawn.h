/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef StealthCanvasAsDrawn_h
#define StealthCanvasAsDrawn_h

#include "mozilla/SurfaceFromElementResult.h"
#include "mozilla/dom/HTMLCanvasElement.h"
#include "nsLayoutUtils.h"

// Chrome-only test hook (compareCanvases): the canvases as drawn.
inline mozilla::SurfaceFromElementResult StealthSurfaceFromCanvasAsDrawn(
    mozilla::dom::HTMLCanvasElement* aCanvas) {
  return nsLayoutUtils::SurfaceFromElement(aCanvas,
                                           nsLayoutUtils::SFE_AS_DRAWN);
}

#endif  // StealthCanvasAsDrawn_h
