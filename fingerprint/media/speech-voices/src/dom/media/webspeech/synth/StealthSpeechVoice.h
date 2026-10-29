/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_MEDIA_WEBSPEECH_SYNTH_STEALTHSPEECHVOICE_H_
#define DOM_MEDIA_WEBSPEECH_SYNTH_STEALTHSPEECHVOICE_H_

#include "mozilla/Maybe.h"
#include "nsStringFwd.h"

namespace mozilla::dom {

// Stealth: voice URIs of the form "stealth-voice:NAME|LANG|D|L" are fabricated
// in SpeechSynthesis::GetVoices when the zoom.stealth.voices.list pref is set.
// All accessors short-circuit here and parse fields directly from the URI,
// so there's no round-trip through nsSynthVoiceRegistry (which would fail:
// these voices never get registered with the OS voice backend).
//
// Contract of every function here: for a URI that is not in the internal form
// the function returns false (or Nothing) and leaves its output untouched, so
// the caller falls through to the nsSynthVoiceRegistry path unchanged.

// Build the internal URI "stealth-voice:" + aEntry (UTF-8 -> UTF-16). aEntry is
// one trimmed "NAME|LANG|D|L" entry of the pref.
void StealthVoiceMakeURI(const nsACString& aEntry, nsAString& aUri);

// voiceURI as the page sees it: the form a retail Windows Firefox reports,
// "urn:moz-tts:sapi:" + name + "?" + lang. The internal form must never reach
// the page: it would be a string that names the engine instead of a voice.
// The scheme is "sapi:" on every platform on purpose: the declared platform is
// Windows everywhere (retail Linux would say "speechd:", macOS "osx:"), which
// also makes the Windows and Linux builds report the same value.
bool StealthVoiceRetailURI(const nsAString& aUri, nsAString& aOut);

// SpeechSynthesisVoice.name / .lang.
bool StealthVoiceName(const nsAString& aUri, nsAString& aOut);
bool StealthVoiceLang(const nsAString& aUri, nsAString& aOut);

// SpeechSynthesisVoice.localService / .default: Some(value) for an internal
// URI, Nothing otherwise.
Maybe<bool> StealthVoiceLocal(const nsAString& aUri);
Maybe<bool> StealthVoiceDefault(const nsAString& aUri);

}  // namespace mozilla::dom

#endif  // DOM_MEDIA_WEBSPEECH_SYNTH_STEALTHSPEECHVOICE_H_
