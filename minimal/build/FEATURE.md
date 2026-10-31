# minimal/build

Category: build
Prefs: none read directly
Test: scripts/validate_release.py --selftest (the minimal cases), scripts/ci/test_ci.py (the minimal mozconfig and asset names), scripts/test_release_plan.py (a minimal candidate is cold, never a release)

## What it does

The minimal engine: the same series built with `--enable-stealth-minimal`, which leaves out of the build what opening a page, reading it and closing it does not need. Our configure file implies `--disable-webdriver` (the remote agent, Marionette and, through the juggler-build hook, Juggler), `--disable-crashreporter`, `--disable-backgroundtasks`, `--disable-parental-controls`, and on the targets where they exist `--disable-notification-server` and `--disable-necko-wifi`; it sets MOZ_STEALTH_MINIMAL, which minimal/glean reads. The normal engine never passes the option and builds exactly as before. Each removal was chosen because a page and the site it loads cannot tell (study of 2026-10-31): what a page can observe stays, the default-browser agent included (its image in chrome://global/content/ is readable by any page). The CI builds it as a flavor (release.yml input `flavor`), a candidate only and cold, and its package gates refuse a minimal engine that still carries Juggler, the remote agent or the crash reporter.

## Hooks in Mozilla files

- `moz.configure` (before include(include_project_configure), already-hook): Includes our stealth-minimal.configure before the project's configure, so its imply_option() calls precede the options they imply.

## Our own files

- `build/moz.configure/stealth-minimal.configure`
