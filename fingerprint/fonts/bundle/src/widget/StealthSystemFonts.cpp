/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthSystemFonts.h"

#include <iterator>

#include "StealthDeclarationGate.h"
#include "mozilla/Preferences.h"
#include "mozilla/Atomics.h"
#include "mozilla/gfx/StealthRefuse.h"
#include "nsXULAppAPI.h"

namespace mozilla::widget {

// Same order as LookAndFeel::FontID, and as sFontPrefs in nsXPLookAndFeel.cpp;
// TestStealthSystemFonts walks every FontID to keep the two the same.
const char* const kStealthSystemFontPrefs[] = {
    "ui.font.caption",         "ui.font.icon",
    "ui.font.menu",            "ui.font.message-box",
    "ui.font.small-caption",   "ui.font.status-bar",
    "ui.font.-moz-pull-down-menu", "ui.font.-moz-button",
    "ui.font.-moz-list",       "ui.font.-moz-field",
};
const size_t kStealthSystemFontPrefCount = std::size(kStealthSystemFontPrefs);

bool StealthFirstUndeclaredSystemFont(nsACString& aPrefName) {
  for (const char* name : kStealthSystemFontPrefs) {
    nsAutoString family;
    nsAutoCString sizePref(name);
    sizePref.AppendLiteral(".size");
    const bool hasFamily =
        NS_SUCCEEDED(Preferences::GetString(name, family)) &&
        !family.IsEmpty();
    if (!hasFamily) {
      aPrefName.Assign(name);
      return true;
    }
    if (!StealthSystemFontDeclared(true,
                                   Preferences::GetFloat(sizePref.get()))) {
      aPrefName = sizePref;
      return true;
    }
  }
  return false;
}

const char* const kStealthSystemColorPrefs[] = {
    "ui.highlight",       "ui.highlighttext",
    "ui.mark",            "ui.marktext",
    "ui.selecteditem",    "ui.selecteditemtext",
    "ui.accentcolor",     "ui.accentcolortext",
    "ui.-moz-field",      "ui.-moz-fieldtext",
    "ui.-moz-hyperlinktext", "ui.-moz-visitedhyperlinktext",
    "ui.-moz-activehyperlinktext",
};
const size_t kStealthSystemColorPrefCount =
    std::size(kStealthSystemColorPrefs);

bool StealthFirstUndeclaredSystemColor(nsACString& aPrefName) {
  for (const char* name : kStealthSystemColorPrefs) {
    nsAutoCString value;
    if (NS_FAILED(Preferences::GetCString(name, value)) || value.IsEmpty()) {
      aPrefName.Assign(name);
      return true;
    }
  }
  return false;
}

Maybe<nscolor> StealthDarkSystemColor(LookAndFeel::ColorID aID,
                                      ColorScheme aScheme) {
  if (aScheme == ColorScheme::Dark &&
      aID == LookAndFeel::ColorID::Highlighttext &&
      gfx::StealthEngineActive()) {
    return Some(NS_SAME_AS_FOREGROUND_COLOR);
  }
  return Nothing();
}

bool StealthFirstUndeclaredSystemTheme(nsACString& aPrefName) {
  return StealthFirstUndeclaredSystemFont(aPrefName) ||
         StealthFirstUndeclaredSystemColor(aPrefName);
}

void StealthVerifySystemTheme() {
#ifndef MOZ_WIDGET_GTK
  return;  // Windows and macOS compute these themselves; Linux is the host leak
#else
  if (!gfx::StealthEngineActive() || !XRE_IsParentProcess()) {
    return;
  }
  nsAutoCString name;
  if (StealthFirstUndeclaredSystemTheme(name)) {
    gfx::StealthRefuse(
        "stealth: %s is not declared. Engine rule 7 - the system fonts and "
        "colours live only in invisible_core, and a missing declaration "
        "refuses rather than asking the toolkit. Launch through "
        "invisible_core, or clear zoom.stealth.fpp.hw_seed to run as stock "
        "Firefox.",
        name.get());
  }
#endif
}

// Whether the last FillStores ran with the engine already on.
static Atomic<bool> sStoresFilledWithEngineOn(false);
static Atomic<bool> sStoresFilled(false);

void StealthSystemThemeFillStores() {
  StealthVerifySystemTheme();
  sStoresFilledWithEngineOn = gfx::StealthEngineActive();
  sStoresFilled = true;
}

void StealthSystemThemeAfterUserPrefs() {
  if (!gfx::StealthEngineActive()) {
    return;
  }
  StealthVerifySystemTheme();
  if (sStoresFilled && !sStoresFilledWithEngineOn) {
    LookAndFeel::Refresh();
  }
}

bool StealthNativeSystemFont(LookAndFeel::FontID aID, nsString& aFontName,
                             gfxFontStyle& aFontStyle) {
  // GetFontValue has already read the pref and found it absent.
  if (gfx::StealthEngineActive()) {
    gfx::StealthRefuse(
        "stealth: %s is not declared. Engine rule 7 - the system font lives "
        "only in invisible_core, and a missing declaration refuses rather "
        "than asking the toolkit. Launch through invisible_core, or clear "
        "zoom.stealth.fpp.hw_seed to run as stock Firefox.",
        kStealthSystemFontPrefs[size_t(aID)]);
  }
  return false;
}

}  // namespace mozilla::widget
