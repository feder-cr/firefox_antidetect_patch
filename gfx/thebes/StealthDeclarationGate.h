/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTHDECLARATIONGATE_H
#define GFX_STEALTHDECLARATIONGATE_H

namespace mozilla {
namespace gfx {

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

}  // namespace gfx
}  // namespace mozilla

#endif  // GFX_STEALTHDECLARATIONGATE_H
