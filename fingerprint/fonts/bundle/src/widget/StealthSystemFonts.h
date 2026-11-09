/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WIDGET_STEALTHSYSTEMFONTS_H
#define WIDGET_STEALTHSYSTEMFONTS_H

#include <stddef.h>

#include "StealthDeclarationGate.h"
#include "nsString.h"
#include "gfxFont.h"
#include "mozilla/ColorScheme.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/Maybe.h"
#include "nsColor.h"

namespace mozilla::widget {

/**
 * The CSS system fonts (font: menu, caption, message-box, small-caption,
 * status-bar, the -moz- widget fonts, and system-ui through Menu) come from the
 * ui.font.* prefs that invisible_core declares, and from nothing else.
 *
 * Windows retail computes them from NONCLIENTMETRICS: Segoe UI at 9pt, i.e.
 * 12px at 96dpi, weight normal (400 unless lfWeight is exactly FW_BOLD),
 * upright; the three form-control fonts (-moz-button, -moz-list, -moz-field)
 * are renamed "MS Shell Dlg 2" and keep that size. GetFontValue already reads
 * the pref family, `.size`, `.weight` (default 400) and `.italic` (default
 * false) before it asks the platform, which is the same rule.
 *
 * On Linux the platform used to answer from GTK through Pango, which named the
 * host: "Sans" at 13.3333px, a family no Windows has. StealthNativeSystemFont
 * is what NativeGetFont now is on GTK: it is reached only when a pref is
 * absent, and then it refuses (engine on) instead of asking the machine, or
 * says "no font" (bare binary, stock behaviour without a host to ask).
 */

// Whether the system fonts are the core's declaration: the stealth engine is on.
// A bare binary is stock Firefox and keeps the toolkit's answer.
inline bool StealthSystemFontsDeclared() { return gfx::StealthEngineActive(); }

// The pref a FontID is declared at, in the order of LookAndFeel::FontID.
extern const char* const kStealthSystemFontPrefs[];
extern const size_t kStealthSystemFontPrefCount;

// Declared means a non-empty family and a size of at least 1px, the two things
// GetFontValue needs to give a font that is not the 16px fallback.
inline bool StealthSystemFontDeclared(bool aHasFamily, float aSize) {
  return aHasFamily && aSize >= 1.0f;
}

// The first ui.font.* pref (family or `.size`) that is not declared, or false.
bool StealthFirstUndeclaredSystemFont(nsACString& aPrefName);

// The system colours a page reads through a CSS keyword and that no stand-in
// covers (sNonNativeThemeStandinColors): Highlight, HighlightText, Mark,
// MarkText, SelectedItem, SelectedItemText, AccentColor, AccentColorText,
// LinkText, VisitedText, ActiveText. Left undeclared they come from the GTK
// theme of the host. invisible_core declares them in its palette (`ui.<name>`,
// and `ui.<name>.dark`), and the gate refuses a launch where one is missing.
extern const char* const kStealthSystemColorPrefs[];
extern const size_t kStealthSystemColorPrefCount;

// The first of those prefs that is not declared, or false.
bool StealthFirstUndeclaredSystemColor(nsACString& aPrefName);

// Retail's dark theme takes HighlightText from GenericDarkColor, which has no
// pref and answers NS_SAME_AS_FOREGROUND_COLOR: the text keeps the colour of
// its parent. With the engine on this answer comes first, so neither
// ui.highlighttext nor ui.highlighttext.dark is read for the dark scheme.
Maybe<nscolor> StealthDarkSystemColor(LookAndFeel::ColorID aID,
                                      ColorScheme aScheme);

// The first undeclared font or colour pref, false when all are declared.
bool StealthFirstUndeclaredSystemTheme(nsACString& aPrefName);

// Engine on, parent process: refuse (StealthRefuse) when a font or colour pref
// is missing, so no store is ever filled from GTK or from the headless
// defaults. Called at the entry of FillStores and of every NativeGetColor and
// NativeGetFont. A no-op for a bare binary and in content processes (which get
// the parent's values).
void StealthVerifySystemTheme();

// Called at the entry of FillStores: verifies, and remembers whether the engine
// was already on when the store was filled.
void StealthSystemThemeFillStores();

// Called right after user.js has been read (nsXREDirProvider). If the look and
// feel was built before the declaration existed, its stores came from the
// toolkit: refuse on a missing declaration, otherwise rebuild them.
void StealthSystemThemeAfterUserPrefs();

// NativeGetFont of the GTK widget.
bool StealthNativeSystemFont(LookAndFeel::FontID aID, nsString& aFontName,
                             gfxFontStyle& aFontStyle);

}  // namespace mozilla::widget

#endif  // WIDGET_STEALTHSYSTEMFONTS_H
