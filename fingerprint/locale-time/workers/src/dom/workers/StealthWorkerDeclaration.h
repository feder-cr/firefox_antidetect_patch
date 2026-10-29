/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StealthWorkerDeclaration_h
#define mozilla_dom_StealthWorkerDeclaration_h

#include "MainThreadUtils.h"
#include "js/RealmOptions.h"  // JS::SetRealmLocaleOverride
#include "js/TypeDecls.h"
#include "mozilla/Assertions.h"
#include "mozilla/dom/Navigator.h"
#include "mozilla/dom/WorkerLoadInfo.h"
#include "mozilla/intl/StealthSessionPrefs.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::dom {

/**
 * STEALTHFOX (firefox-50): the language and the zone a worker declares.
 * Header only, so that WorkerPrivate.cpp's hooks cost no line in moz.build;
 * the feature's history is in fingerprint/locale-time/workers/FEATURE.md.
 */

// Main thread only (the WorkerPrivate constructor, owner-less branch).
// Fills aLoadInfo's mLanguageOverrideLocale, mTimezoneOverride and
// mLanguageOverride with what this worker declares.
//
// The owner's language and zone, or the session's when the owner carries none
// (a service worker spawned while no page of its context is open, a chrome
// worker). From here on aLoadInfo holds what this worker declares, and its
// nested workers copy it (WorkerPrivate::GetLoadInfo).
//
// Then navigator.languages of this worker, from the same LanguageOverride its
// owner's window reads (Navigator::GetLanguages). GetLoadInfo already resolved
// the list for a worker with a window owner; it is still empty when the locale
// came from the session or from a RemoteWorkerData that only carries the
// locale (service workers). An empty list means nothing is declared:
// WorkerNavigator then keeps the process-wide list from intl.accept_languages,
// as upstream does.
//
// The worker's realm is born with the language and the zone its owner
// declares, through the same SetDeclaredRealmTimeAndLanguage the window's
// realm goes through (called at the end of xpc::InitGlobalObjectOptions). Upstream's
// update path (UpdateLanguageOverrideInternal, UpdateTimezoneOverrideInternal)
// only runs on a later BrowsingContext change; the session fallback is
// applied here, at birth, before RuntimeService::GetDefaultJSSettings and the
// realm options are read.
//
// Both realm arguments were empty literals once. The language until
// 2026-08-20: the worker spoke the machine's locale while its page spoke the
// declared one, in all of Intl and in toLocaleString ([B165]). The zone until
// firefox-50: the worker took the PROCESS's zone, the `TZ` the core put in the
// environment, which was right only where `TZ` was. On Windows the C runtime
// and ICU read `TZ` differently and no value satisfies both, so
// America/Phoenix gave the machine's zone in a Worker ([B334]), and a second
// context with a zone of its own gave the session's ([B346]).
inline void DeclareWorkerLanguageAndZone(WorkerLoadInfo& aLoadInfo) {
  MOZ_ASSERT(NS_IsMainThread());

  intl::OrSessionLanguage(aLoadInfo.mLanguageOverrideLocale);
  intl::OrSessionTimezone(aLoadInfo.mTimezoneOverride);

  if (!aLoadInfo.mLanguageOverrideLocale.IsEmpty() &&
      aLoadInfo.mLanguageOverride.IsEmpty()) {
    Navigator::GetAcceptLanguages(aLoadInfo.mLanguageOverride,
                                  &aLoadInfo.mLanguageOverrideLocale);
  }
}

// Worker thread. Sets the realm's Intl default locale from aLanguageOverride
// (an Accept-Language list or a single tag), as its primary tag.
//
// The override may be an Accept-Language list ("fr-FR, fr"); the realm's Intl
// default locale is a single BCP-47 tag, as at birth (SetDeclaredRealmTimeAndLanguage)
// and in the window (BrowsingContext::DidSet). Touches only the realm and its
// arguments.
inline void SetWorkerRealmLocale(JS::Realm* aRealm,
                                 const nsACString& aLanguageOverride) {
  nsAutoCString primaryLocale;
  intl::PrimaryLanguageTag(aLanguageOverride, primaryLocale);
  JS::SetRealmLocaleOverride(aRealm, primaryLocale.get());
}

// Worker thread. True when the worker follows its owner's LanguageOverride,
// i.e. it ignores the process-wide intl.accept_languages broadcast.
//
// UpdateLanguagesInternal is the intl.accept_languages broadcast. A worker that
// follows its owner's LanguageOverride ignores it, exactly as the owner's
// Navigator::GetLanguages does; its nested workers carry the same list. Reads
// only the worker's own load info.
inline bool WorkerFollowsOwnerLanguage(const WorkerLoadInfo& aLoadInfo) {
  return !aLoadInfo.mLanguageOverride.IsEmpty();
}

}  // namespace mozilla::dom

#endif  // mozilla_dom_StealthWorkerDeclaration_h
