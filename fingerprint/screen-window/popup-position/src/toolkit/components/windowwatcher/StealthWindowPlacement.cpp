/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthWindowPlacement.h"

#include "StealthDeclarationGate.h"
#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"
#include "nsIBaseWindow.h"

namespace mozilla {

bool StealthPlacePopupAtDefault(bool aIsCallerChrome,
                                nsIBaseWindow* aTreeOwnerAsWin, bool aHasLeft,
                                bool aHasTop, DesktopIntCoord& aLeft,
                                DesktopIntCoord& aTop) {
  // Stealth: a window a PAGE opens without left/top is born at the declared
  // default position, not wherever the host puts it (70-known-bugs.md B258).
  // Retail Firefox 151 on Windows 11 opens it at 4,4 device pixels (3,3 CSS at
  // 125%, 2,2 at 200%); the host would put it anywhere - Windows staggers from
  // the opener, a bare X server has no window manager at all - and a popup's
  // screenX is its REAL position (nsGlobalWindowOuter::GetScreenXY). Blocked
  // here, at birth, so both builds have the same real position and nothing
  // corrects it afterwards (engine rule 3). Chrome callers keep the stock
  // behaviour.
  //
  // A position the page DOES give needs nothing: CalcSizeSpec has already
  // turned its CSS pixels into desktop pixels with the opener's own
  // CSS-to-desktop scale, which carries the persona's scale and the host
  // display's together. Measured on the Linux build at 1.25, 1.5 and 2: a
  // second conversion here put left=100 at screenX 125, 150 and 200.
  if (aIsCallerChrome) {
    return false;
  }
  const Maybe<gfx::StealthWindowFrame> stealthFrame =
      gfx::StealthDeclaredWindowFrame();
  if (!stealthFrame) {
    return false;
  }
  MOZ_ASSERT(aTreeOwnerAsWin);
  const DesktopToLayoutDeviceScale devToDesktop =
      aTreeOwnerAsWin->DevicePixelsPerDesktopPixel();
  const DesktopIntCoord byDefault = DesktopIntCoord(
      (LayoutDeviceCoord(stealthFrame->mPopupPosition) / devToDesktop)
          .Rounded());
  if (!aHasLeft) {
    aLeft = byDefault;
  }
  if (!aHasTop) {
    aTop = byDefault;
  }
  return true;
}

}  // namespace mozilla
