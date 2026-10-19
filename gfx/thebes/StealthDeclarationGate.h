/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTHDECLARATIONGATE_H
#define GFX_STEALTHDECLARATIONGATE_H

#include "Units.h"
#include "mozilla/Maybe.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/gfx/StealthCanvasProjection.h"
#include "nsPoint.h"

namespace mozilla {
namespace gfx {

/**
 * Is the stealth engine on?
 *
 * THIS EXISTS BECAUSE THE ANSWER WAS BEING RE-DERIVED IN NINE PLACES, IN FOUR
 * DIFFERENT SPELLINGS. Counted 2026-08-23: `hw_seed() > 0`, `hw_seed() <= 0`,
 * `hw_seed() > 0 &&`, and a bare assignment - spread across Element.cpp,
 * Navigator.cpp, CanvasRenderingContext2D.cpp, gfxDWriteFonts.cpp,
 * gfxFT2FontBase.cpp, nsMediaFeatures.cpp, and this file twice.
 *
 * That is engine rule 16 in its purest shape - the same FACT computed in more
 * than one place - and the failure mode is silent. The day "on" becomes two
 * conditions instead of one, whoever adds the second has to find nine sites,
 * and the tenth they miss raises no error: it keeps behaving like stock Firefox
 * on one surface while every other surface is spoofed, which is exactly the
 * kind of internal disagreement a detector looks for.
 *
 * The home is this header rather than a new one because the predicate is this
 * gate's own precondition: the comment on StealthAssertDeclarationsComplete
 * already says "only checked when the stealth engine is actually on", and two
 * of the nine sites were in this very file.
 *
 * A bare binary launched with no seed is stock Firefox and must behave like
 * stock Firefox. That is not an incomplete declaration, it is the absence of
 * one.
 */
inline bool StealthEngineActive() {
  return StaticPrefs::zoom_stealth_fpp_hw_seed() > 0;
}

/**
 * The canvas noise as invisible_core declared it: the seed, or 0 when the
 * canvas is to be left exactly as drawn (the engine is off, or the canvas noise
 * is declared off, zoom.stealth.canvas.substitute_pixels), and the tolerance
 * that tells a blend of two colours from a neighbourhood with a third
 * (zoom.stealth.canvas.edge_tolerance, 0-255, refused at launch when missing). Every place that applies gfx::StealthProjectPixels
 * asks here, so the rule is written once.
 */
inline gfx::StealthCanvasNoise StealthCanvasNoiseDeclared() {
  if (!StealthEngineActive() ||
      !StaticPrefs::zoom_stealth_canvas_substitute_pixels()) {
    return gfx::StealthCanvasNoise::Off();
  }
  return {StaticPrefs::zoom_stealth_fpp_hw_seed(),
          StaticPrefs::zoom_stealth_canvas_edge_tolerance()};
}

/**
 * Refuse to run with an incomplete declaration.
 *
 * Engine rule 7, in the form the owner dictated on 2026-08-09: a value whose
 * domain is finite - or infinite but with the set of values the whole web
 * actually uses finite - lives ONLY in invisible_core. One source. No compiled
 * copy in the engine, not even as a floor. And if the declaration is missing,
 * the engine REFUSES: it does not invent a default and it does not ask the
 * machine.
 *
 * That last sentence is what this exists to enforce, because the two obvious
 * alternatives are both worse and both have already happened here. Falling back
 * to the host is a measurable leak: a CFF webfont diverged on 108 of 150
 * measureText fields between Windows and Linux because one path dropped through
 * to FreeType. Falling back to a compiled constant is not a leak but it is two
 * numbers for one fact, which is how the taskbar height ended up written in
 * four places in two languages, three of which were 48 and one of which was 40.
 *
 * Refusing is loud, immediate, and impossible to mistake for working. The
 * failure it replaces was silent by construction: a page served with a value
 * from the machine looks exactly like a page served correctly.
 *
 * Only checked when the stealth engine is actually on. A bare binary launched
 * with no seed is stock Firefox and must behave like stock Firefox - that is
 * not an incomplete declaration, it is the absence of one.
 */
void StealthAssertDeclarationsComplete();

/**
 * The window frame and the maximized window, in DEVICE pixels.
 *
 * One function per fact, read by everyone who can be asked it, instead of each
 * caller reading the prefs for itself. That is not tidiness. On 2026-08-09 three
 * DOM getters were moved onto the declaration - screenX/screenY,
 * mozInnerScreenX and mozInnerScreenY - and the event path was not, because
 * nothing connected the two in the source. The result was a contradiction a
 * page reads with one subtraction:
 *
 *     event.screenX - event.clientX === window.mozInnerScreenX
 *
 * holds on every real Firefox, because both sides come from the same widget.
 * With the geometry declared in one place and the events still asking the
 * widget, it held on NONE of 45 events on Windows and none of 20 on Linux,
 * against 0 failures out of 6 on stock.
 *
 * DEVICE pixels since firefox-38 (70-known-bugs.md B258, B261). The previous
 * declaration was four CSS integers - a window at (0, 0), no side border, the
 * content 85 below - read the same for every window. Retail Firefox 151 on
 * Windows 11, measured at the four persona scales, says otherwise: a maximized
 * window sits a resize border OFF the screen on three sides (-8 at 100%) and is
 * two borders wider than the work area; its content starts 85 device pixels
 * below the screen top at 100% and 107 at 125%, i.e. 85.6 CSS, a value no CSS
 * integer can carry; and a popup has its own frame and its own position. In
 * device pixels that is one rule, and each reader converts with its window's
 * own scale and Gecko's rounding, as it does for the real values.
 *
 * Nothing() when the engine is off, which is stock Firefox and not an
 * incomplete declaration. When the engine is on they never fall back: a missing
 * declaration goes to StealthAssertDeclarationsComplete(), and the gate has
 * already refused at launch in the parent.
 */
struct StealthWindowFrame {
  // Invisible resize border, on the left, right and bottom of every window and
  // on the top of a popup.
  int32_t mBorder;
  // A maximized window: from the screen top to the content top.
  int32_t mMaximizedUi;
  // A popup: from the window top (border included) to the content top.
  int32_t mPopupUi;
  // A popup opened without left/top: where it is put, on both axes.
  int32_t mPopupPosition;
};
Maybe<StealthWindowFrame> StealthDeclaredWindowFrame();

/**
 * The maximized main window: (-border, -border), the work area plus a border on
 * each side. Device pixels; the caller divides by its window's scale.
 */
Maybe<LayoutDeviceIntRect> StealthDeclaredMaximizedWindow();

/**
 * Lo SCARTO fra l'origine reale del contenuto e quella dichiarata, in app
 * units. Nothing() quando il motore e' spento.
 *
 * Esiste perche' la sostituzione secca non basta, e la prima versione di questa
 * patch lo ha dimostrato sul campo. `mozInnerScreenX` restituiva l'origine
 * dichiarata a OGNI finestra, iframe compresi: misurato il 2026-08-10, un
 * iframe posizionato a (220, 150) nella pagina rispondeva (0, 85) esattamente
 * come il documento di primo livello, e dentro quell'iframe l'invariante sopra
 * dava 220 contro 0. Non si SOSTITUISCE il valore, si sposta l'ANCORA: il
 * chiamante passa la propria origine reale e somma lo scarto, cosi' l'offset fra
 * i frame resta quello vero a qualsiasi profondita'.
 *
 * `aPopupWindowOrigin` decide QUALE finestra si dichiara. Nothing() e' la
 * finestra principale massimizzata, il cui contenuto parte da (0, interfaccia)
 * sullo schermo dichiarato. Some(p) e' un popup, la cui posizione e' quella
 * REALE (la sceglie la pagina, o nsWindowWatcher dalla dichiarazione quando la
 * pagina non la chiede) e il cui contenuto parte da p + (bordo, interfaccia del
 * popup). In pixel di dispositivo; `aAppUnitsPerDevPixel` e' quello del
 * pres context del chiamante.
 *
 * Una funzione sola, perche' lo scarto e' un fatto e i fatti stanno in un posto:
 * quando era calcolato in tre punti, il quarto consumatore fu dimenticato.
 */
Maybe<nsPoint> StealthDeclaredOriginShift(
    const nsPoint& aRealContentOrigin,
    const Maybe<LayoutDeviceIntPoint>& aPopupWindowOrigin,
    int32_t aAppUnitsPerDevPixel);

}  // namespace gfx
}  // namespace mozilla

#endif  // GFX_STEALTHDECLARATIONGATE_H
