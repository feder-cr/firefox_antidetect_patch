/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The CSS system fonts come from the ui.font.* prefs invisible_core declares
// (widget/StealthSystemFonts.cpp), on every host: a page reading `font: menu`
// gets the declared family and size, never the toolkit's. Retail Windows
// answers Segoe UI at 12px for the shell fonts and "MS Shell Dlg 2" for the
// three form-control fonts.

#include "StealthSystemFonts.h"

#include <string.h>

#include <vector>

#include "gtest/gtest.h"
#include "mozilla/gtest/MozHelpers.h"
#include "mozilla/LookAndFeel.h"
#include "mozilla/Preferences.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/ColorScheme.h"
#include "nsColor.h"

using namespace mozilla;
using LookAndFeel = mozilla::LookAndFeel;
using FontID = LookAndFeel::FontID;

namespace {

// The tables are extern arrays of unknown bound in the header: their size is
// kStealth...PrefCount, so a range over them goes through these.
std::vector<const char*> FontPrefs() {
  return {widget::kStealthSystemFontPrefs,
          widget::kStealthSystemFontPrefs + widget::kStealthSystemFontPrefCount};
}

std::vector<const char*> ColorPrefs() {
  return {widget::kStealthSystemColorPrefs,
          widget::kStealthSystemColorPrefs + widget::kStealthSystemColorPrefCount};
}

void Declare(const char* aPref, const char* aFamily, const char* aSize) {
  nsAutoCString size(aPref);
  size.AppendLiteral(".size");
  Preferences::SetCString(aPref, aFamily);
  Preferences::SetCString(size.get(), aSize);
}

void Undeclare(const char* aPref) {
  nsAutoCString size(aPref);
  size.AppendLiteral(".size");
  Preferences::ClearUser(aPref);
  Preferences::ClearUser(size.get());
}

}  // namespace

TEST(StealthSystemFonts, OnePrefPerFontID)
{
  EXPECT_EQ(widget::kStealthSystemFontPrefCount, size_t(FontID::End));
}

TEST(StealthSystemFonts, GetFontReturnsTheDeclaredValues)
{
  // A family and a size no host has, different per font, so a value read from
  // the toolkit or from another id cannot pass.
  size_t i = 0;
  for (const char* pref : FontPrefs()) {
    nsAutoCString family("Declared Family ");
    family.AppendInt(int32_t(i));
    nsAutoCString size;
    size.AppendInt(int32_t(11 + i));
    Declare(pref, family.get(), size.get());
    i++;
  }
  LookAndFeel::Refresh();
  i = 0;
  for (FontID id : MakeEnumeratedRange(FontID::End)) {
    nsString name;
    gfxFontStyle style;
    ASSERT_TRUE(LookAndFeel::GetFont(id, name, style));
    nsAutoCString expected("Declared Family ");
    expected.AppendInt(int32_t(i));
    EXPECT_TRUE(NS_ConvertUTF16toUTF8(name).Equals(expected));
    EXPECT_FLOAT_EQ(style.size, float(11 + i));
    EXPECT_EQ(style.weight, FontWeight::NORMAL);
    EXPECT_TRUE(style.style.IsNormal());
    EXPECT_TRUE(style.systemFont);
    i++;
  }
  for (const char* pref : FontPrefs()) {
    Undeclare(pref);
  }
}

TEST(StealthSystemFonts, AnUndeclaredFontIsNamed)
{
  for (const char* pref : FontPrefs()) {
    Declare(pref, "Segoe UI", "12");
  }
  nsAutoCString missing;
  EXPECT_FALSE(widget::StealthFirstUndeclaredSystemFont(missing));

  Preferences::ClearUser("ui.font.menu");
  EXPECT_TRUE(widget::StealthFirstUndeclaredSystemFont(missing));
  EXPECT_TRUE(missing.EqualsLiteral("ui.font.menu"));

  Declare("ui.font.menu", "Segoe UI", "12");
  Preferences::SetCString("ui.font.status-bar.size", "0");
  EXPECT_TRUE(widget::StealthFirstUndeclaredSystemFont(missing));
  EXPECT_TRUE(missing.EqualsLiteral("ui.font.status-bar.size"));

  for (const char* pref : FontPrefs()) {
    Undeclare(pref);
  }
}

// The system colours a page reads and no stand-in covers come from the declared
// palette, in both colour schemes, never from the GTK theme of the host.
TEST(StealthSystemColors, GetColorReturnsTheDeclaredPalette)
{
  struct Row {
    LookAndFeel::ColorID mID;
    const char* mPref;
    nscolor mLight;
    nscolor mDark;
  };
  // Values no GTK theme has (GTK's Highlight is rgb(53,132,228)).
  const Row rows[] = {
      {LookAndFeel::ColorID::Highlight, "ui.highlight", NS_RGB(1, 2, 3),
       NS_RGB(4, 5, 6)},
      {LookAndFeel::ColorID::Highlighttext, "ui.highlighttext",
       NS_RGB(7, 8, 9), NS_RGB(10, 11, 12)},
      {LookAndFeel::ColorID::Mark, "ui.mark", NS_RGB(13, 14, 15),
       NS_RGB(16, 17, 18)},
      {LookAndFeel::ColorID::Marktext, "ui.marktext", NS_RGB(19, 20, 21),
       NS_RGB(22, 23, 24)},
      {LookAndFeel::ColorID::Selecteditem, "ui.selecteditem",
       NS_RGB(25, 26, 27), NS_RGB(28, 29, 30)},
      {LookAndFeel::ColorID::Selecteditemtext, "ui.selecteditemtext",
       NS_RGB(31, 32, 33), NS_RGB(34, 35, 36)},
      {LookAndFeel::ColorID::Accentcolor, "ui.accentcolor", NS_RGB(37, 38, 39),
       NS_RGB(40, 41, 42)},
      {LookAndFeel::ColorID::Accentcolortext, "ui.accentcolortext",
       NS_RGB(43, 44, 45), NS_RGB(46, 47, 48)},
      {LookAndFeel::ColorID::Field, "ui.-moz-field", NS_RGB(67, 68, 69),
       NS_RGB(70, 71, 72)},
      {LookAndFeel::ColorID::Fieldtext, "ui.-moz-fieldtext", NS_RGB(73, 74, 75),
       NS_RGB(76, 77, 78)},
      {LookAndFeel::ColorID::Linktext, "ui.-moz-hyperlinktext",
       NS_RGB(49, 50, 51), NS_RGB(52, 53, 54)},
      {LookAndFeel::ColorID::Visitedtext, "ui.-moz-visitedhyperlinktext",
       NS_RGB(55, 56, 57), NS_RGB(58, 59, 60)},
      {LookAndFeel::ColorID::Activetext, "ui.-moz-activehyperlinktext",
       NS_RGB(61, 62, 63), NS_RGB(64, 65, 66)},
  };
  auto css = [](nscolor aColor) {
    nsAutoCString s;
    s.AppendPrintf("rgb(%d, %d, %d)", NS_GET_R(aColor), NS_GET_G(aColor),
                   NS_GET_B(aColor));
    return s;
  };
  for (const Row& r : rows) {
    Preferences::SetCString(r.mPref, css(r.mLight));
    nsAutoCString dark(r.mPref);
    dark.AppendLiteral(".dark");
    Preferences::SetCString(dark.get(), css(r.mDark));
  }
  LookAndFeel::Refresh();
  for (const Row& r : rows) {
    auto light = LookAndFeel::GetColor(r.mID, ColorScheme::Light,
                                       LookAndFeel::UseStandins::No);
    auto dark = LookAndFeel::GetColor(r.mID, ColorScheme::Dark,
                                      LookAndFeel::UseStandins::No);
    ASSERT_TRUE(light.isSome()) << r.mPref;
    ASSERT_TRUE(dark.isSome()) << r.mPref;
    EXPECT_EQ(*light, r.mLight) << r.mPref;
    EXPECT_EQ(*dark, r.mDark) << r.mPref;
  }

  // Every one of them is also what the gate asks for, and a missing one is named.
  nsAutoCString missing;
  EXPECT_FALSE(widget::StealthFirstUndeclaredSystemColor(missing));
  Preferences::ClearUser("ui.marktext");
  EXPECT_TRUE(widget::StealthFirstUndeclaredSystemColor(missing));
  EXPECT_TRUE(missing.EqualsLiteral("ui.marktext"));

  for (const Row& r : rows) {
    Preferences::ClearUser(r.mPref);
    nsAutoCString dark(r.mPref);
    dark.AppendLiteral(".dark");
    Preferences::ClearUser(dark.get());
  }
}

#ifdef MOZ_WIDGET_GTK
// With the engine on, a missing declaration refuses; it never reaches GTK or
// the headless defaults. Death tests: the old code answered from the toolkit
// and returned.
namespace {

// A death test's child is a fresh process (threadsafe: the parent has dozens of threads, and
// a fork of it can wait for a lock one of them held), and a refusal there is not a crash
// to report.
void ThreadsafeDeathTests() { GTEST_FLAG_SET(death_test_style, "threadsafe"); }

void RefusingChild(void (*aCall)()) {
  mozilla::gtest::DisableCrashReporter();
  aCall();
}

void EngineOn() { Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7); }
void EngineOff() { Preferences::ClearUser("zoom.stealth.fpp.hw_seed"); }

void DeclareAllExcept(const char* aSkip) {
  for (const char* pref : FontPrefs()) {
    if (strcmp(pref, aSkip)) Declare(pref, "Segoe UI", "12");
  }
  for (const char* pref : ColorPrefs()) {
    if (strcmp(pref, aSkip)) Preferences::SetCString(pref, "rgb(1, 2, 3)");
  }
}

void ClearAll() {
  for (const char* pref : FontPrefs()) Undeclare(pref);
  for (const char* pref : ColorPrefs()) {
    Preferences::ClearUser(pref);
  }
}

}  // namespace

TEST(StealthSystemTheme, AMissingColourRefusesWithTheEngineOn)
{
  ThreadsafeDeathTests();
  DeclareAllExcept("ui.highlight");
  EngineOn();
  ASSERT_DEATH_IF_SUPPORTED(RefusingChild(widget::StealthVerifySystemTheme),
                            "ui.highlight is not declared");
  // The same through the store fill that every LookAndFeel goes through (GTK
  // or headless): no value is read from the toolkit first.
  ASSERT_DEATH_IF_SUPPORTED(RefusingChild(LookAndFeel::Refresh),
                            "ui.highlight is not declared");
  EngineOff();
  ClearAll();
}

TEST(StealthSystemTheme, AMissingFontRefusesWithTheEngineOn)
{
  ThreadsafeDeathTests();
  DeclareAllExcept("ui.font.status-bar");
  EngineOn();
  ASSERT_DEATH_IF_SUPPORTED(RefusingChild(LookAndFeel::Refresh),
                            "ui.font.status-bar is not declared");
  EngineOff();
  ClearAll();
}

TEST(StealthSystemTheme, ABareBinaryKeepsTheToolkitAnswer)
{
  ClearAll();
  EngineOff();
  widget::StealthVerifySystemTheme();  // must return
  SUCCEED();
}

// Retail's dark HighlightText has no pref: it keeps the colour of its parent
// (NS_SAME_AS_FOREGROUND_COLOR). A declared ui.highlighttext(.dark) must not
// turn it into a fixed colour; the light one stays the declared white.
TEST(StealthSystemColors, DarkHighlightTextFollowsTheForeground)
{
  DeclareAllExcept("");
  EngineOn();
  Preferences::SetCString("ui.highlighttext", "rgb(255, 255, 255)");
  Preferences::SetCString("ui.highlighttext.dark", "rgb(0, 0, 0)");
  Preferences::SetCString("ui.highlight", "rgb(1, 2, 3)");
  Preferences::SetCString("ui.highlight.dark", "rgb(4, 5, 6)");

  LookAndFeel::Refresh();
  using Id = LookAndFeel::ColorID;
  auto uncached = [](Id aID, ColorScheme aScheme) {
    return LookAndFeel::GetColor(aID, aScheme, LookAndFeel::UseStandins::No);
  };
  EXPECT_EQ(uncached(Id::Highlighttext, ColorScheme::Dark),
            Some(NS_SAME_AS_FOREGROUND_COLOR));
  EXPECT_EQ(uncached(Id::Highlighttext, ColorScheme::Light),
            Some(NS_RGB(255, 255, 255)));
  EXPECT_EQ(uncached(Id::Highlight, ColorScheme::Light), Some(NS_RGB(1, 2, 3)));
  EXPECT_EQ(uncached(Id::Highlight, ColorScheme::Dark), Some(NS_RGB(4, 5, 6)));

  // A bare binary keeps Mozilla's rule: the declared pref wins.
  EngineOff();
  EXPECT_TRUE(widget::StealthDarkSystemColor(Id::Highlighttext,
                                             ColorScheme::Dark).isNothing());

  for (const char* pref : {"ui.highlighttext.dark", "ui.highlight.dark"}) {
    Preferences::ClearUser(pref);
  }
  ClearAll();
}

#endif  // MOZ_WIDGET_GTK
