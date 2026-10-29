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

}  // namespace mozilla::intl

#endif  // mozilla_intl_StealthSessionPrefs_h__
