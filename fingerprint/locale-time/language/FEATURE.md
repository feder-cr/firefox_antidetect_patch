# fingerprint/locale-time/language

Category: build, other, spoof
Prefs: none read directly
Test: TODO

## What it does

TODO: one paragraph, from the notes below.

## Hooks in Mozilla files

- `dom/base/test/browser_language_override.js` (add_task test_language_override_reaches_workers (new); getWorkerLanguages (new helper), movable): New browser test that the language override reaches dedicated, nested and shared workers (Intl locale, navigator.language, navigator.languages). It is test code inside a Mozilla test file. It could live in a test file of ours registered in the test manifest, which would need a manifest entry. Not run here. Category is other because it is a test, not a fingerprint change; it exercises feature langu
- `intl/locale/LocaleService.cpp` (LocaleService::GetRegionalPrefsLocales; new static helper MachineRegionalPrefsLocales, movable): Regional locales (which feed web-exposed locales and date/time formatting) now take the declared session language first, then the OS. The ~10-line helper body (new static MachineRegionalPrefsLocales plus the two call-site edits) can move into StealthSessionPrefs.cpp, leaving two call sites here. Input-only change per the comment; upstream's en-US rule is kept. The language comes from intl::Session
- `intl/locale/moz.build` ((moz.build EXPORTS and UNIFIED_SOURCES lists), already-hook): Registers our own StealthSessionPrefs files; the .cpp is added to UNIFIED_SOURCES.
- `intl/locale/nsLanguageAtomService.cpp` (nsLanguageAtomService::GetLocaleLanguage (inside the write lock, before the regional-prefs lookup), movable): The fallback language atom (used for text without lang, hence font language group) now comes from the declared session language before the OS lookup. The ~6-line early-return branch can become one call into StealthSessionPrefs. Comment says per process. Reached from workers too (OffscreenCanvas per the StaticPrefList comment); thread-safety concern REFUTED: the pref is read through intl/locale/Ste
