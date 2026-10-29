/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StealthJugglerPipe_h
#define mozilla_StealthJugglerPipe_h

#include <windows.h>
#include <stdint.h>
#include <stdlib.h>

#include "mozilla/CmdLineAndEnvUtils.h"
#include "mozilla/WinHeaderOnlyUtils.h"

#include "ProcThreadAttributes.h"

namespace mozilla {

// Juggler pipe: forward the two pipe handles to the browser child, so the
// pipe code in xul.dll can read and write them.
//
// The handles come from PW_PIPE_READ / PW_PIPE_WRITE and from nowhere
// else. `wmain` (nsWindowsWMain.cpp) has already resolved them into those
// variables before this runs: from CRT descriptors 3 and 4 when a Node.js
// parent passes the pipe through its stdio array, or left exactly as the
// parent set them when it names the handles itself. Reading the
// descriptors here again was a second resolution of the same fact, and it
// disagreed with the first. A parent that names the handles opens no CRT
// descriptor 3, `_get_osfhandle(3)` on a closed descriptor trips the CRT's
// invalid-parameter handler, and this process died with 0xC0000409 before
// creating any browser. It happened on the FIRST launch from every new
// install path and never again: the second launch found a launcher
// timestamp with no browser timestamp and disabled the launcher process.
//
// Only with --juggler-pipe on the command line (argv is not modified), and
// only when both variables exist. The values go through atoi without a check:
// text that is not a number becomes handle 0, which ProcThreadAttributes then
// refuses (not a disk or pipe handle). The return value of
// AddInheritableHandles is ignored, as it is for the standard handles.
inline void StealthAddJugglerPipeHandles(int& aArgc, wchar_t** aArgv,
                                         ProcThreadAttributes& aAttrs) {
  bool hasJugglerPipe =
      mozilla::CheckArg(aArgc, aArgv, "juggler-pipe", nullptr,
                        mozilla::CheckArgFlag::None) == mozilla::ARG_FOUND;
  if (hasJugglerPipe) {
    char pipeRead[20] = {0};
    char pipeWrite[20] = {0};
    if (::GetEnvironmentVariableA("PW_PIPE_READ", pipeRead,
                                  sizeof(pipeRead)) &&
        ::GetEnvironmentVariableA("PW_PIPE_WRITE", pipeWrite,
                                  sizeof(pipeWrite))) {
      HANDLE pipeHandles[] = {
          reinterpret_cast<HANDLE>(static_cast<intptr_t>(atoi(pipeRead))),
          reinterpret_cast<HANDLE>(static_cast<intptr_t>(atoi(pipeWrite)))};
      aAttrs.AddInheritableHandles(pipeHandles);
    }
  }
}

}  // namespace mozilla

#endif  // mozilla_StealthJugglerPipe_h
