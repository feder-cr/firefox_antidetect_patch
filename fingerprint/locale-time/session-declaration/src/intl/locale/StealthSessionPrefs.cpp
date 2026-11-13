/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/intl/StealthSessionPrefs.h"

#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPrefs_juggler.h"
#include "mozilla/ThreadLocal.h"
#include "mozilla/intl/OSPreferences.h"
#include "nsAtom.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::intl {

void OrSessionLanguage(nsACString& aLanguage) {
  if (aLanguage.IsEmpty()) {
    auto declared = StaticPrefs::juggler_locale_override();
    aLanguage = *declared;
  }
}

void OrSessionTimezone(nsAString& aZone) {
  if (aZone.IsEmpty()) {
    auto declared = StaticPrefs::juggler_timezone_override();
    CopyUTF8toUTF16(*declared, aZone);
  }
}

void SessionLanguageTag(nsACString& aTag) {
  nsAutoCString list;
  OrSessionLanguage(list);
  PrimaryLanguageTag(list, aTag);
}

already_AddRefed<nsAtom> SessionLocaleLanguage() {
  nsAutoCString declared;
  SessionLanguageTag(declared);
  if (declared.IsEmpty()) {
    return nullptr;
  }
  ToLowerCase(declared);  // use lowercase for all language atoms
  return NS_Atomize(declared);
}

// STEALTHFOX (firefox-50): the regional settings of the machine the session
// declares, in place of this machine's: its language when one is declared,
// else what the operating system says. Only the INPUT changes; the rule in
// LocaleService::GetRegionalPrefsLocales, which keeps the app locale unless
// the regional language matches it, stays upstream's, because this is an
// en-US build and a real en-US build on a de-DE machine formats as en-US.
// Before, the input was the host's, so the same session gave en-GB on an
// en-GB machine and en-US on any other, in the main runtime's default locale
// (XPCLocale), the locales date and time inputs render with
// (GetWebExposedLocales, datetimebox.js) and AppDateTimeFormat.
nsresult MachineRegionalPrefsLocales(nsTArray<nsCString>& aRetVal) {
  nsAutoCString declared;
  SessionLanguageTag(declared);
  if (!declared.IsEmpty()) {
    aRetVal.Clear();
    aRetVal.AppendElement(declared);
    return NS_OK;
  }
  return OSPreferences::GetInstance()->GetRegionalPrefsLocales(aRetVal);
}

bool SystemLocalesFromDeclared(const nsACString& aDeclared,
                               nsTArray<nsCString>& aLocales) {
  aLocales.Clear();
  nsTArray<nsCString> tags;
  nsCCharSeparatedTokenizer tokens(aDeclared, ',');
  while (tokens.hasMoreTokens()) {
    nsAutoCString tag(tokens.nextToken());
    int32_t q = tag.FindChar(';');
    if (q >= 0) {
      tag.Truncate(q);
    }
    tag.Trim(" \t");
    if (!tag.IsEmpty()) {
      tags.AppendElement(tag);
    }
  }
  for (const auto& tag : tags) {
    bool stoodForByARegionalTag = false;
    if (tag.FindChar('-') < 0) {
      for (const auto& other : tags) {
        stoodForByARegionalTag |= other.Length() > tag.Length() &&
                                  other.CharAt(tag.Length()) == '-' &&
                                  StringBeginsWith(other, tag);
      }
    }
    if (!stoodForByARegionalTag && !aLocales.Contains(tag)) {
      aLocales.AppendElement(tag);
    }
  }
  return !aLocales.IsEmpty();
}

bool SessionSystemLocales(nsTArray<nsCString>& aLocales) {
  nsAutoCString declared;
  OrSessionLanguage(declared);
  return SystemLocalesFromDeclared(declared, aLocales);
}

bool SessionSystemLocale(nsACString& aLocale) {
  nsTArray<nsCString> locales;
  if (!SessionSystemLocales(locales)) {
    return false;
  }
  aLocale = locales[0];
  return true;
}

static MOZ_THREAD_LOCAL(nsAtom*) sWorkerLanguage;

static bool EnsureWorkerLanguageSlot() {
  static const bool sInitialized = sWorkerLanguage.init();
  return sInitialized;
}

void SetWorkerLocaleLanguage(const nsACString& aDeclared) {
  if (!EnsureWorkerLanguageSlot()) {
    return;
  }
  nsAutoCString tag;
  PrimaryLanguageTag(aDeclared, tag);
  if (tag.IsEmpty()) {
    sWorkerLanguage.set(nullptr);  // nothing declared: the thread has no language of its own
    return;
  }
  ToLowerCase(tag);  // use lowercase for all language atoms
  // Kept for the life of the process: the slot holds a bare pointer, and the
  // thread that wrote it may outlive any one reference. A handful of tags at
  // most (one per language a session declares). Reachable, never freed.
  static StaticMutex sMutex MOZ_UNANNOTATED;
  static nsTArray<RefPtr<nsAtom>>* sKept = new nsTArray<RefPtr<nsAtom>>();
  RefPtr<nsAtom> atom = NS_Atomize(tag);
  {
    StaticMutexAutoLock lock(sMutex);
    if (!sKept->Contains(atom)) {
      sKept->AppendElement(atom);
    }
  }
  sWorkerLanguage.set(atom);
}

nsAtom* WorkerLocaleLanguage() {
  return EnsureWorkerLanguageSlot() ? sWorkerLanguage.get() : nullptr;
}

void PrimaryLanguageTag(const nsACString& aList, nsACString& aTag) {
  aTag = aList;
  int32_t comma = aTag.FindChar(',');
  if (comma >= 0) {
    aTag.Truncate(comma);
  }
  aTag.Trim(" \t");
}

}  // namespace mozilla::intl
