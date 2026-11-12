# automation/pipe

Category: juggler
Prefs: none read directly
Test: TODO

## What it does

The browser is driven over a pipe: the client starts it with `--juggler-pipe` and the two ends of the pipe inherited (on Windows the launcher hands them to the browser in `PW_PIPE_READ` and `PW_PIPE_WRITE`), and speaks Juggler on it.

The first command must be `Browser.setBrowserProxy` or `Browser.setNoBrowserProxy`. Until one of them arrives the road of the session is undecided and no request leaves the browser: every channel that asks for a proxy is held, and none is ever released on a timer.

A client that sends neither (a stock automation client started on this binary) therefore seems to hang. When a request has waited more than 5 s the browser writes one line on stderr, once: `juggler: a request has been held for more than 5 s: no request leaves until Browser.setBrowserProxy or Browser.setNoBrowserProxy is received`. A client that decides in time writes nothing.

## Hooks in Mozilla files

- `browser/app/winlauncher/LauncherProcessWin.cpp` (LauncherMain (after attrs.AddInheritableHandles(stdHandles)), movable): When --juggler-pipe is on the command line, reads PW_PIPE_READ and PW_PIPE_WRITE from the environment, parses them with atoi, and adds them to the child's inheritable handles. Windows only. Movable: the ~14-line block can move into an own helper with a one-line call here (plus the two added includes <io.h> and <stdlib.h>). The comment says the handles are resolved earlier in wmain (nsWindowsWMain.
- `toolkit/xre/nsWindowsWMain.cpp` (int wmain(int argc, WCHAR** argv) (line 114); includes at line 14, movable): Copies inherited stdio handles 3 and 4 into the PW_PIPE_READ and PW_PIPE_WRITE env variables, so the juggler pipe code (nsRemoteDebuggingPipe.cpp, not in this list) can read them after the launcher handoff. The comment says this is an FF146 patch ported to FF150, but the feature is juggler's. Move the block into a function in our own file and keep one call here. The comment names a product (Playwr
