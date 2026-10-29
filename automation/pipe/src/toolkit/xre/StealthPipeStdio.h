/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef toolkit_xre_StealthPipeStdio_h
#define toolkit_xre_StealthPipeStdio_h

// Windows only. Textually included into the wmain bootstrap code through
// nsWindowsWMain.cpp (browser/app/nsBrowserApp.cpp,
// ipc/app/MozillaRuntimeMain.cpp, js/xpconnect/shell/xpcshell.cpp), so
// everything here is inline.

#ifndef XP_WIN
#  error This file only makes sense on Windows.
#endif

#include "mozilla/CmdLineAndEnvUtils.h"  // CheckArg, CheckArgFlag, EnvHasValue

#include <io.h>       // _get_osfhandle
#include <stdint.h>   // intptr_t
#include <stdlib.h>   // _itoa
#include <windows.h>  // CHAR, SetEnvironmentVariableA

// When the command line has --juggler-pipe and PW_PIPE_READ is not set yet,
// publish the inherited stdio descriptors 3 and 4 (the pipe the automation
// client created for this process: the parent passes them through the
// Node.js stdio array) as the decimal OS handle values in the
// environment variables PW_PIPE_READ and PW_PIPE_WRITE. The launcher process
// then forwards those two handles to the browser child, and the pipe code in
// juggler/pipe/nsRemoteDebuggingPipe.cpp reads the variables there (see the
// launcher side in browser/app/winlauncher/LauncherProcessWin.cpp).
// Must run in wmain before the launcher handoff.
// History: first written for Firefox 146, ported to Firefox 150.
inline void StealthJugglerPipeStdio(int argc, wchar_t** argv) {
  bool hasJugglerPipe =
      mozilla::CheckArg(argc, argv, "juggler-pipe", nullptr,
                        mozilla::CheckArgFlag::None) == mozilla::ARG_FOUND;
  if (hasJugglerPipe && !mozilla::EnvHasValue("PW_PIPE_READ")) {
    intptr_t stdio3 = _get_osfhandle(3);
    intptr_t stdio4 = _get_osfhandle(4);
    CHAR stdio3str[20];
    CHAR stdio4str[20];
    _itoa(static_cast<int>(stdio3), stdio3str, 10);
    _itoa(static_cast<int>(stdio4), stdio4str, 10);
    SetEnvironmentVariableA("PW_PIPE_READ", stdio3str);
    SetEnvironmentVariableA("PW_PIPE_WRITE", stdio4str);
  }
}

#endif  // toolkit_xre_StealthPipeStdio_h
