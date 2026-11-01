/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthGLMultisample.h"

#include "mozilla/StaticPrefs_webgl.h"

namespace mozilla::gl {

// Mozilla turns multisampling off on the Basic Render Driver, Microsoft's
// software D3D device (bug 978966: on the Windows 8 device multisampled draws
// ignored the blend function). Every Windows machine without a GPU renders
// there, so a page's antialias:false and SAMPLES 0 became a fact about the
// host, one no machine with the GPU the identity declares shows. Today's
// device blends into a multisampled target as a GPU does
// (TestStealthGLMultisample, run on the Windows CI host, which has no GPU).
// With WebGL forced on regardless of the host's driver (webgl.force-enabled,
// which the core sets on every host) the feature stays as the driver reports
// it. Every other workaround, and every other renderer, is Mozilla's.
bool StealthKeepsMultisample(bool aWasSupported, GLRenderer aRenderer) {
  return aWasSupported && aRenderer == GLRenderer::MicrosoftBasicRenderDriver &&
         StaticPrefs::webgl_force_enabled();
}

}  // namespace mozilla::gl
