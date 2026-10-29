/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StealthWorkerDeclaration_h
#define mozilla_dom_StealthWorkerDeclaration_h

#include "mozilla/dom/RemoteWorkerTypes.h"

class nsIPrincipal;

namespace mozilla::dom {

/**
 * STEALTHFOX: the language and the zone of the context a service worker
 * belongs to, written into the worker's RemoteWorkerData.
 *
 * A service worker has no owner document: it is keyed by principal, origin
 * attributes included, so it is shared by every page of ONE user context (one
 * browser context of the automation client) and only by them. Juggler writes
 * the same LanguageOverride and TimezoneOverride on every top-level
 * BrowsingContext of a context (PageTarget.updateLanguageOverride,
 * updateTimezoneOverride), so any of those pages answers the question, and the
 * window that registered the worker and the worker itself end up with the same
 * language and the same clock. Retail Firefox 151, measured, gives a window and
 * its dedicated, shared and service workers the same navigator.languages, Intl
 * locale and zone; with one of each per process that is automatic, with one per
 * context it has to be carried here.
 *
 * It sets aData.languageOverrideLocale() and aData.timezoneOverride(); each is
 * empty when no open page of that context is found, or when its pages disagree
 * on it (an override set per tab, not by Juggler): the worker then gets the
 * session's declaration in WorkerPrivate. Only the locale travels: the resolved
 * navigator.languages list stays empty and WorkerPrivate derives it from the
 * locale in the worker process.
 *
 * ServiceWorkerPrivate::Initialize creates the RemoteWorkerData with an empty
 * language and zone; ServiceWorkerPrivate::RefreshRemoteWorkerData, which runs
 * in Initialize and before every spawn, calls this to fill them.
 *
 * Main thread only (it reads BrowsingContext fields). It scans every
 * BrowsingContextGroup on each call, i.e. once per service-worker spawn.
 */
void StealthWorkerDeclaration(nsIPrincipal* aPrincipal,
                              RemoteWorkerData& aData);

}  // namespace mozilla::dom

#endif  // mozilla_dom_StealthWorkerDeclaration_h
