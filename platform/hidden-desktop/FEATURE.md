# platform/hidden-desktop

Category: other
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `third_party/libwebrtc/modules/desktop_capture/cropping_window_capturer_win.cc` (CroppingWindowCapturerWin::ShouldUseScreenCapturer, already-hook): Vendored libwebrtc file. The hook is 2 logic lines; most of the 13 added lines are a comment block that can be shortened or moved to the commit message. Depends on IsWindowOnInputDesktop (window_capture_utils.h in this group). The comment carries a measurement date; keep that in the commit, not the code.
- `third_party/libwebrtc/modules/desktop_capture/win/window_capture_utils.cc` (WindowCaptureHelperWin::IsWindowOnInputDesktop (new), movable): The whole new function (18 logic lines) sits in a vendored libwebrtc file. Move it to our own .cc (registered in the libwebrtc build) and keep only the member declaration in the header. Behaviour: returns true (treated as on the input desktop) for windows of other processes, for a null thread id and on any API failure, so only this process's windows on another desktop are excluded. Names are compa
- `third_party/libwebrtc/modules/desktop_capture/win/window_capture_utils.h` (WindowCaptureHelperWin (class declaration), already-hook): Declaration and comment only (0 logic lines). The only Mozilla-side piece of the hidden-desktop feature; the definition is in window_capture_utils.cc.
