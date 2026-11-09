/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthAngleAlu.h"

#include <memory>

#include "StealthDeclarationGate.h"
#include "mozilla/StaticPrefs_zoom.h"

namespace mozilla::gl {

// The one place that reads the declaration. The engine off, or any host but
// Windows (where the GL is not ANGLE's D3D), is no declaration: the display is
// asked for exactly as before.
const char* StealthAngleAluDeclaredFeature() {
#ifdef XP_WIN
  if (gfx::StealthEngineActive()) {
    return StealthAngleAluFeatureOf(
        StaticPrefs::zoom_stealth_webgl_alu_class());
  }
#endif
  return nullptr;
}

bool StealthAngleAluDeclared() { return StealthAngleAluDeclaredFeature(); }

const EGLAttrib* StealthAngleAluPlatformAttribs(EGLenum aPlatform,
                                                const EGLAttrib* aAttribs) {
  static thread_local std::unique_ptr<StealthAngleAluAttribs> sLast;
  sLast = std::make_unique<StealthAngleAluAttribs>(
      StealthAngleAluDeclaredFeature(), aPlatform, aAttribs);
  return sLast->Get();
}

}  // namespace mozilla::gl
