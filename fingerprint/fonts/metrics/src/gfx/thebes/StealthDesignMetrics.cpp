/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// STEALTH: design-space glyph metrics for gfxHarfBuzzShaper. The members are
// declared in gfxHarfBuzzShaper.h; their definitions live here so that the
// Mozilla file carries only a hook. See fingerprint/fonts/metrics/FEATURE.md.
// No namespace and no `using`: these are definitions of members of the global
// class gfxHarfBuzzShaper, and everything else is spelled out, so the file does
// not depend on what a unified blob would have opened before it.

#include "gfxHarfBuzzShaper.h"

#include <stdint.h>

#include "gfxFont.h"
#include "gfxFontEntry.h"
#include "gfxFontVariations.h"
#include "harfbuzz/hb-ot.h"
#include "harfbuzz/hb.h"
#include "mozilla/RecursiveMutex.h"
#include "nsTArray.h"

// STEALTH: the design-space metrics font (member mDesignFont, declared in
// gfxHarfBuzzShaper.h), built on demand by GetDesignFont and never used for
// shaping. Mutable because GetDesignGlyphExtents and GetDesignAdvance below are
// const and a font that is only read has no business forcing them not to be.
hb_font_t* gfxHarfBuzzShaper::GetDesignFont() const {
  // Sotto mMutex, come OGNI altro stato mutabile di questa classe.
  //
  // L'intestazione lo dichiara a chiare lettere due membri piu' sotto: lo
  // shaper "is shared across threads via the global gfxFontCache", e per questo
  // esiste il mutex. mDesignFont era l'unico membro mutabile che quel contratto
  // non rispettava - un controlla-e-assegna nudo. Due thread potevano vederlo
  // nullo insieme, costruire entrambi un font e prendere entrambi un
  // riferimento alla faccia: uno vince l'assegnazione, l'altro resta con un
  // puntatore che nessuno distruggera' e che sta gia' usando. Il distruttore
  // ne libera uno solo.
  //
  // E' ricorsivo apposta, quindi prenderlo qui e' sicuro anche quando il
  // chiamante lo tiene gia'.
  mozilla::RecursiveMutexAutoLock lock(mMutex);

  // A second hb_font on the same face, used for nothing but design-space
  // metrics, and deliberately NOT the shaping font: that one carries our own
  // callbacks, which route back into the platform backend, which is the thing
  // being avoided here.
  //
  // hb_ot_font_set_funcs is called EXPLICITLY rather than relying on whatever
  // hb_font_create picks by default. That default comes from
  // get_default_funcs_name() in hb-font.cc, which reads the environment
  // variable HB_FONT_FUNCS, and "ft", "directwrite" and "coretext" are all
  // accepted values. An environment variable that silently redirects our
  // metrics to a host rasteriser is exactly the second source of truth engine
  // rule 7 forbids, so the implementation is pinned rather than inherited.
  //
  // The scale is unitsPerEm, so every answer arrives in FONT UNITS and the
  // caller multiplies by the unrounded size. That is what makes this path
  // TOTAL: hb-ot-font reads glyf, CFF and CFF2 alike, so a PostScript-outline
  // webfont is served by the same code as a bundled TrueType face and there is
  // nothing left to fall through to. Measured before this existed: a CFF
  // webfont diverged on 108 of 150 measureText fields between Windows and
  // Linux, because the CFF case fell through to FreeType's grid-fitted bounds
  // and came back as whole integers.
  if (mDesignFont) {
    return mDesignFont;
  }
  const uint16_t upem = mFont->GetFontEntry()->UnitsPerEm();
  if (!upem || upem == gfxFontEntry::kInvalidUPEM) {
    return nullptr;
  }
  // `auto`, non `hb_face_t*`, e NIENTE hb_face_destroy a mano.
  //
  // GetHBFace() torna un AutoHBFace PER VALORE, che e' un oggetto RAII: se lo
  // si assegna a un puntatore grezzo, il temporaneo viene convertito e poi
  // DISTRUTTO alla fine di quella stessa istruzione, quindi il puntatore e'
  // gia' penzolante alla riga dopo. hb_font_create prende il proprio
  // riferimento scrivendo dentro memoria liberata, e il rilascio esplicito
  // qui sotto era il terzo decremento sullo stesso oggetto morto: da li' in
  // avanti l'allocatore e' corrotto e il segmentation fault arriva altrove,
  // di solito molto dopo, dentro HarfBuzz che compone il testo.
  //
  // Misurato PRIMA di questa correzione: il processo di CONTENUTO moriva in 6
  // sessioni di navigazione su 10 su Linux e 4 su 10 su Windows, contro 0 su
  // 10 del firefox di Playwright sulla stessa macchina, e i core cadevano su
  // hb_face_t::get_num_glyphs con la faccia gia' liberata.
  //
  // Gli altri TREDICI chiamanti di GetHBFace() nell'albero tengono vivo
  // l'oggetto RAII; questo era l'unico che lo buttava via. Il proprietario
  // dev'essere UNO, e qui e' l'AutoHBFace. Vedi 70-known-bugs.md, 2026-08-10.
  const auto face(mFont->GetFontEntry()->GetHBFace());
  if (!face) {
    return nullptr;
  }
  hb_font_t* font = hb_font_create(face);
  if (!font) {
    return nullptr;
  }
  hb_ot_font_set_funcs(font);
  hb_font_set_scale(font, upem, upem);
  hb_font_set_ppem(font, 0, 0);

  // Variations travel too, or a variable font would be measured at its default
  // instance while it renders at another.
  AutoTArray<gfxFontVariation, 8> vars;
  mFont->GetFontEntry()->GetVariationsForStyle(vars, *mFont->GetStyle());
  if (vars.Length() > 0) {
    static_assert(sizeof(gfxFontVariation) == sizeof(hb_variation_t),
                  "gfxFontVariation and hb_variation_t must be compatible");
    hb_font_set_variations(
        font, reinterpret_cast<hb_variation_t*>(vars.Elements()), vars.Length());
  }
  mDesignFont = font;
  return mDesignFont;
}

// STEALTH: glyph metrics in DESIGN space, in FONT UNITS, read from the font's
// own outlines by HarfBuzz and never by a platform rasteriser (this applies to
// GetDesignGlyphExtents and GetDesignAdvance, declared in gfxHarfBuzzShaper.h).
//
// They exist so the FreeType backend reports the same ink box and the same
// advance the DirectWrite one does. gfxDWriteFont::GetGlyphBounds calls
// GetDesignGlyphMetrics, which is the outline and nothing else, while
// gfxFT2FontBase asks FreeType for HINTED bounds - so the two disagree, and
// the disagreement reaches the page through measureText's four
// actualBoundingBox fields. Measured 2026-08-09 against stock Firefox 151:
// with the ink box reported honestly, Windows matches on 12960 of 12960
// fields and Linux differs on 1440.
//
// Confirmed by hand before it was written: Arial 'C' has xMin 102 at 2048
// units per em, so at 16px the left edge is -0.796875, and that is exactly
// what stock Firefox reports for actualBoundingBoxLeft.
//
// These replaced a glyf-only reader on 2026-08-09, and the replacement is the
// whole point rather than a tidy-up. The old one returned false for a font
// with no glyf table and the caller fell through to its own backend, which is
// every CFF webfont a page cares to load: measured, 108 of 150 measureText
// fields diverged between Windows and Linux on one, with Linux returning
// whole integers straight out of FreeType's grid fitting. hb-ot-font reads
// glyf, CFF and CFF2 alike, so there is no case left to fall through with -
// engine rule 7, make the declared path total rather than add a fallback.
bool gfxHarfBuzzShaper::GetDesignGlyphExtents(uint16_t aGID, int32_t& aXBearing,
                                              int32_t& aYBearing,
                                              int32_t& aWidth,
                                              int32_t& aHeight) const {
  hb_font_t* font = GetDesignFont();
  if (!font) {
    return false;
  }
  hb_glyph_extents_t ext;
  if (!hb_font_get_glyph_extents(font, aGID, &ext)) {
    // A glyph with no outline at all - a space - has no extents, and an empty
    // box is the honest answer rather than a failure.
    aXBearing = aYBearing = aWidth = aHeight = 0;
    return true;
  }
  aXBearing = ext.x_bearing;
  aYBearing = ext.y_bearing;
  aWidth = ext.width;
  aHeight = ext.height;
  return true;
}

bool gfxHarfBuzzShaper::GetDesignAdvance(uint16_t aGID,
                                         int32_t& aAdvance) const {
  hb_font_t* font = GetDesignFont();
  if (!font) {
    return false;
  }
  aAdvance = hb_font_get_glyph_h_advance(font, aGID);
  return true;
}
