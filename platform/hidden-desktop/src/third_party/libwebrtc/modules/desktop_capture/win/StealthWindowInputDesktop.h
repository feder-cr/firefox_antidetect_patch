/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MODULES_DESKTOP_CAPTURE_WIN_STEALTH_WINDOW_INPUT_DESKTOP_H_
#define MODULES_DESKTOP_CAPTURE_WIN_STEALTH_WINDOW_INPUT_DESKTOP_H_

#include <windows.h>

#include <cwchar>
#include <memory>
#include <string>

#include "modules/desktop_capture/win/desktop.h"

namespace webrtc {

// STEALTHFOX_HIDDEN_DESKTOP: whether the window lives on the desktop the
// screen shows (the input desktop). This is the body of
// WindowCaptureHelperWin::IsWindowOnInputDesktop (window_capture_utils.h). A
// window of this process created on another Win32 desktop object passes every
// visibility check of WindowCaptureHelperWin (IsWindowVisibleOnCurrentDesktop
// and the others) and is still not on the screen, so nothing cropped from the
// screen can contain it. The desktop of the window's own thread is compared
// by name with the input desktop; the comparison is case-insensitive
// (_wcsicmp), unlike Desktop::IsSame, which compares the names with
// std::wstring == and so is case-sensitive. Only a thread of this process can
// be asked which desktop it was created on: a window of another process, a
// window without a thread, a thread whose desktop cannot be read and an input
// desktop that cannot be opened or named are all reported as on the input
// desktop. Windows of other processes cannot be asked, and are reported as on
// the input desktop, which is where a picker finds them. The handle
// GetThreadDesktop returns is not ours to close.
inline bool StealthIsWindowOnInputDesktop(HWND hwnd) {
  DWORD process_id = 0;
  const DWORD thread_id = ::GetWindowThreadProcessId(hwnd, &process_id);
  if (!thread_id || process_id != ::GetCurrentProcessId()) {
    return true;
  }
  HDESK window_desktop = ::GetThreadDesktop(thread_id);
  if (!window_desktop) {
    return true;
  }
  wchar_t window_desktop_name[256];
  DWORD length = 0;
  if (!::GetUserObjectInformationW(window_desktop, UOI_NAME,
                                   window_desktop_name,
                                   sizeof(window_desktop_name), &length)) {
    return true;
  }
  std::unique_ptr<Desktop> input_desktop(Desktop::GetInputDesktop());
  std::wstring input_desktop_name;
  if (!input_desktop || !input_desktop->GetName(&input_desktop_name)) {
    return true;
  }
  return _wcsicmp(window_desktop_name, input_desktop_name.c_str()) == 0;
}

}  // namespace webrtc

#endif  // MODULES_DESKTOP_CAPTURE_WIN_STEALTH_WINDOW_INPUT_DESKTOP_H_
