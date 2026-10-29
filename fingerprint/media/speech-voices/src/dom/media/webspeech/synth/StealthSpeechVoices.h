/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_WEBSPEECH_SYNTH_STEALTHSPEECHVOICES_H_
#define DOM_MEDIA_WEBSPEECH_SYNTH_STEALTHSPEECHVOICES_H_

#include "mozilla/RefPtr.h"
#include "nsHashKeys.h"
#include "nsRefPtrHashtable.h"
#include "nsTArray.h"

namespace mozilla::dom {

class SpeechSynthesis;
class SpeechSynthesisVoice;

// Voice list declared by the zoom.stealth.voices.list pref instead of read
// from the OS voice registry. Called from SpeechSynthesis::GetVoices after its
// fingerprint-resistance early return, with aResult already cleared.
//
// Returns false (and touches nothing) when the pref is empty: the caller then
// runs the native path. Returns true when the pref is non-empty: aResult and
// aCache (the caller's mVoiceCache) now hold the declared voices and the caller
// must return without consulting the registry.
bool StealthGetVoices(
    SpeechSynthesis* aSelf, nsTArray<RefPtr<SpeechSynthesisVoice>>& aResult,
    nsRefPtrHashtable<nsStringHashKey, SpeechSynthesisVoice>& aCache);

}  // namespace mozilla::dom

#endif  // DOM_MEDIA_WEBSPEECH_SYNTH_STEALTHSPEECHVOICES_H_
