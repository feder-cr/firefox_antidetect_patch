/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_GL_STEALTH_OWN_GL_H_
#define GFX_GL_STEALTH_OWN_GL_H_

#include "StealthDeclarationGate.h"
#include "nsString.h"
#include "prlink.h"

#if defined(MOZ_WIDGET_GTK)
#  include <string>
#  include <unistd.h>
#endif

namespace mozilla::gl {

// On Linux WebGL drew on whatever GL the host had: GLX or EGL from the
// system's libraries, so the host's Mesa version, or its GPU driver, decided
// which declared extensions could be served and every pixel a page read back.
// The GL now ships with the engine: Mesa's llvmpipe behind EGL, built from the
// same pinned tree and patches as WebGPU's lavapipe (scripts/ci/
// lavapipe_build.py), surfaceless only, every Mesa option compiled in and one
// CPU profile, so the same page renders the same bytes on every host. The hooks
// fail closed: our files or no context, never a fallback to the host's libEGL
// (GLLibraryEGL::Init); always EGL's surfaceless platform, never the host's
// X11, Wayland or render nodes (CreateDisplayLocked); always EGL, never GLX
// (GLContextProviderLinux::CreateHeadless).
inline bool StealthOwnGLRequired() {
#if defined(MOZ_WIDGET_GTK)
  return gfx::StealthEngineActive();
#else
  return false;
#endif
}

// With the engine on, loads llvmpipe/libEGL.so.1 and llvmpipe/libGLESv2.so.2,
// next to the executable where the package puts them (package_linux.sh), as
// WebGPU finds its lavapipe. By path, so a host libEGL already mapped under the
// same name is never the one answered; each finds its libgallium beside it
// (RUNPATH $ORIGIN). False, with the reason, when a file is missing: EGL then
// stays unloaded. True and nothing done when the engine is off.
inline bool StealthLoadOwnGLOrRefuse(PRLibrary** aEGL, PRLibrary** aGL,
                                     nsACString* aFailureId) {
  if (!StealthOwnGLRequired()) {
    return true;
  }
#if defined(MOZ_WIDGET_GTK)
  char exe[4096];
  const ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  std::string dir = len > 0 ? std::string(exe, size_t(len)) : std::string();
  const auto slash = dir.rfind('/');
  if (slash != std::string::npos) {
    dir.replace(slash, std::string::npos, "/llvmpipe/");
    PRLibrary* egl = PR_LoadLibrary((dir + "libEGL.so.1").c_str());
    PRLibrary* gles =
        egl ? PR_LoadLibrary((dir + "libGLESv2.so.2").c_str()) : nullptr;
    if (egl && gles) {
      *aEGL = egl;
      *aGL = gles;
      return true;
    }
    if (egl) {
      PR_UnloadLibrary(egl);
    }
  }
#endif
  aFailureId->AssignLiteral("FEATURE_FAILURE_EGL_LOAD_3");
  return false;
}

}  // namespace mozilla::gl

#endif  // GFX_GL_STEALTH_OWN_GL_H_
