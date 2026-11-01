/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_widget_StealthFullscreen_h
#define mozilla_widget_StealthFullscreen_h

// The rectangle a fullscreen window fills: the DECLARED screen, not the host's
// monitor. Read by nsIWidget::InfallibleMakeFullScreen (Windows, headless) and
// by the GTK widget when no window manager can make it fullscreen
// (StealthFullscreenGtk.cpp).

#include "Units.h"
#include "mozilla/Maybe.h"

class nsIWidget;

namespace mozilla::widget {

// The declared screen (CSS pixels, at the origin) times the CSS to device
// scale, in desktop pixels, placed at aScreenOrigin. Nothing() without a
// declared screen or with a scale that is not positive.
Maybe<DesktopRect> StealthFullscreenRect(
    const Maybe<CSSIntRect>& aDeclaredScreen,
    CSSToLayoutDeviceScale aCSSToDevice,
    DesktopToLayoutDeviceScale aDesktopToDevice,
    const DesktopPoint& aScreenOrigin);

// The fullscreen rectangle of aWidget's window, from the declaration, at the
// origin of the screen the widget is on. Nothing() with the engine off.
Maybe<DesktopRect> StealthDeclaredFullscreenRect(nsIWidget& aWidget);

// The same rectangle in device pixels, rounded as Windows sizes a window.
Maybe<LayoutDeviceIntRect> StealthDeclaredFullscreenDeviceRect(
    nsIWidget& aWidget);

// What Windows may do to a fullscreen window (StealthFullscreenWin.h), as pure
// rules. A move-and-resize of a fullscreen window lands on the declared
// rectangle; the largest size the window may take grows to it. Nothing
// changes outside fullscreen, without a declaration, or for a message that
// does not both move and resize.
void StealthHeldFullscreenPosition(const Maybe<LayoutDeviceIntRect>& aDeclared,
                                   bool aFullscreen, bool aMoves, bool aSizes,
                                   int32_t* aX, int32_t* aY, int32_t* aWidth,
                                   int32_t* aHeight);
void StealthFullscreenMaxTrackSize(const Maybe<LayoutDeviceIntRect>& aDeclared,
                                   bool aFullscreen, int32_t* aMaxWidth,
                                   int32_t* aMaxHeight);

}  // namespace mozilla::widget

#endif  // mozilla_widget_StealthFullscreen_h
