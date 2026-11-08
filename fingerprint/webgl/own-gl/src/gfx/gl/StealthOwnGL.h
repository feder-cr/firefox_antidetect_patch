/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_GL_STEALTH_OWN_GL_H_
#define GFX_GL_STEALTH_OWN_GL_H_

#include "GLLibraryEGL.h"
#include "StealthDeclarationGate.h"
#include "mozilla/gfx/gfxVars.h"
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
// (GLContextProviderLinux::CreateHeadless, and UseEGL forced where it is born,
// gfxPlatformGtk's constructor); no host GLX vsync and no host DMABuf.
inline bool StealthOwnGLRequired() {
#if defined(MOZ_WIDGET_GTK)
  return gfx::StealthEngineActive();
#else
  return false;
#endif
}

// UseEGL is born in gfxPlatformGtk's constructor, in the parent, from the host:
// Wayland, or an X11 Mesa that turns X11_EGL on. On an X11 host where it stays
// off, the compositor's context, the global context and the X11 visual took the
// host's GLX and mapped its libGL and libgallium. With the engine on it is always
// EGL, so every choice made on it is the same on every host and none is GLX.
inline void StealthOwnGLForceEGL() {
  if (StealthOwnGLRequired()) {
    gfx::gfxVars::SetUseEGL(true);
  }
}

// DMABuf imports buffers through the host's gbm and DRM render node, the host's
// Mesa; with the engine on it is off before anything probes the device (forcing
// EGL would otherwise lift its "Requires EGL" refusal).
template <class Feature, class StatusT>
inline void StealthOwnGLNoHostDMABuf(Feature& aFeature, StatusT aStatus) {
  if (StealthOwnGLRequired()) {
    aFeature.ForceDisable(aStatus, "The engine draws on its own GL",
                          "FEATURE_FAILURE_STEALTH_OWN_GL"_ns);
  }
}

// GLX SGI video sync opens a GLX context on the host's libGL, and is taken when
// EGL is off or the driver is Mesa. With EGL forced, clearing isMesa leaves the
// software vsync and the host's libGL unloaded.
inline void StealthOwnGLNoHostGLXVsync(bool& aIsMesa) {
  if (StealthOwnGLRequired()) {
    aIsMesa = false;
  }
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

// EGL's surfaceless platform, asked for as EGL defines it: with no native
// display. Mozilla's EGL_DEFAULT_DISPLAY is the X11 display wherever there is
// one (GLLibraryEGL.h), and Mozilla's own surfaceless path runs only where there
// is none. The engine asks for it under X11 too (Xvfb), where Mesa answers a
// native display with EGL_BAD_PARAMETER: candidate 42484125583 had no WebGL on
// the Linux gate. Otherwise as GetAndInitSurfacelessDisplay.
inline std::shared_ptr<EglDisplay> StealthOwnGLSurfacelessDisplay(
    GLLibraryEGL& aEgl, const StaticMutexAutoLock& aProofOfLock) {
  if (!aEgl.IsExtensionSupported(EGLLibExtension::MESA_platform_surfaceless)) {
    return nullptr;
  }
  const EGLAttrib attribs[] = {LOCAL_EGL_NONE};
  const EGLDisplay display = aEgl.fGetPlatformDisplay(
      LOCAL_EGL_PLATFORM_SURFACELESS_MESA, nullptr, attribs);
  if (display == EGL_NO_DISPLAY) {
    return nullptr;
  }
  return EglDisplay::Create(aEgl, display, true, aProofOfLock);
}

}  // namespace mozilla::gl

#endif  // GFX_GL_STEALTH_OWN_GL_H_
