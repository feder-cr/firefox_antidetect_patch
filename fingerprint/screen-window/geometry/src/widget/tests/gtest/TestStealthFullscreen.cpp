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
