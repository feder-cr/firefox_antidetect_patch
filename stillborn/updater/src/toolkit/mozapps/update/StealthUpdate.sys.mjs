/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The application update system as official Firefox builds it, minus every
 * step that would change the installed browser or the host.
 *
 * The updater is compiled and packaged exactly as in a release
 * (build/moz.configure/update-programs.configure is Mozilla's), so the update
 * CHECK goes out as official Firefox sends it: same timer, same URL, same
 * headers. What we refuse is what follows an answer: downloading, staging and
 * applying an update would replace the engine under the seal that pins it, and
 * the Windows background update task would leave a scheduled task on the
 * machine. UpdateService.sys.mjs and BackgroundUpdate.sys.mjs only call the
 * functions below from marked hooks.
 */

/**
 * Called in CheckerService's update check right after Mozilla parsed the
 * server's answer. Whatever the answer offered, the check reports none: every
 * consumer (the background timer, the about dialog, the settings card, the
 * background task) then takes Mozilla's own "no update" path, with no download,
 * no prompt and no badge. The request itself has already gone out unchanged.
 *
 * @param {nsIUpdate[]} aUpdates the updates Mozilla parsed from the answer
 * @returns {nsIUpdate[]} always a new empty array
 */
export function noneOffered(aUpdates) {
  if (aUpdates.length) {
    console.info(
      `StealthUpdate: the update check offered ${aUpdates.length} update(s); none is taken`
    );
  }
  return [];
}

/**
 * Called at the start of UpdateService's #downloadUpdate, the one door every
 * download goes through (a fresh offer, a download resumed from an earlier
 * session, the fallback to a complete update). After noneOffered nothing should
 * reach it; this is the backstop for update state left in the update directory
 * by another build installed at the same path.
 *
 * @returns {boolean} true: the download is refused
 */
export function refusesDownload() {
  return true;
}

/**
 * Called at the start of BackgroundUpdate._registerBackgroundUpdateTask, the
 * only place a browser run registers the Windows "Background Update" scheduled
 * task. Everything before it runs as Mozilla wrote it (its checks, the prefs
 * it records); only the task, which would outlive the session on the host, is
 * not created.
 *
 * @returns {boolean} true: no task is registered
 */
export function refusesHostTask() {
  return true;
}
