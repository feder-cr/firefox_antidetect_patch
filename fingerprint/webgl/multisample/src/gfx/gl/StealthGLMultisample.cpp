/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthGLMultisample.h"

#include "mozilla/StaticPrefs_webgl.h"

namespace mozilla::gl {

// Multisampling is part of the GPU the core declares: webgl.msaa-samples, 4 on
// every identity, which every declared GPU pairs with (its declared
// MAX_SAMPLES is 4 or more). Mozilla's driver workarounds decide it from the
// host's renderer instead: on the Basic Render Driver, Microsoft's software D3D
// device and the one every Windows machine without a GPU renders on, it is
// dropped (bug 978966, a Windows 8 blending bug), and a page read
// antialias:false and SAMPLES 0, a fact about the host. The decision is taken
// back where it is born: when the core declares samples and the driver can
// draw them, the context has them. Today's device blends into a multisampled
// target as a GPU does (TestStealthGLMultisample, on the Windows CI host, which
// has no GPU). A declared 0 needs nothing here: Mozilla then turns antialias
// off for every context (ClientWebGLContext). Never more than the driver
// reports.
bool StealthDeclaredMultisample(bool aDriverSupports) {
  return aDriverSupports && StaticPrefs::webgl_msaa_samples() > 0;
}

}  // namespace mozilla::gl
