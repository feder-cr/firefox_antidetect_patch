# fingerprint/screen-window/screen

Category: spoof
Prefs: zoom.stealth.screen.color_depth, zoom.stealth.screen.height, zoom.stealth.screen.taskbar_px, zoom.stealth.screen.width
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/nsScreen.cpp` (nsScreen::PixelDepth; nsScreen::GetRect; nsScreen::GetAvailRect, movable): Declared color depth, screen rect and avail rect from prefs; avail height is reduced by zoom.stealth.screen_taskbar_px. GetAvailRect has about 6 logic lines and reads StaticPrefs directly, so it is movable behind one call. The file adds #include mozilla/StaticPrefs_zoom.h twice (hunk 1 and again at the end of the include block); remove one. Comments mention invisible_core and a document outside th
- `layout/base/nsPresContext.cpp` (nsPresContext::UpdateInnerSizeSpoofedForRFP, already-hook): The synced top-level inner size is now also filled when the stealth engine is active, not only under RFP. The comment's claim that every other reader of the field gates on RFP is MOSTLY CONFIRMED: nsScreen.cpp:100 and :161 (via GetTopWindowInnerRectForRFP, nsScreen.cpp:271), nsGlobalWindowInner.cpp:7584-7586, ScreenOrientation.cpp:706-771 and nsGlobalWindowOuter.cpp:3531-3535 all sit behind Should

## layout/base/nsPresContext.cpp: nsPresContext::UpdateInnerSizeSpoofedForRFP (condition of the early return)

Stealth: the same synced field (BrowsingContext::SetInnerSizeSpoofedForRFP, filled by nsPresContext::UpdateInnerSizeSpoofedForRFP) carries the top-level inner size when the
stealth engine is on, because a popup's declared outerWidth is that size
plus its frame, and an out-of-process iframe inside the popup has no other
way to know it (its BrowserChild knows the top's OUTER rect and its own
inner size, not the top's inner size). Every other reader of the field -
ScreenOrientation, window.orientation, nsScreen, the RFP branch of
GetOuterSize - reads it only under ShouldResistFingerprinting, so filling
it in UpdateInnerSizeSpoofedForRFP changes nothing for them. One synced copy of one fact, instead of
a second field for the same number. 70-known-bugs.md B258.
