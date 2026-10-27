# fingerprint/screen-window/geometry

Category: spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/nsGlobalWindowOuter.cpp` (CreateNativeGlobalForInner; nsGlobalWindowOuter::GetOuterSize; nsGlobalWindowOuter::GetScreenXY; nsGlobalWindowOuter::StealthContentOriginShift (new); nsGlobalWindowOuter::IsInStealthPopup (new); nsGlobalWindowOuter::GetMozInnerScreenXOuter; nsGlobalWindowOuter::GetMozInnerScreenYOuter, movable): Mixed file. (1) language-declared and timezone-declared: CreateNativeGlobalForInner takes language and zone from the declared session (GetDeclaredLanguage/GetDeclaredTimezone in StealthSessionDeclaration) instead of Top()->GetLanguageOverride()/GetTimezoneOverride(). (2) window-geometry: GetOuterSize and GetScreenXY answer declared maximized-window or popup geometry. (3) window-geometry: StealthCo
- `dom/base/nsGlobalWindowOuter.h` (nsGlobalWindowOuter (declarations next to GetInnerScreenRect), already-hook): Two declarations with Italian comments. Event.cpp includes this header only to call StealthContentOriginShift; if the function moves to our own file, this header change and that include can go.
- `dom/events/Event.cpp` (Event::GetScreenCoords, movable): Adds the declared content origin to the screen coordinates computed in Event::GetScreenCoords, so event.screenX - event.clientX equals mozInnerScreenX. Hot path: the comment records that an earlier version with an extra layout query per event made the browser too slow, so keep it arithmetic-only. Includes nsGlobalWindowOuter.h only for one static call, which pulls a large header into Event.cpp. Ab
