/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_STEALTHGLYPHCOVERAGE_H_
#define MOZILLA_GFX_STEALTHGLYPHCOVERAGE_H_

#include <stddef.h>
#include <stdint.h>

// Deliberately free of every other header. This is included from VENDORED SKIA
// (gfx/skia/skia/src/core/SkScalerContext.cpp), which is compiled in a unified
// build alongside files that know nothing about Gecko; a header that dragged in
// nsString or StaticPrefs would put that whole world into Skia's translation
// units. All of that lives in the .cpp.

namespace mozilla {
namespace gfx {

/**
 * Quantise a glyph's 8-bit coverage mask onto the ladder declared by
 * invisible_core in zoom.stealth.text.coverage_ladder.
 *
 * WHY THIS EXISTS. The number of DISTINCT coverage levels on an antialiased
 * glyph edge is a platform tell that needs no hash to read - a detector can
 * COUNT. Measured over 48 family/size/weight combinations: Windows/DirectWrite
 * produces 9-19 levels per render, Linux/FreeType 193-256 (16 of the 48 used
 * all 256). Counting is also the more robust tell of the two, because two
 * genuine Windows machines produce different mask BYTES anyway (they move with
 * the Windows build and the ClearType settings) while both land in the low tens
 * of levels.
 *
 * WHY HERE AND NOT AT READBACK. This used to be applied to the RGBA buffer in
 * CanvasRenderingContext2D::GetImageBuffer, after compositing, where it had to
 * guess which pixels were text: the test was that the three colour channels
 * agreed, so that a colour-emoji BITMAP - whose alpha carries artwork, not an
 * antialiasing ramp - was left alone. That guess is wrong for the commonest
 * case there is, coloured text. Measured 2026-08-08 on FingerprintJS Pro's own
 * canvas, whose text is #006699 and rgba(102,204,0,0.2): not one ink pixel had
 * r==g==b, so the ladder was a total no-op and the Times New Roman line read
 * back 12 alpha levels on Windows against 208 on Linux.
 *
 * On the mask there is nothing to guess. SkMask::kA8_Format IS a greyscale
 * coverage mask by construction, a colour glyph is kARGB32_Format and never
 * reaches this function, and the fill colour is not applied yet - it cannot
 * interfere because it does not exist here. Blocking at birth rather than
 * correcting afterwards, and the correction was the part that was wrong.
 *
 * THE REAL SHAPE STAYS. This quantises coverage, it does not substitute pixels:
 * text drawn is still the text read back, with the coarser edge Windows
 * produces. 0 and 255 are the ladder's endpoints, so fully transparent and
 * fully opaque bytes never move.
 *
 * An empty or malformed ladder leaves the mask exactly as the rasteriser
 * produced it - an override, never a replacement.
 *
 * @param aImage    first byte of the mask; one byte per pixel
 * @param aWidth    pixels per row (may be less than aRowBytes)
 * @param aHeight   rows
 * @param aRowBytes stride in bytes
 */
/**
 * La modalita' di antialiasing DICHIARATA, o -1 se non lo e'.
 *
 * 0 = nessuno, 1 = scala di grigi, 2 = subpixel, **-1 = non dichiarata**. Il
 * tipo e' un intero con sentinella e non `Maybe<AntialiasMode>` perche' questo
 * header e' incluso da Skia: questo file dichiara in cima di tenersi fuori
 * `nsString`, `StaticPrefs` e `Types.h`, e `Maybe` sarebbe stato un quarto.
 *
 * ⛔ PERCHE' ESISTE. Su Linux quel valore veniva dal FONTCONFIG DELL'HOST:
 * `ScaledFontFontconfig` legge `FC_RGBA` e, se dice RGB/BGR/VRGB/VBGR, mette
 * `AntialiasMode::SUBPIXEL`. Quindi il testo su canvas della nostra build
 * dipendeva dalla configurazione della macchina dell'utente - la fuga verso
 * l'host che la regola 7 vieta, non una differenza fra le nostre due build.
 *
 * MISURATO il 2026-08-15, una "A" nera in un canvas 200x60: 717 pixel con i
 * tre canali diversi su Linux, ZERO su Windows. I colori distinti passano da
 * 16 a 27, e sopra i 16 `ApplyStealthCanvasPixelSubstitution` smette di
 * saltare il buffer e lo riscrive per intero con byte che dipendono dal SEME e
 * non dal contenuto: da li' stringhe diverse davano lo stesso hash, che e' un
 * tell che si legge con due `fillText` e un confronto.
 *
 * E LA CORREZIONE STA QUI E NON SULLA SOGLIA DEI 16 COLORI. Alzare la soglia
 * farebbe sparire il sintomo lasciando in piedi la fuga verso l'host, e
 * rimetterebbe in gioco le prove dei rilevatori che quel salto protegge. Si
 * impedisce al motore di produrre una maschera subpixel, che e' la regola 3:
 * si blocca alla nascita, non si corregge dopo.
 *
 * Due costruttori di `ScaledFontFontconfig` decidono quel campo, piu' il
 * percorso WebRender. Chiamano tutti questa: la relazione e' vera per
 * costruzione invece che perche' tre punti si trovano d'accordo (regola 16).
 */
int32_t StealthDeclaredAntialiasMode();
void StealthQuantiseGlyphCoverage(uint8_t* aImage, int32_t aWidth,
                                  int32_t aHeight, size_t aRowBytes);

}  // namespace gfx
}  // namespace mozilla

#endif  // MOZILLA_GFX_STEALTHGLYPHCOVERAGE_H_
