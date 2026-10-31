/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The minimal engine (--enable-stealth-minimal) keeps no actor of what it leaves out of the
// build (mozbuild/frontend/stealth_minimal.py), nor of browser chrome it never shows: none
// of them changes what a page or the site sees (study of 2026-10-31). A startup component
// of the minimal engine only (components.conf): at profile-after-change it loads toolkit's
// actor tables, as the browser does at startup, and unregisters these before any content
// process exists.

const WINDOW_ACTORS = [
  // the components the minimal engine drops
  "CaptchaDetection",
  "CaptchaDetectionCommunication",
  "PageExtractor",
  "ReportBrokenSite",
  "AboutTranslations",
  "Translations",
  // browser chrome the minimal application has none of
  "AboutCertViewer",
  "AboutHttpsOnlyError",
  "AboutRestricted",
  "AudioPlayback",
  "Autoplay",
  "AutoScroll",
  "BackgroundThumbnails",
  "ContentMeta",
  "Controllers",
  "ExtFind",
  "FindBar",
  "Finder",
  "PopupAndRedirectBlocking",
  "Thumbnails",
  "UnselectedTabHover",
  "ViewSource",
  "ViewSourcePage",
];

const PROCESS_ACTORS = ["AsyncPrefs", "ContentPrefs", "TranslationsEngine"];

// Names that were not registered: a port that renames an actor shows up here (and in the
// minimal engine's tests) instead of silently keeping it.
export const missingActors = [];

export function unregisterActors() {
  ChromeUtils.importESModule("resource://gre/modules/ActorManagerParent.sys.mjs");
  for (const [names, unregister] of [
    [WINDOW_ACTORS, name => ChromeUtils.unregisterWindowActor(name)],
    [PROCESS_ACTORS, name => ChromeUtils.unregisterProcessActor(name)],
  ]) {
    for (const name of names) {
      try {
        unregister(name);
      } catch (e) {
        missingActors.push(name);
      }
    }
  }
}

export class StealthMinimalStartup {
  QueryInterface = ChromeUtils.generateQI(["nsIObserver"]);

  observe(subject, topic) {
    if (topic === "profile-after-change") {
      unregisterActors();
    }
  }
}
