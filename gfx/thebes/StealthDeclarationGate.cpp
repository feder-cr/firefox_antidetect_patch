/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthDeclarationGate.h"

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Preferences.h"
#include "nsXULAppAPI.h"
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

  // PARENT PROCESS ONLY, on first use. This is the THIRD placement and the two
  // failures behind it are the reason it is written down rather than tidied.
  //
  // Attempt 1, gfxPlatform::Init: every legitimate launch hung. On the
  // production path Playwright hands the prefs over once the process is already
  // up, so at gfx-init time they do not exist. Same timing that forced the font
  // manifest onto an environment variable.
  //
  // Attempt 2, first use with no process guard: nsScreen::GetAvailRect runs in
  // the CONTENT process, and a launch with all fifteen declarations present in
  // the profile still killed that process on the first read of
  // screen.availWidth, in a restart loop. The child's MOZ_CRASH text never
  // reached the parent's stderr, so the refusal was wrong AND invisible - the
  // worst of both. WHY those prefs read as missing in a content process is not
  // established yet; that is the open follow-up, and guessing at it here would
  // be the second mistake.
  //
  // The parent is where the check belongs regardless. Prefs travel parent to
  // child, so a complete parent is the precondition for a complete child, and
  // the parent is the process whose exit code the launcher can see. A refusal
  // the parent converts into a silent child restart is not a refusal.
  if (!XRE_IsParentProcess()) {
    return;
  }
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
      // Come il testo viene antialiasato. Aggiunta il 2026-08-15, e la
      // ragione per cui sta QUI e non in un default compilato: su Linux
      // quel valore veniva dal fontconfig dell'host, quindi il canvas di
      // testo dipendeva dalla macchina dell'utente. Un pavimento compilato
      // avrebbe chiuso la fuga e aperto la seconda fonte di verita'.
      {"zoom.stealth.text.antialias_mode", []() -> bool {
         auto lock = StaticPrefs::zoom_stealth_text_antialias_mode();
         return lock->IsEmpty();
       }},
      // The Accept-Language header, added 2026-08-09. Read from JavaScript
      // (juggler/NetworkObserver.js) rather than from C++, which is exactly
      // why it belongs here: nothing on that side can refuse, so the refusal
      // has to happen at startup or not at all. It used to be synthesized in
      // that file from a hardcoded ";q=0.5" while stock 151 sends ";q=0.9".
      {"zoom.stealth.http.accept_language", []() -> bool {
         auto lock = StaticPrefs::zoom_stealth_http_accept_language();
         return lock->IsEmpty();
       }},
      // The speech-synthesis voice list, added 2026-08-09 with the six ints
      // below. When it is empty SpeechSynthesis::GetVoices falls through to
      // nsSynthVoiceRegistry, i.e. the OS TTS registry - which names the real
      // operating system and every language pack installed on it, in one
      // `speechSynthesis.getVoices()`.
      {"zoom.stealth.voices.list", []() -> bool {
         auto lock = StaticPrefs::zoom_stealth_voices_list();
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
      // The window geometry, added 2026-08-09. Four values that three getters
      // read in different combinations - screenX, mozInnerScreenX and
      // outerWidth - which is exactly the shape that produced an impossible
      // window when only some of them were declared: screenX + outerWidth came
      // to 1924 on a 1920 screen. A floor of 0 on all four: a maximized window
      // sits at the origin and stock reports no horizontal chrome, so 0 is a
      // legal value here and the sentinel has to be -1.
      {"zoom.stealth.screen.window_x",
       []() { return StaticPrefs::zoom_stealth_screen_window_x(); }, 0},
      {"zoom.stealth.screen.window_y",
       []() { return StaticPrefs::zoom_stealth_screen_window_y(); }, 0},
      {"zoom.stealth.screen.chrome_w",
       []() { return StaticPrefs::zoom_stealth_screen_chrome_w(); }, 0},
      {"zoom.stealth.screen.chrome_h",
       []() { return StaticPrefs::zoom_stealth_screen_chrome_h(); }, 0},
      // The CSS interaction media features, added 2026-08-09. Floor 1 rather
      // than 0: PointerCapabilities::None is a legal value of the enum and a
      // nonsensical value for a machine somebody is browsing from, so a zero
      // here is a bug rather than a persona.
      {"zoom.stealth.pointer.primary",
       []() { return StaticPrefs::zoom_stealth_pointer_primary(); }, 1},
      {"zoom.stealth.pointer.all",
       []() { return StaticPrefs::zoom_stealth_pointer_all(); }, 1},
      // Six that this gate did NOT cover until 2026-08-09, and every one of
      // them is declared unconditionally by invisible_core. That is exactly
      // what made the hole invisible: nothing is broken in normal operation,
      // so no measurement could see it. What it costs is the refusal - if any
      // of these six ever went missing (a refactor dropping a line from
      // prefs.py, an older launcher, a direct launch), the engine would not
      // stop. It would read the REAL MACHINE and carry on:
      //
      //   screen.width / height  -> nsScreen::GetRect falls to the real
      //                             monitor, and GetAvailRect and
      //                             GetOuterSize fall with it
      //   hw_concurrency         -> the real CPU core count, on the main
      //                             thread AND inside every Worker
      //   audio.sample_rate      -> the real device rate through cubeb
      //   audio.max_channel_count-> the real sound card's channel count
      //   voices.list            -> the OS TTS registry, which names the
      //                             installed language packs
      //
      // The floors say what "declared" means for each: a screen narrower than
      // 320 or a machine with no cores is a bug, not a persona.
      {"zoom.stealth.screen.width",
       []() { return StaticPrefs::zoom_stealth_screen_width(); }, 320},
      {"zoom.stealth.screen.height",
       []() { return StaticPrefs::zoom_stealth_screen_height(); }, 240},
      {"zoom.stealth.hw_concurrency",
       []() { return StaticPrefs::zoom_stealth_hw_concurrency(); }, 1},
      {"zoom.stealth.audio.sample_rate",
       []() { return StaticPrefs::zoom_stealth_audio_sample_rate(); }, 8000},
      {"zoom.stealth.audio.max_channel_count",
       []() { return StaticPrefs::zoom_stealth_audio_max_channel_count(); }, 1},
      {"zoom.stealth.audio.output_latency_ms",
       []() { return StaticPrefs::zoom_stealth_audio_output_latency_ms(); }, 1},
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

Maybe<CSSIntPoint> StealthDeclaredWindowOrigin() {
  if (StaticPrefs::zoom_stealth_fpp_hw_seed() <= 0) {
    // Stock Firefox. Not an incomplete declaration, the absence of one.
    return Nothing();
  }
  const int32_t x = StaticPrefs::zoom_stealth_screen_window_x();
  const int32_t y = StaticPrefs::zoom_stealth_screen_window_y();
  if (x < 0 || y < 0) {
    // Unreachable in practice: the gate refuses at launch in the parent, so a
    // process that is serving pages has these. Kept as a refusal rather than a
    // fallback, because the day it becomes reachable the right behaviour is
    // still to stop, not to answer with the machine's real window position.
    StealthAssertDeclarationsComplete();
    return Nothing();
  }
  return Some(CSSIntPoint(x, y));
}

Maybe<CSSIntPoint> StealthDeclaredContentOrigin() {
  const Maybe<CSSIntPoint> window = StealthDeclaredWindowOrigin();
  if (window.isNothing()) {
    return Nothing();
  }
  const int32_t chromeW = StaticPrefs::zoom_stealth_screen_chrome_w();
  const int32_t chromeH = StaticPrefs::zoom_stealth_screen_chrome_h();
  if (chromeW < 0 || chromeH < 0) {
    StealthAssertDeclarationsComplete();
    return Nothing();
  }
  // chrome_w is the total across both sides; chrome_h is entirely above.
  return Some(CSSIntPoint(window->x + chromeW / 2, window->y + chromeH));
}

Maybe<nsPoint> StealthDeclaredOriginShift(const nsPoint& aRealContentOrigin) {
  const Maybe<CSSIntPoint> declared = StealthDeclaredContentOrigin();
  if (declared.isNothing()) {
    return Nothing();
  }
  const nsPoint declaredAppUnits(
      CSSPixel::ToAppUnits(CSSCoord(float(declared->x))),
      CSSPixel::ToAppUnits(CSSCoord(float(declared->y))));
  return Some(declaredAppUnits - aRealContentOrigin);
}

}  // namespace gfx
}  // namespace mozilla
