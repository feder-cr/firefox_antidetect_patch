/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_intl_StealthSessionPrefs_h__
#define mozilla_intl_StealthSessionPrefs_h__

#include "mozilla/AlreadyAddRefed.h"
#include "nsError.h"
#include "nsStringFwd.h"
#include "nsTArrayForwardDeclare.h"

class nsAtom;

namespace mozilla::intl {

/**
 * STEALTHFOX (firefox-50): the time zone and the language the SESSION
 * declares, and the only code that reads them.
 *
 * invisible_core writes them as `juggler.timezone.override` (an IANA zone) and
 * `juggler.locale.override` (an Accept-Language list, "it-IT, it, en-US, en"),
 * static prefs that reach every process and any thread (StealthPrefList.yaml).
 *
 * A context may carry its own zone and language, in its top BrowsingContext's
 * TimezoneOverride and LanguageOverride fields; the session's are the floor
 * under them (dom/base/StealthSessionDeclaration.h), and the answer to the
 * process-wide questions that have no context to ask: the regional settings
 * of the machine (LocaleService::GetRegionalPrefsLocales, which still weighs
 * them against the app locale as upstream does) and the fallback language of
 * text without `lang` (nsLanguageAtomService::GetLocaleLanguage). Both asked
 * the host's OSPreferences, so a page measured the machine.
 *
 * Empty only when nothing is declared, i.e. the engine was launched without
 * the core: every caller then keeps Firefox's own path.
 *
 * Any thread.
 */

// A context's own declaration when it carries one, the session's otherwise:
// the argument holds the context's value on entry (a top BrowsingContext's
// field, or the copy a worker carried from it) and the declared one on exit.
void OrSessionLanguage(nsACString& aLanguage);
void OrSessionTimezone(nsAString& aZone);

// The session's language as one tag, or empty.
void SessionLanguageTag(nsACString& aTag);

// STEALTHFOX (firefox-50): the language the session declares is the language
// of text that declares none, which picks the fonts of its language group
// (nsFontCache, EncodingToLang, a canvas with no `lang`). It was the host's
// regional settings, which nsLanguageAtomService::GetLocaleLanguage still
// asks right after this when nothing is declared. Per process, not per
// context: a second context with a language of its own keeps the session's
// here, in its layout and its canvas alike.
//
// The session's language as a lowercase atom, or null when nothing is
// declared. The caller holds nsLanguageAtomService::mLock for writing.
already_AddRefed<nsAtom> SessionLocaleLanguage();

// The regional-settings locales of the machine the session declares: the
// declared language as the only entry when one is declared, else the
// operating system's (OSPreferences::GetRegionalPrefsLocales). Same contract
// as that method: on success aRetVal holds at least the entries it returns.
nsresult MachineRegionalPrefsLocales(nsTArray<nsCString>& aRetVal);

// The primary tag of an Accept-Language list: "fr-FR, fr, en-US, en" gives
// "fr-FR"; a single tag passes through. The one place the list becomes a tag,
// for every realm and every process-wide question that needs a BCP-47 tag.
// A realm takes only this tag; navigator.languages keeps the full list.
void PrimaryLanguageTag(const nsACString& aList, nsACString& aTag);

// STEALTHFOX: the languages "the machine" has, as the session declares them,
// in place of this machine's. `OSPreferences::GetSystemLocales` asked the
// operating system (Windows: the user's language list; Linux: setlocale), and
// everything built on it read the host: the order of the CJK fallback fonts
// (gfxPlatformFontList::FontPrefs), the negotiation of the app locale. The
// declared Accept-Language list ("it-IT, it, en-US, en") becomes the list a
// machine has: the tags in order, without a bare language that a regional
// tag of the same language already stands for ("it" next to "it-IT"), without
// q-values, without repeats.
//
// SystemLocalesFromDeclared is the function of a string (nothing declared
// gives false and leaves aLocales empty: the caller keeps Mozilla's own path).
// SessionSystemLocales reads the session's declaration.
bool SystemLocalesFromDeclared(const nsACString& aDeclared,
                               nsTArray<nsCString>& aLocales);
//
// WHY THE DECLARATION IS READ WHEN THE LOCALES ARE ASKED, NOT WHERE THE HOST IS
// READ. User preferences (the profile's user.js, which carries
// juggler.locale.override) are applied after the first requests XPCOM startup
// can make, and OSPreferences caches what it reads for good. The one caller
// that can ask before the declaration is readable is the negotiation of the
// application's own locale: LocaleService::ReadRequestedLocales
// (intl/locale/LocaleService.cpp:101 and :119), which falls back to the
// operating system's locales when intl.locale.requested is empty, and the core
// declares that pref, so the declared one wins when it is there. What that
// negotiation decides is the language of the browser's own interface, which a
// page cannot read; everything a page can reach (the CJK order of
// gfxPlatformFontList::FontPrefs, gfxDWriteFontList.cpp:304 and :1295, the
// locales ContentParent.cpp:2720 hands to content processes) asks later, and
// asks here.
bool SessionSystemLocales(nsTArray<nsCString>& aLocales);
// The first of them, as a single tag; false when nothing is declared.
bool SessionSystemLocale(nsACString& aLocale);

// STEALTHFOX: the language of a WORKER thread's text is its context's.
//
// nsLanguageAtomService::GetLocaleLanguage is the language of text that
// declares none, and it is one atom for the process: a canvas of a worker has
// no document to ask, so an OffscreenCanvas in a worker of a context that
// declares ja-JP, in a browser launched en-US, drew with the session's
// language. A worker declares its own at its birth, on its own thread, when
// its realm is created (SetDeclaredRealmTimeAndLanguage): the tag is kept for
// the thread. WorkerLocaleLanguage is null on every thread that is not a
// worker with a declaration (the main thread above all), where the process's
// answer stands. The atom is interned for the life of the process, so it is
// valid on any thread.
// An empty list clears the thread's language.
void SetWorkerLocaleLanguage(const nsACString& aDeclared);
nsAtom* WorkerLocaleLanguage();

}  // namespace mozilla::intl

#endif  // mozilla_intl_StealthSessionPrefs_h__
