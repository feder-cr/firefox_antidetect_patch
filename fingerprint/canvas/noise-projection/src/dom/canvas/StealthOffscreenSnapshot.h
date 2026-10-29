/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef STEALTHOFFSCREENSNAPSHOT_H_
#define STEALTHOFFSCREENSNAPSHOT_H_

#include "gfxTypes.h"  // gfxAlphaType
#include "mozilla/AlreadyAddRefed.h"

namespace mozilla {

namespace dom {
class OffscreenCanvas;
}  // namespace dom

namespace gfx {
class SourceSurface;
}  // namespace gfx

namespace CanvasUtils {

/**
 * An OffscreenCanvas as a page reads it. Called from
 * OffscreenCanvas::GetSurfaceSnapshot, whose callers today are all a page
 * drawing with the canvas or reading it (ImageBitmap, WebGL texture upload,
 * VideoFrame, WebGPU copyExternalImageToTexture, drawImage on a 2D context),
 * the OffscreenCanvas twin of HTMLCanvasElement::GetSurfaceSnapshot. This is a
 * fact about the callers of today, not something the code enforces: no caller
 * asks for the as-drawn variant here (nsLayoutUtils::SurfaceFromOffscreenCanvas
 * has no equivalent of the SFE_AS_DRAWN check that the HTMLCanvasElement path
 * has), so the day one does, the fix belongs in nsLayoutUtils.
 *
 * The snapshot is the context's, unchanged, for every context type the noise
 * does not apply to (see StealthProjectsContextType in CanvasUtils.h); for
 * WebGL and WebGPU it is StealthProjectSnapshot of it, with the alpha type the
 * context reported and the principal of the canvas's global.
 *
 * aOutAlphaType may be null. When it is not, it always receives the alpha type
 * the context reported (Premult if the context reported none, as when it has no
 * surface to give).
 */
already_AddRefed<gfx::SourceSurface> StealthOffscreenSnapshot(
    dom::OffscreenCanvas* aCanvas, gfxAlphaType* aOutAlphaType);

}  // namespace CanvasUtils
}  // namespace mozilla

#endif /* STEALTHOFFSCREENSNAPSHOT_H_ */
