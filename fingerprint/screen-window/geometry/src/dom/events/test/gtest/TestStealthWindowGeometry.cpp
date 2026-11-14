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

// An out-of-process iframe that starts before the parent sent its dimensions
// has a real outer size of 0x0, and inner 0x0 with it. Retail answers 0x0 for
// outerWidth/outerHeight there; the declared window (1936x1048) only stands in
// for a real window that has a size. Before this the declaration was put over
// the 0x0, and a page read outer 1936x1048 around inner 0x0.
TEST(StealthWindowGeometry, ARealZeroOuterSizeStaysZero)
{
  EXPECT_FALSE(gfx::StealthOuterSizeIsDeclared(LayoutDeviceIntSize(0, 0)));
}

TEST(StealthWindowGeometry, AnyOtherRealOuterSizeIsStillDeclared)
{
  // The states outside the zero one are unchanged: the declaration replaces
  // the real size, whatever it is (a headless widget, a popup, a main window).
  EXPECT_TRUE(gfx::StealthOuterSizeIsDeclared(LayoutDeviceIntSize(1280, 1024)));
  EXPECT_TRUE(gfx::StealthOuterSizeIsDeclared(LayoutDeviceIntSize(500, 400)));
  EXPECT_TRUE(gfx::StealthOuterSizeIsDeclared(LayoutDeviceIntSize(1, 1)));
  EXPECT_TRUE(gfx::StealthOuterSizeIsDeclared(LayoutDeviceIntSize(0, 600)));
  EXPECT_TRUE(gfx::StealthOuterSizeIsDeclared(LayoutDeviceIntSize(800, 0)));
}

// window.outerWidth/outerHeight of the MAIN window around its content area
// (gfx::StealthMainWindowOuterSize). Page.setViewportSize makes the content
// area smaller than the maximized window's; until 2026-11-14 the window stayed
// the maximized one, so a 390x844 content area read outer 1936x1048 at 100%:
// outerWidth - innerWidth 1546 where every Windows Firefox answers 16.

namespace {

// The maximized window of a work area, as StealthDeclaredMaximizedWindow
// builds it: a border off the screen on every side.
LayoutDeviceIntRect Maximized(int32_t aAvailWidth, int32_t aAvailHeight,
                              const gfx::StealthWindowFrame& aFrame) {
  const int32_t b = aFrame.mBorder;
  return LayoutDeviceIntRect(-b, -b, aAvailWidth + 2 * b, aAvailHeight + 2 * b);
}

// The content area the page reads in that maximized window, in CSS pixels,
// rounded as Gecko rounds (and as invisible_core declares the viewport).
CSSIntSize MaximizedContent(int32_t aAvailWidth, int32_t aAvailHeight,
                            const gfx::StealthWindowFrame& aFrame,
                            float aScale) {
  return CSSIntSize(
      int32_t(float(aAvailWidth) / aScale + 0.5f),
      int32_t(float(aAvailHeight - aFrame.mMaximizedUi) / aScale + 0.5f));
}

// Retail Windows 11 frames, per scale (invisible_core WINDOW_FRAME_BY_DPR),
// with a 1920x1080 panel and its 48 CSS taskbar at each scale.
struct Scale {
  float mScale;
  gfx::StealthWindowFrame mFrame;
  int32_t mAvailHeight;
};
const Scale kScales[] = {
    {1.0f, {8, 85, 72, 4}, 1080 - 48},
    {1.25f, {9, 107, 90, 4}, 1080 - 60},
    {1.5f, {11, 127, 106, 4}, 1080 - 72},
    {2.0f, {13, 169, 139, 4}, 1080 - 96},
};

}  // namespace

TEST(StealthWindowGeometry, TheMaximizedContentAreaIsTheMaximizedWindow)
{
  const gfx::StealthWindowFrame frame{8, 85, 72, 4};
  EXPECT_EQ(gfx::StealthMainWindowOuterSize(Maximized(1920, 1032, frame), frame,
                                            1.0f, CSSIntSize(1920, 947)),
            CSSIntSize(1936, 1048));
}

TEST(StealthWindowGeometry, AWindowWithNoContentSizeYetIsTheMaximizedOne)
{
  // The synced inner size is 0x0 until the first layout: a window that has
  // not been sized must not read 16x101.
  const gfx::StealthWindowFrame frame{8, 85, 72, 4};
  EXPECT_EQ(gfx::StealthMainWindowOuterSize(Maximized(1920, 1032, frame), frame,
                                            1.0f, CSSIntSize(0, 0)),
            CSSIntSize(1936, 1048));
}

TEST(StealthWindowGeometry, ASmallerContentAreaKeepsTheMaximizedFrame)
{
  // 100%: the 390x844 check that read 1936x1048 reads 406x945.
  const gfx::StealthWindowFrame frame{8, 85, 72, 4};
  EXPECT_EQ(gfx::StealthMainWindowOuterSize(Maximized(1920, 1032, frame), frame,
                                            1.0f, CSSIntSize(390, 844)),
            CSSIntSize(390 + 16, 844 + 101));

  // At every measured scale, any content area reads outer - inner exactly as
  // the maximized window does.
  const CSSIntSize sizes[] = {{390, 844}, {1024, 768}, {1440, 700},
                              {200, 200}, {1, 1}};
  for (const Scale& s : kScales) {
    const LayoutDeviceIntRect maximized =
        Maximized(1920, s.mAvailHeight, s.mFrame);
    const CSSIntSize content =
        MaximizedContent(1920, s.mAvailHeight, s.mFrame, s.mScale);
    const CSSIntSize frameOfMaximized =
        gfx::StealthMainWindowOuterSize(maximized, s.mFrame, s.mScale,
                                        content) -
        content;
    for (const CSSIntSize& inner : sizes) {
      EXPECT_EQ(gfx::StealthMainWindowOuterSize(maximized, s.mFrame, s.mScale,
                                                inner) -
                    inner,
                frameOfMaximized)
          << "scale " << s.mScale << ", content " << inner.width << "x"
          << inner.height;
    }
  }
}
