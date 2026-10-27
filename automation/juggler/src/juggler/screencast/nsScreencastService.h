/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#pragma once

#include <map>

#include "mozilla/RefPtr.h"
#include "nsCOMPtr.h"
#include "nsIScreencastService.h"
#include "nsString.h"

namespace mozilla {

class nsScreencastService final : public nsIScreencastService {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSISCREENCASTSERVICE

  static already_AddRefed<nsIScreencastService> GetSingleton();

  nsScreencastService();

 private:
  ~nsScreencastService();

  class Session;
  // Main thread only. Each session's capture module lives on that session's
  // own control thread, never this one: the comment on Session says why.
  std::map<nsString, RefPtr<Session>> mIdToSession;
};

}  // namespace mozilla
