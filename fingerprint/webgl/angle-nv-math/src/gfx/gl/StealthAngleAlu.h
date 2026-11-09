/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_GL_STEALTH_ANGLE_ALU_H_
#define GFX_GL_STEALTH_ANGLE_ALU_H_

#include <vector>

#include "GLDefs.h"
#include "GLTypes.h"

namespace mozilla::gl {

// The arithmetic of the GPU the persona declares, in the HLSL ANGLE emits for
// WebGL. Which shader instructions the adapter runs decides the last bit of
// what a page reads back (a dot() on one GPU family is a chain of fused
// multiply-adds, on another a different sequence), so the HLSL carries the
// declared family's arithmetic as IEEE-only helpers and gives the same bits on
// every adapter. ANGLE's translator runs in libGLESv2, a library that cannot
// read Gecko's prefs, so the declaration travels the one channel ANGLE has: the
// feature overrides of the EGL display. `zoom.stealth.webgl.alu_class` is the
// declaration (0 = the host adapter's own arithmetic: no feature, the display
// asked for exactly as before; 1 = NVIDIA Maxwell and Pascal, 2 = NVIDIA Volta
// and later). Every class of 1 or more is the ANGLE D3D feature
// `stealthAluNvidia`, which is what float dot() needs: the two NVIDIA
// generations evaluate it alike. The classes differ only where a later step of
// the emulation tells them apart (sin and cos), so the class is kept whole
// here and each step reads it from the pref when it needs the difference.
//
// The attributes are added where Gecko asks ANGLE for an `EGL_PLATFORM_ANGLE_ANGLE`
// display (GLLibraryEGL::fGetDisplay and fGetPlatformDisplay), which is every
// display the pages' WebGL contexts and the WARP fallback are drawn on.
// StealthAngleAlu.cpp has the declaration's reader and the two functions the
// wrappers call.

// The ANGLE feature of an ALU class, or nullptr for the class whose HLSL is the
// host's own (0, and any class this build does not know).
inline const char* StealthAngleAluFeatureOf(int32_t aAluClass) {
  switch (aAluClass) {
    case 1:
    case 2:
      return "stealthAluNvidia";
    default:
      return nullptr;
  }
}

// The feature the declaration asks of this display, or nullptr. Windows only:
// the HLSL is ANGLE's D3D translation; on Linux the GL is the engine's own.
const char* StealthAngleAluDeclaredFeature();

// What GLLibraryEGL's wrappers call (hooks in GLLibraryEGL.h, declared there, so
// that header needs nothing of ours): whether a feature is declared, and the
// attribute list of an eglGetPlatformDisplay call with the feature added. The
// list lives until the next call on the same thread, which is after ANGLE has
// copied it.
bool StealthAngleAluDeclared();
const EGLAttrib* StealthAngleAluPlatformAttribs(EGLenum aPlatform,
                                                const EGLAttrib* aAttribs);

// The attribute list for eglGetPlatformDisplay: the caller's list as it is when
// there is nothing to add (no feature, another platform, or a list that already
// carries feature overrides), else a copy with the feature overrides appended.
// The object must outlive the call that uses Get().
class StealthAngleAluAttribs final {
 public:
  StealthAngleAluAttribs(const char* aFeature, EGLenum aPlatform,
                         const EGLAttrib* aAttribs)
      : mOriginal(aAttribs), mFeatures{aFeature, nullptr} {
    if (!aFeature || aPlatform != LOCAL_EGL_PLATFORM_ANGLE_ANGLE) {
      return;
    }
    if (aAttribs) {
      for (const EGLAttrib* p = aAttribs; *p != LOCAL_EGL_NONE; p += 2) {
        if (*p == LOCAL_EGL_FEATURE_OVERRIDES_ENABLED_ANGLE) {
          mList.clear();
          return;
        }
        mList.push_back(p[0]);
        mList.push_back(p[1]);
      }
    }
    mList.push_back(LOCAL_EGL_FEATURE_OVERRIDES_ENABLED_ANGLE);
    mList.push_back(reinterpret_cast<EGLAttrib>(mFeatures));
    mList.push_back(LOCAL_EGL_NONE);
  }

  StealthAngleAluAttribs(const StealthAngleAluAttribs&) = delete;
  StealthAngleAluAttribs& operator=(const StealthAngleAluAttribs&) = delete;

  const EGLAttrib* Get() const { return mList.empty() ? mOriginal : mList.data(); }

 private:
  const EGLAttrib* mOriginal;
  const char* mFeatures[2];
  std::vector<EGLAttrib> mList;
};

}  // namespace mozilla::gl

#endif  // GFX_GL_STEALTH_ANGLE_ALU_H_
