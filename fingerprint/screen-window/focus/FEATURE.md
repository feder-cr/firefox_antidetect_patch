# fingerprint/screen-window/focus

Category: spoof
Prefs: zoom.stealth.focus.ignore_os_deactivation
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `xpfe/appshell/AppWindow.cpp` (AppWindow::WindowActivated (hunk 2, line 2746); AppWindow::WindowDeactivated (hunk 3, line 2757); hunk 1 is an include, already-hook): The window keeps focus when the OS takes it away, so document.hasFocus() stays true (page-observable, hence spoof). WindowDeactivated now returns early under the pref, so Mozilla's deactivation body is skipped. The lowering block in WindowActivated (4 logic lines) could move to our own file. Pref accessor needs the pref declared in StaticPrefList (not in this list). Verified: zoom.stealth.focus.ig
