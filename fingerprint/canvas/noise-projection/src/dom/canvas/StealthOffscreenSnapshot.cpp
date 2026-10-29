/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The page-facing readers of an OffscreenCanvas and of the
// transferControlToOffscreen placeholder (feature
// fingerprint/canvas/noise-projection).
//
// Part 1, OffscreenCanvas::GetSurfaceSnapshot (hook in OffscreenCanvas.cpp):
// CanvasUtils::StealthOffscreenSnapshot.
//
// Part 2, the contract of the two readers of OffscreenCanvasDisplayHelper:
//
// GetSurfaceSnapshot()          the placeholder's pixels as a page reads them
//                               (with the canvas noise). It is what a page
//                               reaches: toDataURL/toBlob, drawImage,
//                               createImageBitmap.
// GetSurfaceSnapshotAsDrawn()   the placeholder's pixels as drawn: for the
//                               browser's own readers, which show the canvas
//                               and never hand its bytes to a page (the screen
//                               through GetAsImage, -moz-element, the drag
//                               image, the taskbar preview).
//
// What a page reads of the placeholder is the worker's front buffer, through
// the canvas noise only for a WebGL/WebGPU context, the same rule the worker's
// own readers follow, so the two sides of a transferControlToOffscreen read one
// image. GetSurfaceSnapshotAsDrawn (-> TransformSurface) hands back
// premultiplied or opaque pixels, hence aPremultiplied is true below.
// GetAsImage (the screen's copy, nsHTMLCanvasFrame) reads the as-drawn surface,
// like every canvas on screen.

#include "StealthOffscreenSnapshot.h"

#include "CanvasUtils.h"
#include "OffscreenCanvasDisplayHelper.h"
#include "gfxTypes.h"
#include "mozilla/Assertions.h"
#include "mozilla/Mutex.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/HTMLCanvasElement.h"
#include "mozilla/dom/OffscreenCanvas.h"
#include "mozilla/gfx/2D.h"
#include "nsCOMPtr.h"
#include "nsICanvasRenderingContextInternal.h"
#include "nsIGlobalObject.h"
#include "nsIPrincipal.h"

namespace mozilla::CanvasUtils {

already_AddRefed<gfx::SourceSurface> StealthOffscreenSnapshot(
    dom::OffscreenCanvas* aCanvas, gfxAlphaType* aOutAlphaType) {
  MOZ_ASSERT(aCanvas);
  nsICanvasRenderingContextInternal* const context = aCanvas->GetContext();
  if (!context) {
    // The caller has already returned for this; kept so the helper stands on
    // its own.
    return nullptr;
  }

  // An OffscreenCanvas as a page reads it: every caller here is a page drawing
  // with it or reading it (HTMLCanvasElement's twin). The canvas noise applies
  // to a WebGL/WebGPU context only; a 2D one reads as drawn, as on retail.
  // The context is asked into a local because aOutAlphaType may be null and the
  // alpha type is needed below either way; the caller's pointer gets it back.
  gfxAlphaType alphaType = gfxAlphaType::Premult;
  RefPtr<gfx::SourceSurface> drawn =
      context->GetSurfaceSnapshot(aOutAlphaType ? &alphaType : nullptr);
  if (aOutAlphaType) {
    *aOutAlphaType = alphaType;
  }

  // The context type is read after the snapshot, as before the move.
  if (!StealthProjectsContextType(aCanvas->GetContextType())) {
    return drawn.forget();
  }

  nsIGlobalObject* const global = aCanvas->GetParentObject();
  return StealthProjectSnapshot(drawn, alphaType != gfxAlphaType::NonPremult,
                                global ? global->PrincipalOrNull() : nullptr);
}

}  // namespace mozilla::CanvasUtils

namespace mozilla::dom {

already_AddRefed<gfx::SourceSurface>
OffscreenCanvasDisplayHelper::StealthPageSnapshot() {
  RefPtr<gfx::SourceSurface> drawn = GetSurfaceSnapshotAsDrawn();
  nsCOMPtr<nsIPrincipal> principal;
  {
    MutexAutoLock lock(mMutex);
    if (!CanvasUtils::StealthProjectsContextType(mType)) {
      return drawn.forget();
    }
    if (mCanvasElement) {
      principal = mCanvasElement->NodePrincipal();
    }
  }
  return CanvasUtils::StealthProjectSnapshot(drawn, true, principal);
}

}  // namespace mozilla::dom
