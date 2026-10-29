/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthSpeechVoices.h"

#include "SpeechSynthesis.h"
#include "SpeechSynthesisVoice.h"
#include "mozilla/Assertions.h"
#include "mozilla/RefPtr.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "nsIObserver.h"
#include "nsISupportsUtils.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsTArray.h"

namespace mozilla::dom {

// Stealth: if zoom.stealth.voices.list pref is set, fabricate voices from
// the pref and bypass the OS voice registry entirely. Each pref entry has
// the form "NAME|LANG|D|L" (D=default 0|1, L=local 0|1), comma-separated.
// The synthetic voice URI encodes all four fields so the accessors in
// SpeechSynthesisVoice.cpp can answer queries without a registry lookup.
bool StealthGetVoices(
    SpeechSynthesis* aSelf, nsTArray<RefPtr<SpeechSynthesisVoice>>& aResult,
    nsRefPtrHashtable<nsStringHashKey, SpeechSynthesisVoice>& aCache) {
  MOZ_ASSERT(aSelf);
  // GetVoices clears aResult before calling us; uris below runs parallel to it.
  MOZ_ASSERT(aResult.IsEmpty());

  nsAutoCString voicesPref;
  {
    auto lock = mozilla::StaticPrefs::zoom_stealth_voices_list();
    voicesPref = *lock;
  }
  if (voicesPref.IsEmpty()) {
    return false;
  }

  nsISupports* voiceParent = NS_ISUPPORTS_CAST(nsIObserver*, aSelf);
  // The voice URIs in the order of aResult. SpeechSynthesisVoice::mUri is
  // private (only SpeechSynthesis is a friend), and the constructor stores the
  // string verbatim, so the strings built here are the cache keys.
  nsTArray<nsString> uris;
  int32_t start = 0;
  while (start < static_cast<int32_t>(voicesPref.Length())) {
    int32_t comma = voicesPref.FindChar(',', start);
    if (comma < 0) comma = voicesPref.Length();
    nsAutoCString entry(Substring(voicesPref, start, comma - start));
    entry.Trim(" \t\r\n");
    start = comma + 1;
    if (entry.IsEmpty()) continue;
    // Synthetic URI: "stealth-voice:NAME|LANG|D|L" — accessors parse it.
    nsAutoString uri;
    uri.AssignLiteral("stealth-voice:");
    AppendUTF8toUTF16(entry, uri);
    RefPtr<SpeechSynthesisVoice> voice = aCache.Get(uri);
    if (!voice) {
      voice = new SpeechSynthesisVoice(voiceParent, uri);
    }
    aResult.AppendElement(voice);
    uris.AppendElement(uri);
  }
  MOZ_ASSERT(uris.Length() == aResult.Length());
  aCache.Clear();
  for (uint32_t i = 0; i < aResult.Length(); i++) {
    aCache.InsertOrUpdate(uris[i], RefPtr{aResult[i].get()});
  }
  return true;
}

}  // namespace mozilla::dom
