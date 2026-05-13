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
