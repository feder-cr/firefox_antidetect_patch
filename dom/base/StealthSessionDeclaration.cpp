/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/StealthSessionDeclaration.h"

#include "MainThreadUtils.h"
#include "js/RealmOptions.h"
#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "mozilla/intl/StealthSessionPrefs.h"
#include "mozilla/intl/TimeZone.h"
#include "nsIGlobalObject.h"
#include "nsPIDOMWindow.h"
#include "nsRFPService.h"
#include "nsString.h"

namespace mozilla::dom {

void GetDeclaredTimezone(BrowsingContext* aContext, nsAString& aZone) {
  MOZ_ASSERT(NS_IsMainThread());
  aZone.Truncate();
  if (aContext) {
    aZone = aContext->Top()->GetTimezoneOverride();
  }
  intl::OrSessionTimezone(aZone);
}

void GetDeclaredLanguage(BrowsingContext* aContext, nsACString& aLanguage) {
  MOZ_ASSERT(NS_IsMainThread());
  aLanguage.Truncate();
  if (aContext) {
    aLanguage = aContext->Top()->GetLanguageOverride();
  }
  intl::OrSessionLanguage(aLanguage);
}

// The context a document belongs to: its own, or that of the window it is
// scoped to. A document parsed by DOMParser or loaded by XMLHttpRequest has
// no BrowsingContext, and it is the page that made it whose zone a script
// compares it with.
static BrowsingContext* ContextOf(const Document* aDocument) {
  if (!aDocument) {
    return nullptr;
  }
  if (BrowsingContext* context = aDocument->GetBrowsingContext()) {
    return context;
  }
  if (nsIGlobalObject* global = aDocument->GetScopeObject()) {
    if (nsPIDOMWindowInner* window = global->GetAsInnerWindow()) {
      return window->GetBrowsingContext();
    }
  }
  return nullptr;
}

void GetDeclaredTimezone(const Document* aDocument, nsAString& aZone) {
  GetDeclaredTimezone(ContextOf(aDocument), aZone);
}

void GetDeclaredLanguage(const Document* aDocument, nsACString& aLanguage) {
  GetDeclaredLanguage(ContextOf(aDocument), aLanguage);
}

void SetRealmTimeAndLanguage(JS::RealmOptions& aOptions, bool aForceUTC,
                             bool aLocaleEnUS, const nsACString& aLanguage,
                             const nsAString& aZone) {
  if (aForceUTC) {
    nsCString timeZone = nsRFPService::GetSpoofedJSTimeZone();
    aOptions.behaviors().setTimeZoneOverride(timeZone.get());
  } else if (!aZone.IsEmpty()) {
    aOptions.behaviors().setTimeZoneOverride(
        NS_ConvertUTF16toUTF8(aZone).get());
  }
  if (aLocaleEnUS) {
    nsCString locale = nsRFPService::GetSpoofedJSLocale();
    aOptions.behaviors().setLocaleOverride(locale.get());
  } else if (!aLanguage.IsEmpty()) {
    // The override may be an Accept-Language list ("fr-FR, fr") so that
    // navigator.languages reports the whole desktop-default list; the realm's
    // Intl default locale is a single BCP-47 tag.
    nsAutoCString tag;
    intl::PrimaryLanguageTag(aLanguage, tag);
    aOptions.behaviors().setLocaleOverride(tag.get());
  }
}

void ExplodeLocalTime(PRTime aTime, const Document* aDocument,
                      PRExplodedTime* aExploded) {
  MOZ_ASSERT(aDocument);
  if (aDocument->ShouldResistFingerprinting(RFPTarget::JSDateTimeUTC)) {
    PR_ExplodeTime(aTime, PR_GMTParameters, aExploded);
    return;
  }
  nsAutoString declared;
  GetDeclaredTimezone(aDocument, declared);
  if (declared.IsEmpty()) {
    PR_ExplodeTime(aTime, PR_LocalTimeParameters, aExploded);
    return;
  }
  NS_ConvertUTF16toUTF8 zone(declared);
  int32_t offsetMs = 0;
  int32_t dstMs = 0;
  auto created = intl::TimeZone::TryCreate(
      Some(Span<const char>(zone.get(), zone.Length())));
  if (created.isOk()) {
    UniquePtr<intl::TimeZone> timeZone = created.unwrap();
    const int64_t utcMs = aTime / PR_USEC_PER_MSEC;
    auto offset = timeZone->GetOffsetMs(utcMs);
    auto dst = timeZone->GetDSTOffsetMs(utcMs);
    if (offset.isOk() && dst.isOk()) {
      offsetMs = offset.unwrap();
      dstMs = dst.unwrap();
    }
  }
  // The wall clock of the zone is UTC moved by the zone's whole offset at
  // that instant. The parameters split that offset into standard time and
  // daylight saving, as PR_LocalTimeParameters does; EXSLT's date prints
  // their sum.
  PR_ExplodeTime(aTime + PRTime(offsetMs) * PR_USEC_PER_MSEC, PR_GMTParameters,
                 aExploded);
  aExploded->tm_params.tp_gmt_offset = (offsetMs - dstMs) / 1000;
  aExploded->tm_params.tp_dst_offset = dstMs / 1000;
}

}  // namespace mozilla::dom
