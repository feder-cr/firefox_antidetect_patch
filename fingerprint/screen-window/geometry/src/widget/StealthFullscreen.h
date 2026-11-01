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

}  // namespace mozilla::widget

#endif  // mozilla_widget_StealthFullscreen_h
