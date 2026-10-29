/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StealthSessionDeclaration_h
#define mozilla_dom_StealthSessionDeclaration_h

#include "js/TypeDecls.h"
#include "nsString.h"
#include "nsStringFwd.h"
#include "prtime.h"

namespace JS {
class RealmOptions;
}

namespace mozilla::dom {

class BrowsingContext;
class Document;

/**
 * STEALTHFOX (firefox-50): the ONE reader of the time zone and the language a
 * page declares, for every realm the engine creates for web content and every
 * value it computes from local time.
 *
 * WHAT IT READS. The TOP BrowsingContext's TimezoneOverride and
 * LanguageOverride fields: synchronised to every process, seeded in the parent
 * from the session's `juggler.timezone.override` / `juggler.locale.override`
 * (BrowsingContext::Attach), and replaced per context when a client sets a
 * context's own zone or language. When the field is empty, or there is no
 * context to ask (a document no window owns), it is the session's declaration
 * (intl/locale/StealthSessionPrefs.h, the one reader of those prefs).
 *
 * WHY ONE READER. Until firefox-50 the window took the field, workers and
 * worklets took the PROCESS's zone (`TZ`, read by ICU), worklets a default
 * locale nobody set, and `document.lastModified` and EXSLT's date the C
 * runtime's local time. On Windows no single `TZ` satisfies both of its
 * readers - the C runtime ignores an IANA name and ICU ignores most POSIX
 * forms - so a page saw the session's zone in Date and the machine's in a
 * Worker ([B334]) or in lastModified ([B339]); and a context with a zone of
 * its own saw the session's in all of those ([B346]). Every one of them now
 * asks here.
 *
 * Empty only when nothing is declared, i.e. the engine was launched without
 * invisible_core: every caller then keeps Firefox's own path, which is the
 * engine not having been told anything, not a fallback from a declaration.
 *
 * Main thread only: a BrowsingContext's fields are read there.
 *
 * BrowsingContext::DidSet(LanguageOverride) reads it too: the field, or the
 * session's declaration when the field is empty, so a context whose override
 * is cleared speaks the session's language, never the build's.
 *
 * BrowsingContext::DidSet(TimezoneOverride) reads it as well: the field, or
 * the session's declaration when the field is empty. It replaces a "lock" that
 * read the pref and preferred it to the field: in a content process the pref
 * arrived empty, so the lock held only in the parent, where no page realm
 * lives, and a context's own zone was overridden by the session's there only.
 * Now the field wins everywhere, as it does for the language, and a cleared
 * field falls to the session's zone in every process, never to the host's.
 */
void GetDeclaredTimezone(BrowsingContext* aContext, nsAString& aZone);
void GetDeclaredLanguage(BrowsingContext* aContext, nsACString& aLanguage);
// Workers carry their owner's fields in WorkerLoadInfoData and fall to the
// session's in WorkerPrivate, through the same intl::OrSession*.

// The same for a document: its BrowsingContext, or the window it is scoped to
// when it has none (a document made by DOMParser or XMLHttpRequest).
void GetDeclaredTimezone(const Document* aDocument, nsAString& aZone);
void GetDeclaredLanguage(const Document* aDocument, nsACString& aLanguage);

/**
 * STEALTHFOX (firefox-50): the declaration of this document's context, or the
 * session's when the context carries none, for the realm that is created for
 * the global of `aDocument`. Same values that GetDeclaredLanguage /
 * GetDeclaredTimezone give for `aDocument->GetBrowsingContext()` - NOT the
 * Document overloads above, which fall back to the scope window's context for
 * a document without one; a document that gets a global of its own has a
 * context of its own, and when it has none the session's values are what the
 * realm has always received. `aDocument` must not be null. Main thread only.
 */
nsCString GetDeclaredRealmLanguage(Document* aDocument);
nsString GetDeclaredRealmTimezone(Document* aDocument);

/**
 * The time zone and the default Intl locale of a realm the engine creates for
 * web content: resistFingerprinting's spoofed values when it asks for them,
 * else the declared ones, else nothing (Firefox's own). The window, worker and
 * worklet realms all come through here, so the rule exists once. `aLanguage`
 * may be an Accept-Language list: the realm takes its primary tag. Any
 * thread: the caller brings the declared values.
 */
void SetRealmTimeAndLanguage(JS::RealmOptions& aOptions, bool aForceUTC,
                             bool aLocaleEnUS, const nsACString& aLanguage,
                             const nsAString& aZone);

/**
 * BrowsingContext::Attach: if our `juggler.timezone.override` pref is set and
 * this is a top-level BC with no override yet, seed it from the pref. This
 * guarantees the pref is the single source of truth, even when no caller
 * (Playwright, WebDriver, ...) ever invokes Browser.setTimezoneOverride. The
 * setter triggers DidSet which propagates the value per-realm.
 *
 * Same single-source-of-truth pattern for the locale. The
 * juggler.locale.override pref seeds the LanguageOverride FIELD, whose DidSet
 * applies JS::SetRealmLocaleOverride per-realm - making Intl.DateTimeFormat /
 * NumberFormat / toLocaleString follow the locale. languageOverride (docShell)
 * only covers navigator.language + the Accept-Language header; without this the
 * realm's default Intl locale stays at the build app locale (en-US), a
 * navigator-vs-Intl mismatch flagged for non-US locales.
 *
 * The zone is seeded first, then the language. Main thread only.
 */
void SeedDeclaredTimezoneAndLanguage(BrowsingContext* aContext);

/**
 * Sets the default Intl locale of a window realm from the declared language.
 * The override may be an Accept-Language list ("fr-FR, fr") so that
 * navigator.languages reports the full desktop-default list; the realm's Intl
 * default locale is a single BCP-47 tag, so it takes the list's primary tag.
 * `aLanguage` must not be empty: the caller clears the override with nullptr
 * itself. Main thread only.
 */
void SetRealmDeclaredLocale(JS::Realm* aRealm, const nsACString& aLanguage);

/**
 * `aTime` exploded as the local time of a page in `aDocument`, with
 * `tm_params` filled: UTC when resistFingerprinting asks for it, else the
 * declared zone's wall clock at that instant (daylight saving included),
 * computed with ICU. It replaces PR_ExplodeTime with PR_LocalTimeParameters,
 * the C runtime's local time, which on Windows is the host's whatever `TZ`
 * says ([B339]), for document.lastModified and EXSLT's date:date-time; that
 * path remains only when nothing is declared. A zone ICU does not know is
 * exploded as UTC, never as the host's. Main thread only.
 */
void ExplodeLocalTime(PRTime aTime, const Document* aDocument,
                      PRExplodedTime* aExploded);

}  // namespace mozilla::dom

#endif  // mozilla_dom_StealthSessionDeclaration_h
