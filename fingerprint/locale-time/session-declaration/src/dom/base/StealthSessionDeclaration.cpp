/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/StealthSessionDeclaration.h"

#include "MainThreadUtils.h"
#include "js/Realm.h"
#include "js/RealmOptions.h"
#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"
#include "mozilla/Span.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/Document.h"
#include "mozilla/intl/StealthSessionPrefs.h"
#include "mozilla/intl/TimeZone.h"
#include "nsAtom.h"
#include "nsGlobalWindowInner.h"
#include "nsIGlobalObject.h"
#include "nsLanguageAtomService.h"
#include "nsPresContext.h"
#include "nsPIDOMWindow.h"
#include "nsString.h"
#include "nsTArray.h"
#include "nsUnicharUtils.h"
#include "nsXULAppAPI.h"
#include "xpcpublic.h"

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

nsCString GetDeclaredRealmLanguage(Document* aDocument) {
  nsCString language;
  GetDeclaredLanguage(aDocument->GetBrowsingContext(), language);
  return language;
}

nsString GetDeclaredRealmTimezone(Document* aDocument) {
  nsString zone;
  GetDeclaredTimezone(aDocument->GetBrowsingContext(), zone);
  return zone;
}

void SeedDeclaredTimezoneAndLanguage(BrowsingContext* aContext) {
  MOZ_ASSERT(aContext);
  if (aContext->IsTop() && aContext->GetTimezoneOverride().IsEmpty() &&
      XRE_IsParentProcess()) {
    nsAutoString zone;
    intl::OrSessionTimezone(zone);
    if (!zone.IsEmpty()) {
      (void)aContext->SetTimezoneOverride(zone);
    }
  }

  if (aContext->IsTop() && aContext->GetLanguageOverride().IsEmpty() &&
      XRE_IsParentProcess()) {
    nsAutoCString language;
    intl::OrSessionLanguage(language);
    if (!language.IsEmpty()) {
      (void)aContext->SetLanguageOverride(language);
    }
  }
}

// Document's friend (Document.h): the one door to its private recompute.
class StealthDocumentAccess {
 public:
  static void RecomputeLanguage(Document* aDocument) {
    nsAtom* before = aDocument->mLanguageFromCharset;
    aDocument->RecomputeLanguageFromCharset();
    if (aDocument->mLanguageFromCharset != before) {
      // As a change of charset does: the fonts chosen for the old language
      // and the style computed with it are stale. The first container of a
      // document has no PresContext yet: nothing to flush.
      if (nsPresContext* presContext = aDocument->GetPresContext()) {
        presContext->DocumentCharSetChanged(aDocument->mCharacterSet);
      }
    }
  }
};

void RefreshDocumentLanguage(Document* aDocument) {
  StealthDocumentAccess::RecomputeLanguage(aDocument);
}

void SetRealmDeclaredLocale(JS::Realm* aRealm, const nsACString& aLanguage) {
  MOZ_ASSERT(NS_IsMainThread());
  MOZ_ASSERT(!aLanguage.IsEmpty());
  nsAutoCString tag;
  intl::PrimaryLanguageTag(aLanguage, tag);
  JS::SetRealmLocaleOverride(aRealm, tag.get());
  // The document of this window caches the language of its text with no
  // `lang`: it follows its context when the context's language changes.
  if (nsGlobalWindowInner* window = xpc::WindowOrNull(JS::GetRealmGlobalOrNull(aRealm))) {
    if (Document* document = window->GetExtantDoc()) {
      RefreshDocumentLanguage(document);
    }
  }
}

void SetDeclaredRealmTimeAndLanguage(JS::RealmOptions& aOptions,
                                     bool aForceUTC, bool aLocaleEnUS,
                                     const nsACString& aLanguage,
                                     const nsAString& aZone) {
  if (!aForceUTC && !aZone.IsEmpty()) {
    aOptions.behaviors().setTimeZoneOverride(
        NS_ConvertUTF16toUTF8(aZone).get());
  }
  if (!aLocaleEnUS && !aLanguage.IsEmpty()) {
    // The override may be an Accept-Language list ("fr-FR, fr") so that
    // navigator.languages reports the whole desktop-default list; the realm's
    // Intl default locale is a single BCP-47 tag.
    nsAutoCString tag;
    intl::PrimaryLanguageTag(aLanguage, tag);
    aOptions.behaviors().setLocaleOverride(tag.get());
  }
  if (!NS_IsMainThread()) {
    // This worker thread's text; empty clears it, for a thread that is given a
    // second realm with no declaration (a worklet's).
    intl::SetWorkerLocaleLanguage(aLocaleEnUS ? ""_ns : aLanguage);
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

nsAtom* ChooseCharsetLanguage(nsAtom* aFromCharset, nsAtom* aProcessLanguage,
                              nsAtom* aDeclared) {
  return aDeclared && aFromCharset == aProcessLanguage ? aDeclared
                                                       : aFromCharset;
}

nsAtom* InternedLanguage(const nsACString& aDeclared) {
  MOZ_ASSERT(NS_IsMainThread());
  // The last answer: the same list is asked for at every style resolution.
  static nsCString* sLastDeclared = new nsCString();  // never freed, like the atoms
  static nsAtom* sLastAtom = nullptr;
  if (sLastAtom && aDeclared.Equals(*sLastDeclared)) {
    return sLastAtom;
  }
  nsAutoCString tag;
  intl::PrimaryLanguageTag(aDeclared, tag);
  if (tag.IsEmpty()) {
    return nullptr;
  }
  ToLowerCase(tag);  // use lowercase for all language atoms
  // Kept for the life of the process, as the contract says: a Document holds
  // the pointer without owning it, and one can outlive XPCOM shutdown's
  // clearing of any static. A handful of tags at most (one per language a
  // session declares). Reachable, never freed.
  static nsTArray<RefPtr<nsAtom>>* sKept = new nsTArray<RefPtr<nsAtom>>();
  RefPtr<nsAtom> atom = NS_Atomize(tag);
  if (!sKept->Contains(atom)) {
    sKept->AppendElement(atom);
  }
  *sLastDeclared = aDeclared;
  sLastAtom = atom;
  return atom;
}

nsAtom* DocumentCharsetLanguage(const Document* aDocument,
                                nsAtom* aFromCharset) {
  nsAtom* process = nsLanguageAtomService::GetService()->GetLocaleLanguage();
  if (aFromCharset != process) {
    return aFromCharset;  // an encoding with a language of its own
  }
  nsAutoCString declared;
  GetDeclaredLanguage(aDocument, declared);
  return ChooseCharsetLanguage(aFromCharset, process,
                               InternedLanguage(declared));
}

}  // namespace mozilla::dom
