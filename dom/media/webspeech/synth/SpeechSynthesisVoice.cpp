/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "SpeechSynthesis.h"
#include "mozilla/dom/SpeechSynthesisVoiceBinding.h"
#include "nsSynthVoiceRegistry.h"

namespace mozilla::dom {

// Stealth: voice URIs of the form "stealth-voice:NAME|LANG|D|L" are fabricated
// in SpeechSynthesis::GetVoices when the zoom.stealth.voices.list pref is set.
// All accessors short-circuit here and parse fields directly from the URI,
// so there's no round-trip through nsSynthVoiceRegistry (which would fail:
// these voices never get registered with the OS voice backend).
static constexpr auto kStealthVoicePrefix = u"stealth-voice:"_ns;

static bool IsStealthVoiceURI(const nsAString& aUri) {
  return StringBeginsWith(aUri, kStealthVoicePrefix);
}

// Extract field `idx` from the URI's trailing "NAME|LANG|D|L" section.
// idx 0=name, 1=lang, 2=default("0"/"1"), 3=local("0"/"1"). Missing = "".
static void GetStealthVoiceField(const nsAString& aUri, uint32_t idx,
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

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(SpeechSynthesisVoice, mParent)
NS_IMPL_CYCLE_COLLECTING_ADDREF(SpeechSynthesisVoice)
NS_IMPL_CYCLE_COLLECTING_RELEASE(SpeechSynthesisVoice)
NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(SpeechSynthesisVoice)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

SpeechSynthesisVoice::SpeechSynthesisVoice(nsISupports* aParent,
                                           const nsAString& aUri)
    : mParent(aParent), mUri(aUri) {}

SpeechSynthesisVoice::~SpeechSynthesisVoice() = default;

JSObject* SpeechSynthesisVoice::WrapObject(JSContext* aCx,
                                           JS::Handle<JSObject*> aGivenProto) {
  return SpeechSynthesisVoice_Binding::Wrap(aCx, this, aGivenProto);
}

nsISupports* SpeechSynthesisVoice::GetParentObject() const { return mParent; }

void SpeechSynthesisVoice::GetVoiceURI(nsString& aRetval) const {
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
  if (IsStealthVoiceURI(mUri)) {
    nsAutoString name, lang;
    GetStealthVoiceField(mUri, 0, name);
    GetStealthVoiceField(mUri, 1, lang);
    aRetval.AssignLiteral(u"urn:moz-tts:sapi:");
    aRetval.Append(name);
    aRetval.AppendLiteral(u"?");
    aRetval.Append(lang);
    return;
  }
  aRetval = mUri;
}

void SpeechSynthesisVoice::GetName(nsString& aRetval) const {
  if (IsStealthVoiceURI(mUri)) {
    GetStealthVoiceField(mUri, 0, aRetval);
    return;
  }
  DebugOnly<nsresult> rv =
      nsSynthVoiceRegistry::GetInstance()->GetVoiceName(mUri, aRetval);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "Failed to get SpeechSynthesisVoice.name");
}

void SpeechSynthesisVoice::GetLang(nsString& aRetval) const {
  if (IsStealthVoiceURI(mUri)) {
    GetStealthVoiceField(mUri, 1, aRetval);
    return;
  }
  DebugOnly<nsresult> rv =
      nsSynthVoiceRegistry::GetInstance()->GetVoiceLang(mUri, aRetval);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "Failed to get SpeechSynthesisVoice.lang");
}

bool SpeechSynthesisVoice::LocalService() const {
  if (IsStealthVoiceURI(mUri)) {
    nsAutoString field;
    GetStealthVoiceField(mUri, 3, field);
    return field.EqualsLiteral("1");
  }
  bool isLocal;
  DebugOnly<nsresult> rv =
      nsSynthVoiceRegistry::GetInstance()->IsLocalVoice(mUri, &isLocal);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "Failed to get SpeechSynthesisVoice.localService");

  return isLocal;
}

bool SpeechSynthesisVoice::Default() const {
  if (IsStealthVoiceURI(mUri)) {
    nsAutoString field;
    GetStealthVoiceField(mUri, 2, field);
    return field.EqualsLiteral("1");
  }
  bool isDefault;
  DebugOnly<nsresult> rv =
      nsSynthVoiceRegistry::GetInstance()->IsDefaultVoice(mUri, &isDefault);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv),
                       "Failed to get SpeechSynthesisVoice.default");

  return isDefault;
}

}  // namespace mozilla::dom
