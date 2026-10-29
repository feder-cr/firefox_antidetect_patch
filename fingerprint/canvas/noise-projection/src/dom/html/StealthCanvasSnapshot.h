/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StealthCanvasSnapshot_h
#define mozilla_dom_StealthCanvasSnapshot_h

#include "gfxTypes.h"
#include "mozilla/AlreadyAddRefed.h"

namespace mozilla {
namespace gfx {
class DrawTarget;
class SourceSurface;
}  // namespace gfx

namespace dom {

class HTMLCanvasElement;

/**
 * HTMLCanvasElement::GetSurfaceSnapshot: the canvas as a PAGE reads it, with
 * the canvas noise (gfx/2d/StealthCanvasProjection.h) applied to a WebGL or
 * WebGPU canvas. A 2D or bitmaprenderer canvas, and a canvas whose noise is
 * off, come back exactly as drawn (GetSurfaceSnapshotAsDrawn). The only caller
 * is the hook in HTMLCanvasElement::GetSurfaceSnapshot.
 *
 * HTMLCanvasElement::GetSurfaceSnapshot is the canvas as a PAGE reads it, with
 * the canvas noise (gfx/2d/StealthCanvasProjection.h): every way a page turns
 * this canvas into bytes or into the source of another drawing comes through
 * it (or through GetSurfaceSnapshotAsync, declared after it in
 * HTMLCanvasElement.h, which a context may answer without calling it), so a
 * reader added in a future Firefox reads the same image as the others without
 * being taught to.
 *
 * HTMLCanvasElement::GetSurfaceSnapshotAsDrawn (declared in HTMLCanvasElement.h,
 * the body GetSurfaceSnapshot had before the hook) is the same snapshot as
 * DRAWN, for the browser's own readers that show the canvas and never hand its
 * bytes to a page: -moz-element, the drag image, the taskbar preview,
 * nsDOMWindowUtils. (Printing draws the canvas into a static clone that no page
 * can read, so it is left on the page road: a difference nothing can observe is
 * not one to add code for.)
 */
already_AddRefed<gfx::SourceSurface> StealthPageSnapshot(
    HTMLCanvasElement* aCanvas, gfxAlphaType* aOutAlphaType,
    gfx::DrawTarget* aTarget);

}  // namespace dom
}  // namespace mozilla

#endif  // mozilla_dom_StealthCanvasSnapshot_h
