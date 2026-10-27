# fingerprint/screen-window/occlusion

Category: spoof
Prefs: zoom.stealth.widget.occlusion_tracking.enabled
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `widget/gtk/nsWindow.cpp` (nsWindow::OnVisibilityNotifyEvent (hunk 2; hunk 1 adds the StaticPrefs include), already-hook): GTK/Linux only. A fully covered window stays visible, so the page does not see hidden. Verified: pref declared at modules/libpref/init/StaticPrefList.yaml:21556. The comment names a bug id (B149) and a measured Xvfb result.
