/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthFullscreen.h"

#include <algorithm>

#include "StealthDeclarationGate.h"
#include "mozilla/widget/Screen.h"
#include "nsCOMPtr.h"
#include "nsIScreen.h"
#include "nsIWidget.h"

namespace mozilla::widget {

// A fullscreen window is exactly the screen: retail's outer == inner ==
// screen.width x screen.height. Firefox sizes it to the HOST's monitor
// (nsIScreen::GetRectDisplayPix), so a page read the machine's display in
// fullscreen - innerWidth 2560 on a 1440p host behind a declared 1920x1080
// screen, or the size of a CI desktop. The declared screen is in CSS pixels,
// as the page reads it; the window is sized in device pixels, so it goes
// through the window's own CSS scale (the declared device pixel ratio:
// 1536x864 CSS at 125% is a 1920x1080 window) and then to desktop pixels.
Maybe<DesktopRect> StealthFullscreenRect(
    const Maybe<CSSIntRect>& aDeclaredScreen,
    CSSToLayoutDeviceScale aCSSToDevice,
    DesktopToLayoutDeviceScale aDesktopToDevice,
    const DesktopPoint& aScreenOrigin) {
  if (!aDeclaredScreen || aDeclaredScreen->IsEmpty() ||
      aCSSToDevice.scale <= 0.0f || aDesktopToDevice.scale <= 0.0f) {
    return Nothing();
  }
  const LayoutDeviceRect device(
      0.0f, 0.0f, float(aDeclaredScreen->Width()) * aCSSToDevice.scale,
      float(aDeclaredScreen->Height()) * aCSSToDevice.scale);
  DesktopRect rect = device / aDesktopToDevice;
  rect.MoveTo(aScreenOrigin);
  return Some(rect);
}

Maybe<DesktopRect> StealthDeclaredFullscreenRect(nsIWidget& aWidget) {
  if (!gfx::StealthEngineActive()) {
    return Nothing();
  }
  DesktopPoint origin;
  nsCOMPtr<nsIScreen> screen = aWidget.GetWidgetScreen();
  if (screen) {
    origin = DesktopRect(screen->GetRectDisplayPix()).TopLeft();
  }
  return StealthFullscreenRect(gfx::StealthDeclaredScreenRect(),
                               aWidget.GetDefaultScale(),
                               aWidget.GetDesktopToDeviceScale(), origin);
}

Maybe<LayoutDeviceIntRect> StealthDeclaredFullscreenDeviceRect(
    nsIWidget& aWidget) {
  const Maybe<DesktopRect> rect = StealthDeclaredFullscreenRect(aWidget);
  if (!rect) {
    return Nothing();
  }
  return Some(LayoutDeviceIntRect::Round(*rect *
                                         aWidget.GetDesktopToDeviceScale()));
}

void StealthHeldFullscreenPosition(const Maybe<LayoutDeviceIntRect>& aDeclared,
                                   bool aFullscreen, bool aMoves, bool aSizes,
                                   int32_t* aX, int32_t* aY, int32_t* aWidth,
                                   int32_t* aHeight) {
  if (!aDeclared || !aFullscreen || !aMoves || !aSizes) {
    return;
  }
  *aX = aDeclared->x;
  *aY = aDeclared->y;
  *aWidth = aDeclared->width;
  *aHeight = aDeclared->height;
}

void StealthFullscreenMaxTrackSize(const Maybe<LayoutDeviceIntRect>& aDeclared,
                                   bool aFullscreen, int32_t* aMaxWidth,
                                   int32_t* aMaxHeight) {
  if (!aDeclared || !aFullscreen) {
    return;
  }
  *aMaxWidth = std::max(*aMaxWidth, aDeclared->width);
  *aMaxHeight = std::max(*aMaxHeight, aDeclared->height);
}

}  // namespace mozilla::widget
