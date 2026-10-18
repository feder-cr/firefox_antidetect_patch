/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/intl/StealthSessionPrefs.h"

#include "mozilla/StaticPrefs_juggler.h"
#include "nsString.h"

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

void PrimaryLanguageTag(const nsACString& aList, nsACString& aTag) {
  aTag = aList;
  int32_t comma = aTag.FindChar(',');
  if (comma >= 0) {
    aTag.Truncate(comma);
  }
  aTag.Trim(" \t");
}

}  // namespace mozilla::intl
