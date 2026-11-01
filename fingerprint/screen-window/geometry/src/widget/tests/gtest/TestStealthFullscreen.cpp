/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The rectangle a fullscreen window fills (widget/StealthFullscreen.cpp): the
// declared screen through the window's scales, never the host's monitor.
// Retail Windows at 100% fills 1920x1080 device pixels for a 1920x1080 CSS
// screen, and at 125% the same 1920x1080 for a 1536x864 CSS screen.

#include "StealthFullscreen.h"
#include "gtest/gtest.h"

using namespace mozilla;
using widget::StealthFullscreenRect;

namespace {

Maybe<CSSIntRect> DeclaredScreen(int32_t aWidth, int32_t aHeight) {
  return Some(CSSIntRect(0, 0, aWidth, aHeight));
}

}  // namespace

TEST(StealthFullscreen, TheDeclaredScreenAtTheDeclaredScale)
{
  // 100%: one CSS pixel is one device pixel is one desktop pixel.
  EXPECT_EQ(StealthFullscreenRect(DeclaredScreen(1920, 1080), CSSToLayoutDeviceScale(1.0f),
                                  DesktopToLayoutDeviceScale(1.0f),
                                  DesktopPoint(0, 0)),
            Some(DesktopRect(0, 0, 1920, 1080)));
  // 125% (layout.css.devPixelsPerPx): a 1536x864 CSS screen is a 1920x1080
  // window, on a desktop that counts device pixels (Windows, X11).
  EXPECT_EQ(StealthFullscreenRect(DeclaredScreen(1536, 864), CSSToLayoutDeviceScale(1.25f),
                                  DesktopToLayoutDeviceScale(1.0f),
                                  DesktopPoint(0, 0)),
            Some(DesktopRect(0, 0, 1920, 1080)));
  // A desktop that counts in scaled pixels gets the same window in its units.
  EXPECT_EQ(StealthFullscreenRect(DeclaredScreen(1536, 864), CSSToLayoutDeviceScale(1.25f),
                                  DesktopToLayoutDeviceScale(1.25f),
                                  DesktopPoint(0, 0)),
            Some(DesktopRect(0, 0, 1536, 864)));
}

TEST(StealthFullscreen, PlacedOnTheScreenTheWindowIsOn)
{
  // A second monitor to the right of the first: the window fills it from its
  // own origin, with the declared size, not that monitor's size.
  EXPECT_EQ(StealthFullscreenRect(DeclaredScreen(1920, 1080), CSSToLayoutDeviceScale(1.0f),
                                  DesktopToLayoutDeviceScale(1.0f),
                                  DesktopPoint(2560, 0)),
            Some(DesktopRect(2560, 0, 1920, 1080)));
}

TEST(StealthFullscreen, NothingWithoutADeclaredScreenOrAScale)
{
  EXPECT_EQ(StealthFullscreenRect(Nothing(), CSSToLayoutDeviceScale(1.0f),
                                  DesktopToLayoutDeviceScale(1.0f),
                                  DesktopPoint(0, 0)),
            Nothing());
  EXPECT_EQ(StealthFullscreenRect(DeclaredScreen(0, 0), CSSToLayoutDeviceScale(1.0f),
                                  DesktopToLayoutDeviceScale(1.0f),
                                  DesktopPoint(0, 0)),
            Nothing());
  EXPECT_EQ(StealthFullscreenRect(DeclaredScreen(1920, 1080), CSSToLayoutDeviceScale(0.0f),
                                  DesktopToLayoutDeviceScale(1.0f),
                                  DesktopPoint(0, 0)),
            Nothing());
  EXPECT_EQ(StealthFullscreenRect(DeclaredScreen(1920, 1080), CSSToLayoutDeviceScale(1.0f),
                                  DesktopToLayoutDeviceScale(0.0f),
                                  DesktopPoint(0, 0)),
            Nothing());
}

// Windows' own rules for a fullscreen window (StealthFullscreenWin.cpp). Run
// 41325657085: on the GPU-less CI desktop (a 1024x768 monitor) the window of a
// persona with a 1280x720 or 1920x1080 screen went fullscreen at 1024x768,
// because OnWindowPosChanging forced it onto the monitor and the size was
// clamped to about the monitor's.
TEST(StealthFullscreen, AFullscreenMoveAndResizeLandsOnTheDeclaredRect)
{
  const auto declared = Some(LayoutDeviceIntRect(0, 0, 1920, 1080));
  int32_t x = 0, y = 0, w = 1024, h = 768;
  widget::StealthHeldFullscreenPosition(declared, true, true, true, &x, &y, &w,
                                        &h);
  EXPECT_EQ(LayoutDeviceIntRect(x, y, w, h), *declared);

  // A message that only moves, or only resizes, is left as it is.
  w = 1024;
  h = 768;
  widget::StealthHeldFullscreenPosition(declared, true, true, false, &x, &y, &w,
                                        &h);
  EXPECT_EQ(w, 1024);
  widget::StealthHeldFullscreenPosition(declared, true, false, true, &x, &y, &w,
                                        &h);
  EXPECT_EQ(h, 768);
  // Outside fullscreen, or without a declaration, Mozilla's rect stands.
  widget::StealthHeldFullscreenPosition(declared, false, true, true, &x, &y, &w,
                                        &h);
  EXPECT_EQ(w, 1024);
  widget::StealthHeldFullscreenPosition(Nothing(), true, true, true, &x, &y, &w,
                                        &h);
  EXPECT_EQ(w, 1024);
}

TEST(StealthFullscreen, TheLargestSizeGrowsToTheDeclaredScreenInFullscreen)
{
  const auto declared = Some(LayoutDeviceIntRect(0, 0, 1920, 1080));
  int32_t w = 1040, h = 784;  // about a 1024x768 monitor plus frame
  widget::StealthFullscreenMaxTrackSize(declared, true, &w, &h);
  EXPECT_EQ(w, 1920);
  EXPECT_EQ(h, 1080);
  // Never smaller than Windows' own limit.
  w = 2600;
  h = 1500;
  widget::StealthFullscreenMaxTrackSize(declared, true, &w, &h);
  EXPECT_EQ(w, 2600);
  EXPECT_EQ(h, 1500);
  // Outside fullscreen the limit is Windows'.
  w = 1040;
  h = 784;
  widget::StealthFullscreenMaxTrackSize(declared, false, &w, &h);
  EXPECT_EQ(w, 1040);
  widget::StealthFullscreenMaxTrackSize(Nothing(), true, &w, &h);
  EXPECT_EQ(w, 1040);
}
