# automation/pipe

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `browser/app/winlauncher/LauncherProcessWin.cpp` (LauncherMain (after attrs.AddInheritableHandles(stdHandles)), movable): When --juggler-pipe is on the command line, reads PW_PIPE_READ and PW_PIPE_WRITE from the environment, parses them with atoi, and adds them to the child's inheritable handles. Windows only. Movable: the ~14-line block can move into an own helper with a one-line call here (plus the two added includes <io.h> and <stdlib.h>). The comment says the handles are resolved earlier in wmain (nsWindowsWMain.
- `toolkit/xre/nsWindowsWMain.cpp` (int wmain(int argc, WCHAR** argv) (line 114); includes at line 14, movable): Copies inherited stdio handles 3 and 4 into the PW_PIPE_READ and PW_PIPE_WRITE env variables, so the juggler pipe code (nsRemoteDebuggingPipe.cpp, not in this list) can read them after the launcher handoff. The comment says this is an FF146 patch ported to FF150, but the feature is juggler's. Move the block into a function in our own file and keep one call here. The comment names a product (Playwr
