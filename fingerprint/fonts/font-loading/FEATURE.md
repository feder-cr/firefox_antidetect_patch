# fingerprint/fonts/font-loading

Category: other
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `layout/style/FontFace.cpp` (FontFace::Status; FontFace::Load, movable): Inert in this build: StealthFontAllowed (FontFaceImpl.cpp) always returns Nothing, so these branches never run. When it returns a value, Status() reports Loaded or Error, and Load() resolves or rejects without loading. The hunk also adds a GetFamily call per Status() call. Movable: the two branches can collapse into one call in FontFaceImpl. Verifier: StealthFontAllowed has no other definition or 
- `layout/style/FontFaceImpl.cpp` (StealthFontAllowed (new, in mozilla::dom); FontFaceImpl::SetStatus; FontFaceImpl::DoLoad (context only), movable): StealthFontAllowed always returns Nothing (inert), so the SetStatus override block never runs. If it ever returns a value, SetStatus forces the status and notifies the font-face sets. The comment still describes a per-profile fontlist that the code no longer reads. The file adds includes for StaticPrefs_zoom.h and nsUnicharUtils.h; no pref is read in the added code. Movable: the SetStatus block ca
- `layout/style/FontFaceImpl.h` (namespace mozilla::dom: StealthFontAllowed declaration (before class FontFaceImpl), already-hook): Declaration of the inert hook defined in FontFaceImpl.cpp. Adds mozilla/Maybe.h include. The header comment describes a per-profile fontlist gate that is not implemented. Verifier: StealthFontAllowed has no other definition or caller (only FontFaceImpl.h:36, FontFaceImpl.cpp:357,377, FontFace.cpp:247,265), so it is always Nothing and this code changes nothing a page can observe; the category is "o
