# minimal/remove

Category: build
Prefs: none read directly
Test: scripts/ci/test_ci.py runs toolkit/stealth-minimal/keep_jar.py --check (every dropped name is listed by its parent, the kept jar.mn is up to date); on the package, the page-surface judgement against retail (validate_release.py, minimal flavor) and the minimal app's behaviour tests

## What it does

The minimal engine leaves out the code of what opening a page, reading it and closing it does not need: 55 directories (toolkit components such as contentprefs, pageextractor, crash monitor; the browser front-end such as urlbar, asrouter, newtab, sidebar, preferences), dropped from their parents' DIRS by one hook in the moz.build reader (python/mozbuild/mozbuild/frontend/stealth_minimal.py), so no moz.build of Mozilla's changes; and the actors of that code and of browser chrome it never shows, unregistered at profile-after-change by a startup component of the minimal engine only. What those directories install into chrome packages stays, the same bytes: chrome://browser/content/ and chrome://global/content/ are readable by any page and package names can be probed, so toolkit/stealth-minimal/keep_jar.py copies their jar.mn files into browser/stealth-minimal and fails on any form it does not know. Kept on purpose: the about: pages of toolkit (only page-readable files), reader and narrate, search and syncedtabs (imported by kept code), the built-in add-ons, and everything a page observes (study of 2026-10-31, scratchpad inventory).

## Hooks in Mozilla files

- `python/mozbuild/mozbuild/frontend/reader.py` (_read_mozbuild, before the main context is yielded, already-hook): With MOZ_STEALTH_MINIMAL, prunes each moz.build's DIRS (mozbuild/frontend/stealth_minimal.py) before the backend reads them: no moz.build of Mozilla's is touched.

## Our own files

- `browser/stealth-minimal/jar.mn`
- `browser/stealth-minimal/moz.build`
- `python/mozbuild/mozbuild/frontend/stealth_minimal.py`
- `toolkit/stealth-minimal/StealthMinimal.sys.mjs`
- `toolkit/stealth-minimal/components.conf`
- `toolkit/stealth-minimal/keep_jar.py`
