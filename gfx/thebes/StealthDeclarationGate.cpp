/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthDeclarationGate.h"

#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/gfx/StealthRefuse.h"
#include "mozilla/Preferences.h"
#include "nsXULAppAPI.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/net/StealthBranding.h"
#include "nsIFile.h"
#include "nsString.h"

namespace mozilla {
namespace gfx {

namespace {

// The refusal of a missing declaration, one sentence for every list below.
[[noreturn]] void RefuseUndeclared(const char* aName) {
  StealthRefuse(
      "stealth: %s is not declared. Engine rule 7 - a finite-domain value "
      "lives only in invisible_core, and a missing declaration refuses "
      "rather than falling back to a compiled default or to the host. "
      "Launch through invisible_core, or clear zoom.stealth.fpp.hw_seed to "
      "run as stock Firefox.",
      aName);
}

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
  if (!StealthEngineActive()) {
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
      // The speech-synthesis voice list, added 2026-08-09 with the six ints
      // below. When it is empty SpeechSynthesis::GetVoices falls through to
      // nsSynthVoiceRegistry, i.e. the OS TTS registry - which names the real
      // operating system and every language pack installed on it, in one
      // `speechSynthesis.getVoices()`.
      {"zoom.stealth.voices.list", []() -> bool {
         auto lock = StaticPrefs::zoom_stealth_voices_list();
         return lock->IsEmpty();
       }},
      // The branding a page reads, added 2026-10-16. Missing, it would serve
      // the build's own brand: the compiled fallback this gate exists against.
      // "Not declared" here also means "not usable": the same predicate the
      // content sandbox uses to open the archive, so a launch this gate lets
      // through is one whose content processes can read the files.
      {"zoom.stealth.branding.archive", []() -> bool {
         nsCOMPtr<nsIFile> archive =
             mozilla::net::StealthBrandingDeclaredArchive();
         return !archive;
       }},
  };

  for (const auto& d : kStrings) {
    if (d.mIsEmpty()) {
      RefuseUndeclared(d.mName);
    }
  }

  const IntDeclaration kInts[] = {
      {"zoom.stealth.screen.taskbar_px",
       []() { return StaticPrefs::zoom_stealth_screen_taskbar_px(); }, 0},
      {"zoom.stealth.screen.color_depth",
       []() { return StaticPrefs::zoom_stealth_screen_color_depth(); }, 1},
      {"zoom.stealth.max_touch_points",
       []() { return StaticPrefs::zoom_stealth_max_touch_points(); }, 0},
      // Lo stile di hinting e l'antialias di FreeType, aggiunti il 2026-08-16.
      // Senza di loro `PrepareFontOptions` torna a chiedere al pattern
      // fontconfig, cioe' alla macchina: stesso binario, stessa pagina, hash
      // dei pixel diversi al variare di `/etc/fonts` - 3842037683 con l'host
      // (che qui vale `hintslight`), 1512591551 con una configurazione vuota,
      // 2489286717 con `hintnone`, misurati attraverso il prodotto.
      //
      // Il pavimento e' 0 e il sentinella e' -1, perche' 0 e' un valore legale
      // per entrambi: `FC_HINT_NONE` vale 0 e l'antialias spento pure.
      //
      // Sono letti solo dal ramo GTK, e il gate li pretende su ogni
      // piattaforma di proposito: una dichiarazione che vale per una sola
      // build e' una dichiarazione che qualcuno dimentichera' di spedire, e la
      // build che la dimentica non e' quella che se ne accorge.
      // The tolerance of the canvas noise's two-colour test, added 2026-10-19
      // with the single canvas function: a pixel whose neighbourhood is a
      // blend of two colours within it (text, two-colour antialiasing) is left
      // as drawn. 0 is a valid declaration; the undeclared -1 is not.
      {"zoom.stealth.canvas.edge_tolerance",
       []() { return StaticPrefs::zoom_stealth_canvas_edge_tolerance(); }, 0},
      // The Windows text quality, added 2026-10-19 ([B362]): read by
      // gfxDWriteFonts' GetSystemTextQuality instead of SystemParametersInfo.
      // Required on both platforms, like the cleartype_params it gates: a
      // profile is complete or it is not. The floor refuses the undeclared -1;
      // a value outside 0/4/5 is refused where it is read.
      {"zoom.stealth.text.system_quality",
       []() { return StaticPrefs::zoom_stealth_text_system_quality(); }, 0},
      {"zoom.stealth.text.freetype_hintstyle",
       []() { return StaticPrefs::zoom_stealth_text_freetype_hintstyle(); }, 0},
      {"zoom.stealth.text.freetype_antialias",
       []() { return StaticPrefs::zoom_stealth_text_freetype_antialias(); }, 0},
      // The window geometry, in DEVICE pixels since firefox-38 (B258, B261).
      // Until then four CSS integers - window_x, window_y, chrome_w, chrome_h -
      // added 2026-08-09 after screenX + outerWidth came to 1924 on a 1920
      // screen. The group is still declared whole or not at all, for the same
      // reason. Floors: a maximized window and a popup always have UI above
      // their content, a work area narrower than 320 is a bug; the border and
      // the popup position may be 0, so the sentinel is -1.
      {"zoom.stealth.screen.frame_border",
       []() { return StaticPrefs::zoom_stealth_screen_frame_border(); }, 0},
      {"zoom.stealth.screen.maximized_ui",
       []() { return StaticPrefs::zoom_stealth_screen_maximized_ui(); }, 1},
      {"zoom.stealth.screen.popup_ui",
       []() { return StaticPrefs::zoom_stealth_screen_popup_ui(); }, 1},
      {"zoom.stealth.screen.popup_position",
       []() { return StaticPrefs::zoom_stealth_screen_popup_position(); }, 0},
      {"zoom.stealth.screen.avail_device_width",
       []() { return StaticPrefs::zoom_stealth_screen_avail_device_width(); },
       320},
      {"zoom.stealth.screen.avail_device_height",
       []() { return StaticPrefs::zoom_stealth_screen_avail_device_height(); },
       240},
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
      RefuseUndeclared(name);
    }
  }

  for (const auto& d : kInts) {
    if (d.mGet() < d.mFloor) {
      RefuseUndeclared(d.mName);
    }
  }
}

Maybe<StealthWindowFrame> StealthDeclaredWindowFrame() {
  if (!StealthEngineActive()) {
    // Stock Firefox. Not an incomplete declaration, the absence of one.
    return Nothing();
  }
  const StealthWindowFrame frame{
      StaticPrefs::zoom_stealth_screen_frame_border(),
      StaticPrefs::zoom_stealth_screen_maximized_ui(),
      StaticPrefs::zoom_stealth_screen_popup_ui(),
      StaticPrefs::zoom_stealth_screen_popup_position()};
  if (frame.mBorder < 0 || frame.mMaximizedUi < 1 || frame.mPopupUi < 1 ||
      frame.mPopupPosition < 0) {
    // Unreachable in practice: the gate refuses at launch in the parent, so a
    // process that is serving pages has these. Kept as a refusal rather than a
    // fallback, because the day it becomes reachable the right behaviour is
    // still to stop, not to answer with the machine's real window.
    StealthAssertDeclarationsComplete();
    return Nothing();
  }
  return Some(frame);
}

Maybe<LayoutDeviceIntRect> StealthDeclaredMaximizedWindow() {
  const Maybe<StealthWindowFrame> frame = StealthDeclaredWindowFrame();
  if (frame.isNothing()) {
    return Nothing();
  }
  const int32_t w = StaticPrefs::zoom_stealth_screen_avail_device_width();
  const int32_t h = StaticPrefs::zoom_stealth_screen_avail_device_height();
  if (w < 320 || h < 240) {
    StealthAssertDeclarationsComplete();
    return Nothing();
  }
  const int32_t b = frame->mBorder;
  return Some(LayoutDeviceIntRect(-b, -b, w + 2 * b, h + 2 * b));
}

Maybe<nsPoint> StealthDeclaredOriginShift(
    const nsPoint& aRealContentOrigin,
    const Maybe<LayoutDeviceIntPoint>& aPopupWindowOrigin,
    int32_t aAppUnitsPerDevPixel) {
  const Maybe<StealthWindowFrame> frame = StealthDeclaredWindowFrame();
  if (frame.isNothing()) {
    return Nothing();
  }
  // The maximized window's content starts at the screen's left edge, the UI
  // height below its top; a popup's starts a border in and the popup UI down
  // from wherever the popup really is.
  const LayoutDeviceIntPoint declared =
      aPopupWindowOrigin
          ? *aPopupWindowOrigin +
                LayoutDeviceIntPoint(frame->mBorder, frame->mPopupUi)
          : LayoutDeviceIntPoint(0, frame->mMaximizedUi);
  return Some(LayoutDevicePixel::ToAppUnits(declared, aAppUnitsPerDevPixel) -
              aRealContentOrigin);
}

}  // namespace gfx
}  // namespace mozilla
