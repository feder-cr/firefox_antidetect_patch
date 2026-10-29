/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthSpeechVoice.h"

#include <cstdint>

#include "nsDependentSubstring.h"
#include "nsReadableUtils.h"
#include "nsString.h"

namespace mozilla::dom {

namespace {

constexpr auto kStealthVoicePrefix = u"stealth-voice:"_ns;

bool IsStealthVoiceURI(const nsAString& aUri) {
  return StringBeginsWith(aUri, kStealthVoicePrefix);
}

// Extract field `idx` from the URI's trailing "NAME|LANG|D|L" section.
// idx 0=name, 1=lang, 2=default("0"/"1"), 3=local("0"/"1"). Missing = "".
void GetStealthVoiceField(const nsAString& aUri, uint32_t idx,
                          nsAString& aOut) {
  aOut.Truncate();
  const uint32_t prefixLen = kStealthVoicePrefix.Length();
  if (aUri.Length() <= prefixLen) return;
  nsDependentSubstring body(aUri, prefixLen);
  uint32_t cur = 0;
  int32_t start = 0;
  while (start <= static_cast<int32_t>(body.Length())) {
    int32_t bar = body.FindChar(u'|', start);
    if (bar < 0) bar = body.Length();
    if (cur == idx) {
      aOut = Substring(body, start, bar - start);
      return;
    }
    start = bar + 1;
    cur++;
  }
}

}  // namespace

void StealthVoiceMakeURI(const nsACString& aEntry, nsAString& aUri) {
  aUri.Assign(kStealthVoicePrefix);
  AppendUTF8toUTF16(aEntry, aUri);
}

// QUI USCIVA IL NOSTRO FORMATO INTERNO, FINO ALLA PAGINA.
//
// `mUri` e' la nostra codifica di servizio, "stealth-voice:NOME|LANG|D|L", e
// ogni altro accessor di questo file la sa interpretare. Questo no: tornava
// `mUri` verbatim, quindi una pagina che chiama
// `speechSynthesis.getVoices()[0].voiceURI` leggeva una stringa che comincia
// con la parola "stealth". Misurato il 2026-08-23 sul prodotto:
//   a mano   -> urn:moz-tts:sapi:Microsoft David - English (United States)?en-US
//   prodotto -> stealth-voice:Microsoft David - English (United States)|en-US|1|1
// Non e' un camuffamento imperfetto: e' una firma che si autodenuncia, ed e'
// l'unica superficie di tutta la sessione di misure che nominava noi.
//
// Il formato retail non e' stato dedotto: e' copiato da
// `dom/media/webspeech/synth/windows/SapiService.cpp`, che costruisce
// `"urn:moz-tts:sapi:" + descrizione + "?" + locale`.
//
// E LO SCHEMA E' SEMPRE `sapi:`, ANCHE SU LINUX. Un Firefox Linux vero direbbe
// `speechd:` (`SpeechDispatcherService.cpp`) e uno macOS `osx:`
// (`OSXSpeechSynthesizerService.mm`), ma noi dichiariamo Windows sempre e
// ovunque: e' la regola 4, e ha l'effetto collaterale di rendere le due build
// identiche anche qui.
bool StealthVoiceRetailURI(const nsAString& aUri, nsAString& aOut) {
  if (!IsStealthVoiceURI(aUri)) return false;
  nsAutoString name, lang;
  GetStealthVoiceField(aUri, 0, name);
  GetStealthVoiceField(aUri, 1, lang);
  aOut.AssignLiteral(u"urn:moz-tts:sapi:");
  aOut.Append(name);
  aOut.AppendLiteral(u"?");
  aOut.Append(lang);
  return true;
}

bool StealthVoiceName(const nsAString& aUri, nsAString& aOut) {
  if (!IsStealthVoiceURI(aUri)) return false;
  GetStealthVoiceField(aUri, 0, aOut);
  return true;
}

bool StealthVoiceLang(const nsAString& aUri, nsAString& aOut) {
  if (!IsStealthVoiceURI(aUri)) return false;
  GetStealthVoiceField(aUri, 1, aOut);
  return true;
}

Maybe<bool> StealthVoiceLocal(const nsAString& aUri) {
  if (!IsStealthVoiceURI(aUri)) return Nothing();
  nsAutoString field;
  GetStealthVoiceField(aUri, 3, field);
  return Some(field.EqualsLiteral("1"));
}

Maybe<bool> StealthVoiceDefault(const nsAString& aUri) {
  if (!IsStealthVoiceURI(aUri)) return Nothing();
  nsAutoString field;
  GetStealthVoiceField(aUri, 2, field);
  return Some(field.EqualsLiteral("1"));
}

}  // namespace mozilla::dom
