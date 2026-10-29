/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthWorkerDeclaration.h"

#include "MainThreadUtils.h"
#include "mozilla/Assertions.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/OriginAttributes.h"
#include "mozilla/RefPtr.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/BrowsingContextGroup.h"
#include "nsIPrincipal.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::dom {

void StealthWorkerDeclaration(nsIPrincipal* aPrincipal,
                              RemoteWorkerData& aData) {
  AssertIsOnMainThread();
  MOZ_ASSERT(aPrincipal);

  // Same objects the inline code wrote through its two reference parameters.
  nsACString& aLanguage = aData.languageOverrideLocale();
  nsAString& aZone = aData.timezoneOverride();

  aLanguage.Truncate();
  aZone.Truncate();

  const OriginAttributes& attrs =
      BasePrincipal::Cast(aPrincipal)->OriginAttributesRef();

  nsTArray<RefPtr<BrowsingContextGroup>> groups;
  BrowsingContextGroup::GetAllGroups(groups);

  bool seen = false;
  bool languageAgrees = true;
  bool zoneAgrees = true;
  for (const RefPtr<BrowsingContextGroup>& group : groups) {
    for (const RefPtr<BrowsingContext>& top : group->Toplevels()) {
      if (top->IsDiscarded() || !top->IsContent()) {
        continue;
      }
      const OriginAttributes& topAttrs = top->OriginAttributesRef();
      if (topAttrs.mUserContextId != attrs.mUserContextId ||
          topAttrs.mPrivateBrowsingId != attrs.mPrivateBrowsingId) {
        continue;
      }
      if (!seen) {
        aLanguage = top->GetLanguageOverride();
        aZone = top->GetTimezoneOverride();
        seen = true;
        continue;
      }
      languageAgrees =
          languageAgrees && aLanguage.Equals(top->GetLanguageOverride());
      zoneAgrees = zoneAgrees && aZone.Equals(top->GetTimezoneOverride());
    }
  }
  if (!languageAgrees) {
    aLanguage.Truncate();
  }
  if (!zoneAgrees) {
    aZone.Truncate();
  }
}

}  // namespace mozilla::dom
