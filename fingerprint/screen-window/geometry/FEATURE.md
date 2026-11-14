# fingerprint/screen-window/geometry

Category: spoof
Prefs: zoom.stealth.screen.width, zoom.stealth.screen.height (the fullscreen rectangle, through gfx::StealthDeclaredScreenRect); the window frame through StealthDeclarationGate
Test: dom/events/test/gtest/TestStealthWindowGeometry.cpp (StealthWindowGeometry.FullscreenContentStartsAtTheScreenOrigin, StealthWindowGeometry.OutsideFullscreenNothingChanges, StealthWindowGeometry.TheMaximizedContentAreaIsTheMaximizedWindow, StealthWindowGeometry.AWindowWithNoContentSizeYetIsTheMaximizedOne, StealthWindowGeometry.ASmallerContentAreaKeepsTheMaximizedFrame); widget/tests/gtest/TestStealthFullscreen.cpp (StealthFullscreen.TheDeclaredScreenAtTheDeclaredScale, StealthFullscreen.PlacedOnTheScreenTheWindowIsOn, StealthFullscreen.NothingWithoutADeclaredScreenOrAScale); juggler/tests/fullscreen_releases_the_viewport.test.mjs; the fullscreen state of the host gate in the wrapper (scripts/ci_host_gate.py: geometry after a real click on requestFullscreen() and after exitFullscreen(), with its invariants) on every CI host, Linux under Xvfb without a window manager included; the g.fullscreen key of headless_probe.py. widget/tests/gtest/TestStealthFullscreen.cpp also holds StealthFullscreen.AFullscreenMoveAndResizeLandsOnTheDeclaredRect and StealthFullscreen.TheLargestSizeGrowsToTheDeclaredScreenInFullscreen (the Windows rules); the host gate invariant fullscreen_is_the_screen on win-x86_64 is their end-to-end proof.

## What it does

The window geometry a page reads comes from the declaration, in every state
the page can drive. Normal: the main window is the declared maximized window
(outer size, screenX/Y, the content origin under mozInnerScreenX/Y and every
mouse event), a popup its real inner size plus the declared frame. Fullscreen
(requestFullscreen, F11, a popup too): the window is the declared screen, as
on retail (outer == inner == screen, position and content origin 0). The
fullscreen state itself is decided on every host, not by the host's window
manager or monitor: nsIWidget::InfallibleMakeFullScreen fills the declared
screen instead of the host's monitor (Windows, headless), and on X with no
window manager, where GTK refused fullscreen and the widget never left its
size mode (display-mode false, innerHeight the work area), the GTK widget
decides it as Windows and HeadlessWidget do (nsWindow::StealthMakeFullScreenWithoutWM:
the real X window resized to the declared screen, the widget rectangle updated
at once so AppWindow's fullscreen change finishes, then the size mode and the
listener; leaving restores both). It is a member of nsWindow, defined in our
file, because the size mode and the client area are private state that only
OnWindowStateEvent sets; the hook is one call at the head of MakeFullScreen.
Juggler's viewport pin lets go in fullscreen and comes back on leaving.

## A resized content area keeps the frame (2026-11-14)

Page.setViewportSize makes the main window's content area smaller than the
maximized window's (a responsive check at 390x844). The outer size used to stay
the declared maximized window, so at 100% a page read outerWidth 1936 around
innerWidth 390: outerWidth - innerWidth 1546 where every Windows Firefox
answers 16, and outerHeight - innerHeight 204 where it answers 101.
gfx::StealthMainWindowOuterSize (platform/declaration-gate) now answers the
maximized window only for the maximized content area (or a content area with
no size yet), and otherwise a restored window around the content with the
maximized window's own outer - inner, taken in CSS pixels from the two CSS
sizes the page reads unresized so it holds at every scale (16x101 at 100%,
14x100 at 125%, 15x100 at 150%, 13x97 at 200%). Position and content origin are
unchanged (screenX/Y, mozInnerScreenX/Y, event screenX - clientX): the window is
anchored at the top left of the work area. Popups and fullscreen are unchanged.
The inner size is the synced top-level one (TopInnerSizeSpoofedForRFP, the same
the popup frame reads), so an out-of-process iframe answers the same outer size.

Not measured: retail's restored-window UI height. The vertical difference here
is the maximized window's; if a restored retail window's tab strip sits lower,
that is a measured `restored_ui` declaration to add, like maximized_ui.

## Windows fullscreen (2026-11-02)

On Windows a fullscreen window was put back on the HOST's monitor after
InfallibleMakeFullScreen had asked for the declared rectangle: run 41325657085
read innerWidth x innerHeight 1024x768 (the GPU-less CI desktop) under a declared
1280x720 or 1920x1080 screen. Two Mozilla rules decide from the host there:
nsWindow::OnWindowPosChanging forces every move-and-resize of a fullscreen window
onto the monitor's rect (bug 1482920), and DefWindowProc clamps the size to
MINMAXINFO.ptMaxTrackSize, about the monitor's size. Both are answered from the
declaration in fullscreen (widget/windows/StealthFullscreenWin.cpp, thin
adapters over the pure rules StealthHeldFullscreenPosition and
StealthFullscreenMaxTrackSize in widget/StealthFullscreen.cpp).

## Hooks in Mozilla files

- `widget/nsIWidget.cpp` (include; nsIWidget::InfallibleMakeFullScreen before "Move to fill the screen", already-hook): the fullscreen rectangle is the declared screen (StealthDeclaredFullscreenRect); Mozilla's path with the engine off.
- `widget/gtk/nsWindow.h` (after MakeFullScreen, already-hook): declares nsWindow::StealthMakeFullScreenWithoutWM.
- `widget/gtk/nsWindow.cpp` (one line at the head of nsWindow::MakeFullScreen; the file belongs to fingerprint/screen-window/occlusion, whose hooks.patch carries this line): on X with no window manager the widget decides fullscreen itself.
- `widget/moz.build`, `widget/gtk/moz.build` (SOURCES, X11 headers), `widget/tests/gtest/moz.build`, `dom/events/test/gtest/moz.build`: build our files and tests.
- `dom/base/nsGlobalWindowOuter.cpp` (CreateNativeGlobalForInner; nsGlobalWindowOuter::GetOuterSize; nsGlobalWindowOuter::GetScreenXY; nsGlobalWindowOuter::StealthContentOriginShift (new); nsGlobalWindowOuter::IsInStealthPopup (new); nsGlobalWindowOuter::GetMozInnerScreenXOuter; nsGlobalWindowOuter::GetMozInnerScreenYOuter, movable): Mixed file. (1) language-declared and timezone-declared: CreateNativeGlobalForInner takes language and zone from the declared session (GetDeclaredLanguage/GetDeclaredTimezone in StealthSessionDeclaration) instead of Top()->GetLanguageOverride()/GetTimezoneOverride(). (2) window-geometry: GetOuterSize and GetScreenXY answer declared maximized-window or popup geometry. (3) window-geometry: StealthCo
- `dom/base/nsGlobalWindowOuter.h` (nsGlobalWindowOuter (declarations next to GetInnerScreenRect), already-hook): Two declarations with Italian comments. Event.cpp includes this header only to call StealthContentOriginShift; if the function moves to our own file, this header change and that include can go.
- `dom/events/Event.cpp` (Event::GetScreenCoords, movable): Adds the declared content origin to the screen coordinates computed in Event::GetScreenCoords, so event.screenX - event.clientX equals mozInnerScreenX. Hot path: the comment records that an earlier version with an extra layout query per event made the browser too slow, so keep it arithmetic-only. Includes nsGlobalWindowOuter.h only for one static call, which pulls a large header into Event.cpp. Ab
