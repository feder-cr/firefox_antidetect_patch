/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The declared content origin (gfx::StealthDeclaredOriginShift), which
// mozInnerScreenX/Y and every mouse event's screenX/Y follow. A window in
// fullscreen is the declared screen: its content starts at (0, 0), as on
// retail. Before 2026-11-01 the main window kept the maximized window's 85
// device pixels of UI after requestFullscreen().

#include "StealthDeclarationGate.h"
#include "Units.h"
#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/ScopeExit.h"

using namespace mozilla;

namespace {

constexpr int32_t kAuPerDevPx = 60;
const char* const kPrefs[] = {
    "zoom.stealth.fpp.hw_seed",           "zoom.stealth.screen.frame_border",
    "zoom.stealth.screen.maximized_ui",   "zoom.stealth.screen.popup_ui",
    "zoom.stealth.screen.popup_position",
};

// A declared window frame like retail's at 100%, the engine on.
void Declare() {
  Preferences::SetInt(kPrefs[0], 7);
  Preferences::SetInt(kPrefs[1], 8);
  Preferences::SetInt(kPrefs[2], 85);
  Preferences::SetInt(kPrefs[3], 62);
  Preferences::SetInt(kPrefs[4], 26);
}

nsPoint Declared(const Maybe<LayoutDeviceIntPoint>& aPopup, bool aFullscreen) {
  // A real origin of zero: the shift is then the declared origin itself.
  const Maybe<nsPoint> shift = gfx::StealthDeclaredOriginShift(
      nsPoint(0, 0), aPopup, aFullscreen, kAuPerDevPx);
  EXPECT_TRUE(shift);
  return shift ? *shift : nsPoint(-1, -1);
}

}  // namespace

TEST(StealthWindowGeometry, FullscreenContentStartsAtTheScreenOrigin)
{
  auto restore = MakeScopeExit([] {
    for (const char* pref : kPrefs) {
      Preferences::ClearUser(pref);
    }
  });
  Declare();
  // The main window in fullscreen: no UI above the content.
  EXPECT_EQ(Declared(Nothing(), true), nsPoint(0, 0));
  // A popup in fullscreen too, wherever it was.
  EXPECT_EQ(Declared(Some(LayoutDeviceIntPoint(300, 200)), true),
            nsPoint(0, 0));
}

TEST(StealthWindowGeometry, OutsideFullscreenNothingChanges)
{
  auto restore = MakeScopeExit([] {
    for (const char* pref : kPrefs) {
      Preferences::ClearUser(pref);
    }
  });
  Declare();
  // The maximized main window: the UI height below the screen top.
  EXPECT_EQ(Declared(Nothing(), false), nsPoint(0, 85 * kAuPerDevPx));
  // A popup: a border in, the popup UI down from where it really is.
  EXPECT_EQ(Declared(Some(LayoutDeviceIntPoint(300, 200)), false),
            nsPoint((300 + 8) * kAuPerDevPx, (200 + 62) * kAuPerDevPx));
  // The shift is relative to the real origin, so frames keep their offset.
  const Maybe<nsPoint> shift = gfx::StealthDeclaredOriginShift(
      nsPoint(10 * kAuPerDevPx, 20 * kAuPerDevPx), Nothing(), true,
      kAuPerDevPx);
  ASSERT_TRUE(shift);
  EXPECT_EQ(*shift, nsPoint(-10 * kAuPerDevPx, -20 * kAuPerDevPx));
}
