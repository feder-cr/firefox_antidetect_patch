# stillborn/updater

Category: stillborn
Prefs: app.update.interval, app.update.promptWaitTime, app.update.url.manual, app.update.url.details, app.update.checkInstallTime.days, app.update.badgeWaitTime (values only)
Test: juggler/tests/updater_refuses_what_follows_the_check.test.mjs; scripts/validate_release.py --selftest (assert_updater_is_retails)

## What it does

The application update system is the retail's, minus what would change the
installed browser or the host (owner's decision, 2026-10-30: "come ufficiale,
senza installare").

- `build/moz.configure/update-programs.configure` is Mozilla's file: the
  updater, the maintenance service and the update agent are compiled and
  packaged as in a release. `.mozconfig` passes the retail's MAR channel
  (`firefox-mozilla-release`), which Mozilla passes from its CI environment.
- The update check to aus5.mozilla.org goes out as official Firefox sends it:
  same timer, same URL, same headers.
- `toolkit/mozapps/update/StealthUpdate.sys.mjs` refuses what follows the
  answer: `noneOffered()` after Mozilla parses it (every consumer then takes
  Mozilla's own no-update path: no prompt, no badge, no download),
  `refusesDownload()` at the one door of every download (backstop for update
  a download another build left half done at the same install path; an update
  it already staged is applied by the C++ driver at startup, not covered),
  `refusesHostTask()` at
  the only place a browser run registers the Windows "Background Update"
  scheduled task.
- We build with the unofficial branding, whose update timings are Nightly-like
  (a check every 24 h, and one at every start once the build is 2 days old).
  Its lines carry the official branding's release values instead.

From 2026-08-31 to 2026-10-30 the update options were deleted from the
configure file instead: no updater was built and the check official Firefox
makes at startup never went out.

Gate: `scripts/validate_release.py` `assert_updater_is_retails` refuses a
package without the updater binary, without the retail's MAR channel in
`update-settings.ini`, or whose `UpdateService.sys.mjs` lacks our refusals.

## Hooks in Mozilla files

- `toolkit/mozapps/update/UpdateService.sys.mjs`: the import; `noneOffered()`
  after `#parseUpdates` in `#updateCheck`; `refusesDownload()` at the head of
  `#downloadUpdate`.
- `toolkit/mozapps/update/BackgroundUpdate.sys.mjs`: the import;
  `refusesHostTask()` at the head of `_registerBackgroundUpdateTask`.
- `toolkit/mozapps/update/moz.build`: our module, after Mozilla's list.
- `browser/branding/unofficial/pref/firefox-branding.js`: six update prefs,
  each on Mozilla's line with the release value.
