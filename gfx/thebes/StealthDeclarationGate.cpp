/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthDeclarationGate.h"

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Preferences.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "nsString.h"

namespace mozilla {
namespace gfx {

namespace {

// One row per declaration whose domain is finite, i.e. per value that
// invisible_core is the single source of. Adding a declared surface means
// adding a row here, and forgetting to is caught the first time a profile is
// generated without it rather than by a page noticing months later.
//
// The message names the pref, not "a declaration", because the person reading
// it is looking at a browser that refused to start and needs to know which one.
// The string prefs are DataMutexString, so reading one means taking the lock
// and copying out; there is no accessor that hands back a plain string.
struct StringDeclaration {
  const char* mName;
  bool (*mIsEmpty)();
};

struct IntDeclaration {
  const char* mName;
  int32_t (*mGet)();
  int32_t mFloor;  // a value below this means "not declared"
};

}  // namespace

void StealthAssertDeclarationsComplete() {
  // Stock behaviour when the engine is off: no seed, no declarations, nothing
  // to be complete about.
  if (StaticPrefs::zoom_stealth_fpp_hw_seed() <= 0) {
    return;
  }

  // Checked on FIRST USE, not at startup, and that is not a preference - it is
  // the only place it can work. The first attempt put this in
  // gfxPlatform::Init, which runs before the prefs exist on the production
  // path: Playwright hands them over the connection once the process is
  // already up, so a legitimate launch refused and hung. Same timing that
  // forced the font manifest onto an environment variable.
  //
  // First USE is still early enough to keep the promise, because it runs before
  // the value is handed back, so no page can ever receive one that came from a
  // compiled default. Once per process.
  static Atomic<bool> sChecked(false);
  if (sChecked.exchange(true)) {
    return;
  }

  const StringDeclaration kStrings[] = {
      {"zoom.stealth.fonts.manifest", []() -> bool {
         auto lock = StaticPrefs::zoom_stealth_fonts_manifest();
         return lock->IsEmpty();
       }},
      {"zoom.stealth.fonts.generics", []() -> bool {
         auto lock = StaticPrefs::zoom_stealth_fonts_generics();
         return lock->IsEmpty();
       }},
      {"zoom.stealth.media.decode_support", []() -> bool {
         auto lock = StaticPrefs::zoom_stealth_media_decode_support();
         return lock->IsEmpty();
       }},
      {"zoom.stealth.media.decoding_info", []() -> bool {
         auto lock = StaticPrefs::zoom_stealth_media_decoding_info();
         return lock->IsEmpty();
       }},
      {"zoom.stealth.text.coverage_ladder", []() -> bool {
         auto lock = StaticPrefs::zoom_stealth_text_coverage_ladder();
         return lock->IsEmpty();
       }},
  };

  for (const auto& d : kStrings) {
    if (d.mIsEmpty()) {
      MOZ_CRASH_UNSAFE_PRINTF(
          "stealth: %s is not declared. Engine rule 7 - a finite-domain value "
          "lives only in invisible_core, and a missing declaration refuses "
          "rather than falling back to a compiled default or to the host. "
          "Launch through invisible_core, or clear zoom.stealth.fpp.hw_seed to "
          "run as stock Firefox.",
          d.mName);
    }
  }

  const IntDeclaration kInts[] = {
      {"zoom.stealth.screen.taskbar_px",
       []() { return StaticPrefs::zoom_stealth_screen_taskbar_px(); }, 0},
      {"zoom.stealth.screen.color_depth",
       []() { return StaticPrefs::zoom_stealth_screen_color_depth(); }, 1},
      {"zoom.stealth.max_touch_points",
       []() { return StaticPrefs::zoom_stealth_max_touch_points(); }, 0},
      {"zoom.stealth.canvas.noise_skip_mask",
       []() { return StaticPrefs::zoom_stealth_canvas_noise_skip_mask(); }, 0},
  };

  // The rasterisation parameters are checked here too, and they are the reason
  // this list is worth keeping rather than trusting the callers.
  // gfxDWriteFont::UpdateClearTypeVars opens with five hard-coded values -
  // ClearType level 1.0, contrast 1.0, gamma 2.2, RGB geometry, DEFAULT
  // rendering mode - and only then lets these prefs override them, each on a
  // -1 sentinel. With the engine on and one pref missing, the compiled value
  // wins in silence, which is the second source of truth engine rule 7
  // forbids. It is not hypothetical: the core declares rendering_mode 5 while
  // the compiled default is DWRITE_RENDERING_MODE_DEFAULT, which is 0, so the
  // two answers were never even the same number.
  //
  // Plain Preferences rather than StaticPrefs because that is how the
  // rasteriser reads them, and a gate has to check the value the code will
  // actually see. Both platforms' sets are required on both platforms: a
  // profile is complete or it is not, and which half the host happens to use
  // is not the profile's business.
  static const char* const kRasterInts[] = {
      "gfx.font_rendering.cleartype_params.gamma",
      "gfx.font_rendering.cleartype_params.enhanced_contrast",
      "gfx.font_rendering.cleartype_params.cleartype_level",
      "gfx.font_rendering.cleartype_params.pixel_structure",
      "gfx.font_rendering.cleartype_params.rendering_mode",
      "gfx.font_rendering.freetype.gamma",
      "gfx.font_rendering.freetype.enhanced_contrast",
  };
  for (const char* name : kRasterInts) {
    if (Preferences::GetInt(name, -1) < 0) {
      MOZ_CRASH_UNSAFE_PRINTF(
          "stealth: %s is not declared. Engine rule 7 - a finite-domain value "
          "lives only in invisible_core, and a missing declaration refuses "
          "rather than falling back to a compiled default or to the host. "
          "Launch through invisible_core, or clear zoom.stealth.fpp.hw_seed to "
          "run as stock Firefox.",
          name);
    }
  }

  for (const auto& d : kInts) {
    if (d.mGet() < d.mFloor) {
      MOZ_CRASH_UNSAFE_PRINTF(
          "stealth: %s is not declared. Engine rule 7 - a finite-domain value "
          "lives only in invisible_core, and a missing declaration refuses "
          "rather than falling back to a compiled default or to the host. "
          "Launch through invisible_core, or clear zoom.stealth.fpp.hw_seed to "
          "run as stock Firefox.",
          d.mName);
    }
  }
}

}  // namespace gfx
}  // namespace mozilla
