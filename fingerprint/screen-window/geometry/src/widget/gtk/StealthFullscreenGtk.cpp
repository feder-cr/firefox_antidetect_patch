/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsWindow.h"

#include "GtkCompositorWidget.h"
#include "StealthDeclarationGate.h"
#include "StealthFullscreen.h"
#include "nsIWidgetListener.h"

using namespace mozilla;

// Fullscreen on an X display with no window manager, the way the engine runs
// on Linux servers (Xvfb). GTK's fullscreen is a request to the window
// manager (_NET_WM_STATE_FULLSCREEN): with nobody to answer it,
// nsWindow::MakeFullScreen refuses, the DOM side completes anyway, and the
// widget stays in its old size mode. A page then read display-mode:
// fullscreen false, window.fullScreen false and an innerHeight that was the
// work area, where retail Windows (whose widget decides fullscreen itself)
// gives the screen. The decision is taken here, where it was refused, the
// way Windows and HeadlessWidget take it: the real X window is resized to the
// declared screen, the widget knows its new size at once (so AppWindow's
// fullscreen change, which compares the window with the screen, finishes
// without waiting), then the size mode changes and the listener hears it.
// Leaving restores the rectangle and the mode the window had. With the
// engine off, or a kiosk monitor, it answers false and Mozilla's code runs.
bool nsWindow::StealthMakeFullScreenWithoutWM(bool aFullScreen) {
  if (!gfx::StealthEngineActive() || !mShell || mKioskMonitor.isSome()) {
    return false;
  }

  DesktopRect target;
  if (aFullScreen) {
    if (mSizeMode == nsSizeMode_Fullscreen) {
      return true;
    }
    const Maybe<DesktopRect> declared =
        widget::StealthDeclaredFullscreenRect(*this);
    if (!declared) {
      return false;
    }
    if (mSizeMode != nsSizeMode_Minimized) {
      mLastSizeModeBeforeFullscreen = mSizeMode;
    }
    if (!mSavedBounds) {
      mSavedBounds = Some(FullscreenSavedState());
    }
    mSavedBounds->windowRect = GetScreenBounds() / GetDesktopToDeviceScale();
    target = *declared;
  } else {
    if (mSizeMode != nsSizeMode_Fullscreen || !mSavedBounds) {
      return true;
    }
    target = mSavedBounds->windowRect;
  }

  // No window manager stands between the request and the X server, so the
  // window is what was asked for: the widget's own rectangle follows now, as
  // NativeMoveResize does for the windows only we place (popups), and the
  // ConfigureNotify that arrives later finds nothing to change.
  Resize(target, true);
  mClientArea.SizeTo(mLastSizeRequest);
  mClientArea.MoveTo(mLastMoveRequest);
  NotifyWindowMoved(mClientArea.TopLeft());
  DispatchResized();

  mSizeMode = aFullScreen ? nsSizeMode_Fullscreen
                          : mLastSizeModeBeforeFullscreen;
  if (aFullScreen) {
    mSavedBounds->screenRect = target;
  }
  if (mWidgetListener) {
    mWidgetListener->SizeModeChanged(mSizeMode);
  }
  if (mCompositorWidgetDelegate) {
    mCompositorWidgetDelegate->NotifyFullscreenChanged(aFullScreen);
  }
  return true;
}
