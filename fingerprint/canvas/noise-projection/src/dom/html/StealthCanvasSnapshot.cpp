/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthCanvasSnapshot.h"

#include "CanvasUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/HTMLCanvasElement.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/gfx/DrawTargetRecording.h"
#include "mozilla/gfx/StealthCanvasProjection.h"

namespace mozilla::dom {

already_AddRefed<gfx::SourceSurface> StealthPageSnapshot(
    HTMLCanvasElement* aCanvas, gfxAlphaType* aOutAlphaType,
    gfx::DrawTarget* aTarget) {
  MOZ_ASSERT(aCanvas);
  gfxAlphaType alphaType = gfxAlphaType::Premult;
  RefPtr<gfx::SourceSurface> drawn = aCanvas->GetSurfaceSnapshotAsDrawn(
      aOutAlphaType ? &alphaType : nullptr, aTarget);
  if (aOutAlphaType) {
    *aOutAlphaType = alphaType;
  }
  if (!drawn) {
    return nullptr;
  }
  // A 2D or bitmaprenderer canvas is a source as it was drawn, as on retail:
  // the canvas noise is a WebGL/WebGPU readback only.
  if (!CanvasUtils::StealthProjectsContextType(
          aCanvas->GetCurrentContextType())) {
    return drawn.forget();
  }
  const bool premultiplied = alphaType != gfxAlphaType::NonPremult;
  // A canvas drawn onto a canvas that records for the GPU process stays on the
  // GPU: the projection is recorded and runs there, where the pixels are
  // (RecordedStealthProjectSurface). Reading it back here to project it would
  // cost a readback per drawImage that the real browser does not pay.
  // The test is on the underlying type, as in StealthProjectedSurface: since
  // 157 a canvas recording's snapshot answers CANVAS_RECORDING to GetType(),
  // and asking GetType() sent every such drawImage to the readback below.
  if (aTarget && aTarget->IsRecording() &&
      drawn->GetUnderlyingType() == gfx::SurfaceType::RECORDING) {
    const gfx::StealthCanvasNoise noise =
        CanvasUtils::StealthNoise(aCanvas->NodePrincipal());
    if (!noise) {
      return drawn.forget();
    }
    return static_cast<gfx::DrawTargetRecording*>(aTarget)
        ->StealthProjectedSurface(drawn, premultiplied, noise);
  }
  return CanvasUtils::StealthProjectSnapshot(drawn, premultiplied,
                                             aCanvas->NodePrincipal());
}

}  // namespace mozilla::dom
