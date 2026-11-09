/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTHFCEMPTYCONFIG_H
#define GFX_STEALTHFCEMPTYCONFIG_H

// Fontconfig without the host (Linux).
//
// Stock Firefox lets fontconfig load the host's configuration: /etc/fonts,
// ~/.config/fontconfig, every font directory they name, and the FONTCONFIG_*
// variables. Nothing a page reads may come from the host, so the process runs
// on an empty configuration instead: no configuration file, no font directory,
// no rule. The only fonts in it are the ones Firefox adds itself with
// FcConfigAppFontAddDir (the bundle); the font list is the bundle's anyway.
//
// The system font options (what GDK and Xft would say about antialiasing,
// subpixel order and the LCD filter) are fixed the same way: ClearType, the declared
// pixel structure's order (RGB, as a stock Windows install), ClearType level 100.

#include <stdint.h>

typedef struct _FcConfig FcConfig;
typedef struct _cairo_font_options cairo_font_options_t;

// Makes an empty configuration the current one, the first time it is called:
// FcConfigCreate (no file is parsed), rescan interval 0 (so neither
// FcInitBringUptoDate nor the font list's update timer ever reloads the system
// configuration), FcConfigSetCurrent. Every later call does nothing and returns
// the same configuration, so the fonts the application already added to it stay.
// Never null: when fontconfig cannot create or install it, the process stops
// (MOZ_RELEASE_ASSERT) rather than fall back to the host's configuration.
//
// Called before FcInit() in nsLookAndFeel's constructor and at the start of
// gfxFcPlatformFontList's constructor, whichever runs first.
FcConfig* StealthFcEmptyConfig();

// The body of gfxFcPlatformFontList::UpdateSystemFontOptions: replaces
// aOptions (system cairo, owned by the caller) with ClearType options -
// CAIRO_ANTIALIAS_SUBPIXEL, the subpixel order of the declared pixel structure, hint
// style left to the pattern - sets aLcdFilter to FC_LCD_DEFAULT (ClearType level 100)
// and the 2D factory's subpixel order to the same order. Neither Xft nor GDK is read.
// Returns whether the options changed, as UpdateSystemFontOptions does: true the first
// time, false afterwards.
bool StealthFixedFontOptions(cairo_font_options_t*& aOptions,
                             int32_t& aLcdFilter);

#endif  // GFX_STEALTHFCEMPTYCONFIG_H
