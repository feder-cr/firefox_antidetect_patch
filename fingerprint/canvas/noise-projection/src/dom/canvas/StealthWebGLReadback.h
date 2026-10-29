/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_CANVAS_STEALTHWEBGLREADBACK_H_
#define DOM_CANVAS_STEALTHWEBGLREADBACK_H_

// The canvas noise (gfx::StealthProjectPixels) on every road by which a page
// reads the pixels of a WebGL canvas, called from hooks in
// ClientWebGLContext.cpp.
//
// Where the noise is NOT applied: ClientWebGLContext::GetSurfaceSnapshot. Its
// snapshot is also what the screen, -moz-element and the GPU-side copies are
// made from. A page reads it through HTMLCanvasElement::GetSurfaceSnapshot or
// through the readers below, which apply gfx::StealthProjectPixels once, on
// these stored pixels. The async snapshot (captureStream) is projected in
// DoReadPixelsAsync, through ProjectAsyncSnapshot.
//
// The function is a projection (applying it twice changes nothing), so the
// roads agree: readPixels into an array, readPixels into a PIXEL_PACK_BUFFER,
// toDataURL and toBlob, and captureStream frames give the same bytes for the
// same pixels.
//
// This is a header with inline definitions, included by ClientWebGLContext.cpp
// only: the PIXEL_PACK_BUFFER road sends commands with
// ClientWebGLContext::Run, whose Run_WithDestArgTypes is a template defined in
// that .cpp and can be instantiated nowhere else.

#include <cstdint>
#include <cstring>
#include <vector>

#include "CanvasUtils.h"
#include "ClientWebGLContext.h"
#include "HostWebGLContext.h"
#include "WebGLChild.h"
#include "WebGLMethodDispatcher.h"
#include "mozilla/AlreadyAddRefed.h"
#include "mozilla/Maybe.h"
#include "mozilla/RefPtr.h"
#include "mozilla/Span.h"
#include "mozilla/gfx/StealthCanvasProjection.h"

#define STEALTH_WEBGL_RPROC(_METHOD) \
  decltype(&HostWebGLContext::_METHOD), &HostWebGLContext::_METHOD

namespace mozilla {

// A friend of ClientWebGLContext (the declaration is in ClientWebGLContext.h):
// it reaches mNotLost, Run and DoReadPixels.
struct StealthWebGLReadback final {
  StealthWebGLReadback() = delete;

  // toDataURL/toBlob (GetImageBuffer and GetInputStream): the canvas noise, as
  // every reader of this canvas reads. aAlphaType is the alpha type
  // GetSurfaceSnapshot reported for aSnapshot. The snapshot itself is left
  // untouched; the result is a new surface, or aSnapshot again when the seed
  // is 0.
  static already_AddRefed<gfx::SourceSurface> ProjectReaderSnapshot(
      const ClientWebGLContext& aCtx, gfx::SourceSurface* aSnapshot,
      const gfxAlphaType aAlphaType) {
    return CanvasUtils::StealthProjectSnapshot(
        aSnapshot, aAlphaType != gfxAlphaType::NonPremult,
        aCtx.PrincipalOrNull());
  }

  // captureStream() is the only reader of the async snapshot
  // (DoReadPixelsAsync): its frames get the canvas noise like readPixels and
  // toDataURL. Projected there, once, on the stored pixels;
  // HTMLCanvasElement::GetSurfaceSnapshotAsync must not project this result
  // again (the same contract is stated where it is enforced, in
  // HTMLCanvasElement.cpp).
  //
  // aPremultiplied is the alpha type GetImageBuffer passes: only an alpha
  // context that is not premultiplied is NonPremult, that is
  // `!options.alpha || options.premultipliedAlpha`, computed when the read is
  // asked for.
  static RefPtr<gfx::SourceSurface> ProjectAsyncSnapshot(
      const ClientWebGLContext& aCtx, gfx::SourceSurface* aSurface,
      const bool aPremultiplied) {
    return CanvasUtils::StealthProjectSnapshot(aSurface, aPremultiplied,
                                               aCtx.PrincipalOrNull());
  }

  // readPixels into an array: the read itself (ClientWebGLContext::
  // DoReadPixels), then the noise on the bytes the page receives. Returns what
  // DoReadPixels returned.
  static bool ReadPixels(const ClientWebGLContext& aCtx,
                         const webgl::ReadPixelsDesc& aDesc,
                         const Span<uint8_t> aDest) {
    webgl::ReadPixelsResult written;
    if (!aCtx.DoReadPixels(aDesc, aDest, &written)) {
      return false;
    }
    ProjectReadPixels(aCtx, aDesc, aDest, written);
    return true;
  }

  // The canvas noise for a readPixels into a PIXEL_PACK_BUFFER, after the host
  // wrote it: reads the same pixels into memory with the buffer unbound,
  // applies the noise as readPixels into an array applies it, and writes them
  // back over the buffer row by row with BufferSubData, so the buffer holds
  // what a page's readPixels into an array would have given. Only RGBA/
  // UNSIGNED_BYTE, only with a seed, only with the bound PBO big enough for the
  // read (otherwise the host refused the read and wrote nothing). It costs two
  // more reads: a tight one, then the 1px-larger one inside ProjectReadPixels.
  static void ProjectReadPixelsPbo(const ClientWebGLContext& aCtx,
                                   const webgl::ReadPixelsDesc& aDesc,
                                   const WebGLsizeiptr aOffset) {
    const uint64_t offset = static_cast<uint64_t>(aOffset);
    if (aDesc.pi.format != LOCAL_GL_RGBA ||
        aDesc.pi.type != LOCAL_GL_UNSIGNED_BYTE || !aDesc.size.x ||
        !aDesc.size.y) {
      return;
    }
    if (!CanvasUtils::StealthNoise(aCtx.PrincipalOrNull())) {
      return;
    }
    const auto& bound = aCtx.State().mBoundBufferByTarget;
    const auto found = bound.find(LOCAL_GL_PIXEL_PACK_BUFFER);
    if (found == bound.end() || !found->second) {
      return;  // the host refused the read and raised the error
    }
    const RefPtr<WebGLBufferJS> pbo = found->second;
    // Where the page's packing put each row in the buffer. For UNSIGNED_BYTE
    // the alignment is in bytes.
    const auto& packing = aDesc.packState;
    const uint64_t rowPixels =
        packing.rowLength ? packing.rowLength : aDesc.size.x;
    const uint64_t align = packing.alignmentInTypeElems;
    const uint64_t stride = (rowPixels * 4 + align - 1) / align * align;
    const uint64_t needed = offset +
                            (packing.skipRows + aDesc.size.y - 1) * stride +
                            (packing.skipPixels + aDesc.size.x) * 4;
    const auto size =
        BufferParameter(aCtx, LOCAL_GL_PIXEL_PACK_BUFFER, LOCAL_GL_BUFFER_SIZE);
    if (!size || needed > uint64_t(*size)) {
      return;  // too small: the host refused the read and wrote nothing
    }
    // The same pixels read into memory, with the buffer unbound for the time of
    // the read, and the noise applied as readPixels into an array applies it.
    webgl::ReadPixelsDesc tight = aDesc;
    tight.packState = {};
    std::vector<uint8_t> bytes(size_t(aDesc.size.x) * aDesc.size.y * 4);
    aCtx.Run<STEALTH_WEBGL_RPROC(BindBuffer)>(LOCAL_GL_PIXEL_PACK_BUFFER, 0);
    webgl::ReadPixelsResult written;
    const bool read =
        aCtx.DoReadPixels(tight, Span<uint8_t>(bytes), &written);
    if (read) {
      ProjectReadPixels(aCtx, tight, Span<uint8_t>(bytes), written);
    }
    aCtx.Run<STEALTH_WEBGL_RPROC(BindBuffer)>(LOCAL_GL_PIXEL_PACK_BUFFER,
                                              pbo->mId);
    if (!read || written.subrect.IsEmpty()) {
      return;
    }
    // Over the bytes the host wrote, row by row, so the gaps a packing leaves
    // between rows stay what they were.
    const auto& sub = written.subrect;
    for (int32_t y = sub.Y(); y < sub.YMost(); ++y) {
      const uint64_t dst = offset + (packing.skipRows + uint64_t(y)) * stride +
                           (packing.skipPixels + uint64_t(sub.X())) * 4;
      const uint8_t* src =
          bytes.data() + size_t(y) * written.byteStride + size_t(sub.X()) * 4;
      aCtx.Run<STEALTH_WEBGL_RPROC(BufferSubData)>(
          LOCAL_GL_PIXEL_PACK_BUFFER, dst,
          Span<const uint8_t>(src, size_t(sub.Width()) * 4),
          /* unsynchronized */ false);
    }
  }

 private:
  // The canvas noise on what a page's readPixels wrote into aDest
  // (gfx::StealthProjectPixels on the framebuffer's stored pixels).
  // aWritten is what the read wrote: the part of the requested rectangle
  // inside the framebuffer, and the row stride.
  static void ProjectReadPixels(const ClientWebGLContext& aCtx,
                                const webgl::ReadPixelsDesc& aDesc,
                                const Span<uint8_t> aDest,
                                const webgl::ReadPixelsResult& aWritten) {
    // RGBA/UNSIGNED_BYTE only, the format of every default framebuffer and the
    // one readPixels always accepts. The float and integer formats of a user
    // framebuffer need their own definition of the bit the noise owns (an open
    // entry in the workbench docs); until then they read as drawn.
    if (aDesc.pi.format != LOCAL_GL_RGBA ||
        aDesc.pi.type != LOCAL_GL_UNSIGNED_BYTE) {
      return;
    }
    const gfx::StealthCanvasNoise noise =
        CanvasUtils::StealthNoise(aCtx.PrincipalOrNull());
    if (!noise || aWritten.subrect.IsEmpty() || !aWritten.byteStride) {
      return;
    }
    // The same rectangle one pixel larger on every side, tightly packed: the
    // function needs each pixel's neighbours, and a pixel at the edge of the
    // request has neighbours outside it. The part of it the framebuffer holds
    // is `fb`; beyond the framebuffer the function repeats the edge pixel, as
    // it does at the edge of every canvas.
    webgl::ReadPixelsDesc wide = aDesc;
    wide.srcOffset = {aDesc.srcOffset.x - 1, aDesc.srcOffset.y - 1};
    wide.size = {aDesc.size.x + 2, aDesc.size.y + 2};
    wide.packState = {};
    const size_t wideStride = size_t(wide.size.x) * 4;
    std::vector<uint8_t> wideBytes(wideStride * wide.size.y);
    webgl::ReadPixelsResult wideRead;
    if (!aCtx.DoReadPixels(wide, Span<uint8_t>(wideBytes), &wideRead) ||
        wideRead.subrect.IsEmpty() || wideRead.byteStride != wideStride) {
      return;
    }
    const gfx::IntRect fb = wideRead.subrect;
    uint8_t* const fbBase =
        wideBytes.data() + size_t(fb.Y()) * wideStride + size_t(fb.X()) * 4;
    // The page's pixels inside the framebuffer, in `fb` coordinates.
    const gfx::IntRect asked =
        (aWritten.subrect + gfx::IntPoint(1, 1) - fb.TopLeft())
            .Intersect(gfx::IntRect(gfx::IntPoint(), fb.Size()));
    if (asked.IsEmpty()) {
      return;
    }
    // The default framebuffer of a premultipliedAlpha context stores
    // premultiplied pixels; a user framebuffer stores whatever was drawn.
    const auto& options = aCtx.mNotLost->info.options;
    const bool premultiplied = !aCtx.State().mBoundReadFb && options.alpha &&
                               options.premultipliedAlpha;
    uint8_t* const askedBase =
        fbBase + size_t(asked.Y()) * wideStride + size_t(asked.X()) * 4;
    if (!gfx::StealthProjectPixels(fbBase, int32_t(wideStride), fb.Size(),
                                   gfx::SurfaceFormat::R8G8B8A8, asked,
                                   askedBase, int32_t(wideStride),
                                   premultiplied, noise)) {
      return;
    }
    // Back where the page's read put them, with the page's own packing.
    const auto& packing = aDesc.packState;
    const size_t bytes = size_t(asked.Width()) * 4;
    for (int32_t row = 0; row < asked.Height(); ++row) {
      // Request coordinates of this row's first pixel.
      const int32_t rx = asked.X() + fb.X() - 1;
      const int32_t ry = asked.Y() + fb.Y() - 1 + row;
      const size_t destOffset =
          size_t(packing.skipRows + ry) * aWritten.byteStride +
          size_t(packing.skipPixels + rx) * 4;
      if (destOffset + bytes > aDest.size()) {
        return;
      }
      memcpy(aDest.data() + destOffset, askedBase + size_t(row) * wideStride,
             bytes);
    }
  }

  // The same IPC read as ClientWebGLContext::GetBufferParameter, for the
  // projection's own use. Nothing() when the context is lost or the host does
  // not answer.
  static Maybe<double> BufferParameter(const ClientWebGLContext& aCtx,
                                       const GLenum aTarget,
                                       const GLenum aPname) {
    RefPtr<webgl::NotLostData> notLost(aCtx.mNotLost);
    if (!notLost) {
      return Nothing();
    }
    const auto& child = notLost->outOfProcess;
    child->FlushPendingCmds();
    Maybe<double> ret;
    if (!child->SendGetBufferParameter(aTarget, aPname, &ret)) {
      ret.reset();
    }
    return ret;
  }
};

}  // namespace mozilla

#undef STEALTH_WEBGL_RPROC

#endif  // DOM_CANVAS_STEALTHWEBGLREADBACK_H_
