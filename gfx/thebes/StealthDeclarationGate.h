/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTHDECLARATIONGATE_H
#define GFX_STEALTHDECLARATIONGATE_H

#include "Units.h"
#include "mozilla/Maybe.h"
#include "mozilla/StaticPrefs_zoom.h"
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
 * Where the window is, and where its content starts, in CSS pixels.
 *
 * These two exist so that the answer is computed ONCE and read by everyone who
 * can be asked it, instead of each caller reading the prefs for itself. That is
 * not tidiness. On 2026-08-09 three DOM getters were moved onto the
 * declaration - screenX/screenY, mozInnerScreenX and mozInnerScreenY - and the
 * event path was not, because nothing connected the two in the source. The
 * result was a contradiction a page reads with one subtraction:
 *
 *     event.screenX - event.clientX === window.mozInnerScreenX
 *
 * holds on every real Firefox, because both sides come from the same widget.
 * With the geometry declared in one place and the events still asking the
 * widget, it held on NONE of 45 events on Windows and none of 20 on Linux,
 * against 0 failures out of 6 on stock. Humanised movement made it worse, since
 * it emits dozens of events per action and every one of them is a fresh sample
 * of the same broken relation.
 *
 * So the invariant is restored the only way that keeps holding when somebody
 * adds a new geometry-reading API next month: there is one function, everything
 * calls it, and the relation is true by construction rather than by two callers
 * happening to agree.
 *
 * Both return Nothing() when the engine is off, which is stock Firefox and not
 * an incomplete declaration. When the engine is on they never fall back: a
 * missing declaration goes to StealthAssertDeclarationsComplete(), and the gate
 * has already refused at launch in the parent, so content processes cannot
 * reach a state where these are undeclared.
 *
 * CSS pixels, deliberately. The widget layer underneath works in device pixels
 * and does not know the CSS scale - `layout.css.devPixelsPerPx` is applied
 * above it, in the device context - so declaring down there would have meant
 * converting with a number the widget cannot see.
 */
Maybe<CSSIntPoint> StealthDeclaredWindowOrigin();

/**
 * The content origin: the window origin plus the chrome around it. Horizontal
 * chrome is the TOTAL of both sides, so half of it sits to the left; vertical
 * chrome is entirely above, which is the tab strip and the toolbar.
 */
Maybe<CSSIntPoint> StealthDeclaredContentOrigin();

/**
 * Lo SCARTO fra le coordinate reali dello schermo e quelle dichiarate, in app
 * units. Nothing() quando il motore e' spento.
 *
 * Esiste perche' la sostituzione secca non basta, e la prima versione di questa
 * patch lo ha dimostrato sul campo. `mozInnerScreenX` restituiva l'origine
 * dichiarata a OGNI finestra, iframe compresi: misurato il 2026-08-10, un
 * iframe posizionato a (220, 150) nella pagina rispondeva (0, 85) esattamente
 * come il documento di primo livello, e dentro quell'iframe l'invariante che
 * questa patch esiste per difendere
 *
 *     event.screenX - event.clientX === window.mozInnerScreenX
 *
 * dava 220 contro 0. La contraddizione era stata tolta al primo livello e
 * ricreata identica un livello sotto, e il gate non se ne accorgeva perche'
 * guardava solo il primo livello.
 *
 * La forma giusta e' quella che Event.cpp usava gia': non si SOSTITUISCE il
 * valore, si sposta l'ANCORA. Il chiamante passa la propria origine reale - per
 * il documento di primo livello e' l'offset del root widget, per un sottoframe
 * e' il rettangolo del suo root frame - e somma lo scarto. Cosi' l'offset
 * relativo fra i frame resta quello vero, e la relazione fra eventi e geometria
 * vale a QUALSIASI profondita' di annidamento per costruzione, invece che a un
 * livello solo perche' due chiamanti si trovano d'accordo.
 *
 * Una funzione sola, perche' lo scarto e' un fatto e i fatti stanno in un posto:
 * quando era calcolato in tre punti, il quarto consumatore fu dimenticato.
 */
Maybe<nsPoint> StealthDeclaredOriginShift(const nsPoint& aRealContentOrigin);

}  // namespace gfx
}  // namespace mozilla

#endif  // GFX_STEALTHDECLARATIONGATE_H
