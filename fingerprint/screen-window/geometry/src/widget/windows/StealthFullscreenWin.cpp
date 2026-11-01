/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthFullscreenWin.h"

#include "StealthFullscreen.h"
#include "nsIWidget.h"

namespace mozilla::widget {

void StealthHoldDeclaredFullscreen(nsIWidget& aWidget, bool aFullscreen,
                                   WINDOWPOS* aPos) {
  Maybe<LayoutDeviceIntRect> declared;
  if (aFullscreen) {
    declared = StealthDeclaredFullscreenDeviceRect(aWidget);
  }
  int32_t x = aPos->x, y = aPos->y, cx = aPos->cx, cy = aPos->cy;
  StealthHeldFullscreenPosition(
      declared, aFullscreen, !(aPos->flags & SWP_NOMOVE), !(aPos->flags & SWP_NOSIZE),
      &x, &y, &cx, &cy);
  aPos->x = x;
  aPos->y = y;
  aPos->cx = cx;
  aPos->cy = cy;
}

void StealthDeclaredFullscreenTrackSize(nsIWidget& aWidget, bool aFullscreen,
                                        MINMAXINFO* aInfo) {
  Maybe<LayoutDeviceIntRect> declared;
  if (aFullscreen) {
    declared = StealthDeclaredFullscreenDeviceRect(aWidget);
  }
  int32_t w = aInfo->ptMaxTrackSize.x, h = aInfo->ptMaxTrackSize.y;
  StealthFullscreenMaxTrackSize(declared, aFullscreen, &w, &h);
  aInfo->ptMaxTrackSize.x = w;
  aInfo->ptMaxTrackSize.y = h;
}

}  // namespace mozilla::widget
