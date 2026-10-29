# fingerprint/screen-window/focus

Category: spoof
Prefs: zoom.stealth.focus.ignore_os_deactivation
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `xpfe/appshell/AppWindow.cpp` (AppWindow::WindowActivated (hunk 2, line 2746); AppWindow::WindowDeactivated (hunk 3, line 2757); hunk 1 is an include, already-hook): The window keeps focus when the OS takes it away, so document.hasFocus() stays true (page-observable, hence spoof). WindowDeactivated now returns early under the pref, so Mozilla's deactivation body is skipped. The lowering block in WindowActivated (4 logic lines) could move to our own file. Pref accessor needs the pref declared in StaticPrefList (not in this list). Verified: zoom.stealth.focus.ig

## xpfe/appshell/AppWindow.cpp: AppWindow::WindowActivated

With zoom.stealth.focus.ignore_os_deactivation the window this one
replaces was not lowered when the system took its focus (see
WindowDeactivated), so it is lowered here, first and with its own
action id: the order and the ids a page's process sees are those
of an ordinary switch. Left to WindowRaised, the lowering travels
with the raise's id, the page's process has already given up its
active window when the deactivation arrives, and
document.hasFocus() turns false with no blur event (measured, 4 in
4, when the session opened another page).

## xpfe/appshell/AppWindow.cpp: AppWindow::WindowDeactivated

⛔ zoom.stealth.focus.ignore_os_deactivation (B149): the window keeps its
activation when the SYSTEM takes it away - another application, another
session's window on the same display - and loses it only to this
session's own windows, whose activation arrives as WindowActivated and
lowers the window it replaces there, with the same blur a page sees in
any Firefox. Measured: a second visible
session on one display took the focus of the first 4 times in 4, and a
window opened on the host's desktop 6 in 6, so a page read
document.hasFocus() false while nothing in its own session had moved.
focusmanager.testmode stops that too but also emulates every raise, and
a page can tell: hasFocus() turns false with no blur event (3 in 3).
