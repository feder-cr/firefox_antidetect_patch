/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthWebGPUDeclaration.h"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <utility>

#include "StealthDeclarationGate.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/ErrorResult.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/WebGPUBinding.h"
#include "mozilla/gfx/StealthRefuse.h"
#include "mozilla/webgpu/ffi/wgpu.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsString.h"
#include "SupportedFeatures.h"
#include "SupportedLimits.h"

namespace mozilla::webgpu {

using gfx::StealthEngineActive;
using gfx::StealthRefuse;

// -
// The fallback answer, per adapter.
//
// Retail answers isFallbackAdapter true only for the adapter a page asked for
// with forceFallbackAdapter (WARP). The request knows the option and the
// Adapter constructor does not, so the request leaves a note keyed by the
// instance and the adapter id, and the constructor turns it into the answer of
// that adapter. WebGPU runs on the main thread and on workers, hence the lock.

static StaticMutex sFallbackLock MOZ_UNANNOTATED;
static std::map<std::pair<const Instance*, RawId>, bool>& PendingRequests() {
  static std::map<std::pair<const Instance*, RawId>, bool> sMap;
  return sMap;
}
static std::unordered_map<const Adapter*, bool>& AdapterAnswers() {
  static std::unordered_map<const Adapter*, bool> sMap;
  return sMap;
}

void StealthWebGPUNoteRequest(const Instance* aInstance, RawId aAdapter,
                              bool aForceFallback) {
  if (!StealthEngineActive()) {
    return;
  }
  StaticMutexAutoLock lock(sFallbackLock);
  PendingRequests()[{aInstance, aAdapter}] = aForceFallback;
}

Maybe<bool> StealthWebGPUDeclaredIsFallback(const Adapter* aAdapter) {
  if (!StealthEngineActive()) {
    return Nothing();
  }
  StaticMutexAutoLock lock(sFallbackLock);
  const auto it = AdapterAnswers().find(aAdapter);
  // An adapter the constructor declared always has an entry; one it did not
  // (none exists with the engine on) answers as a hardware adapter.
  return Some(it != AdapterAnswers().end() && it->second);
}

// -
// The declaration.

// The three declarations are DataMutexString prefs: read under their lock,
// then used as a copy, as the media declaration reads its own.
static nsAutoCString DeclaredAdapter() {
  nsAutoCString spec;
  auto lock = StaticPrefs::zoom_stealth_webgpu_adapter();
  spec = *lock;
  return spec;
}
static nsAutoCString DeclaredFeatures() {
  nsAutoCString spec;
  auto lock = StaticPrefs::zoom_stealth_webgpu_features();
  spec = *lock;
  return spec;
}
static nsAutoCString DeclaredLimits() {
  nsAutoCString spec;
  auto lock = StaticPrefs::zoom_stealth_webgpu_limits();
  spec = *lock;
  return spec;
}

bool StealthWebGPUNoAdapter() {
  if (!StealthEngineActive()) {
    return false;
  }
  const nsAutoCString adapter = DeclaredAdapter();
  if (adapter.EqualsLiteral("none")) {
    return true;
  }
  if (!adapter.EqualsLiteral("d3d12")) {
    StealthRefuse(
        "stealth: zoom.stealth.webgpu.adapter is \"%s\", not \"d3d12\" or "
        "\"none\". Engine rule 7: a missing or malformed declaration refuses "
        "rather than handing the page the host's adapter.",
        adapter.get());
  }
  return false;
}

Maybe<nsTArray<dom::GPUFeatureName>> StealthParseWebGPUFeatures(
    const nsACString& aSpec) {
  nsTArray<dom::GPUFeatureName> out;
  for (const nsACString& line : aSpec.Split('\n')) {
    if (line.IsEmpty()) {
      continue;
    }
    const Maybe<dom::GPUFeatureName> feature =
        dom::StringToEnum<dom::GPUFeatureName>(line);
    if (!feature) {
      return Nothing();
    }
    out.AppendElement(*feature);
  }
  return Some(std::move(out));
}

bool StealthParseWebGPULimits(const nsACString& aSpec, StealthLimitKey aKey,
                              ffi::WGPULimits& aOut) {
  // Every limit Firefox has must be declared exactly once: a limit left out
  // would keep the host's value, which is the leak this exists against.
  std::unordered_map<std::string_view, Limit> byKey;
  for (const auto limit : MakeInclusiveEnumeratedRange(Limit::_LAST)) {
    byKey[aKey(limit)] = limit;
  }
  size_t seen = 0;
  for (const nsACString& line : aSpec.Split('\n')) {
    if (line.IsEmpty()) {
      continue;
    }
    const int32_t eq = line.FindChar('=');
    if (eq <= 0) {
      return false;
    }
    const nsAutoCString name(Substring(line, 0, eq));
    const auto it = byKey.find(std::string_view(name.get(), name.Length()));
    if (it == byKey.end()) {
      return false;
    }
    nsresult rv;
    const uint64_t value =
        nsAutoCString(Substring(line, eq + 1)).ToUnsignedInteger64(&rv);
    if (NS_FAILED(rv)) {
      return false;
    }
    SetLimit(&aOut, it->second, value);
    byKey.erase(it);
    seen++;
  }
  return seen > 0 && byKey.empty();
}

bool StealthWebGPUDeclareAdapter(Adapter* aAdapter, const Instance* aInstance,
                                 RawId aId, RefPtr<SupportedFeatures>& aFeatures,
                                 ffi::WGPULimits& aLimits,
                                 StealthLimitKey aKey) {
  if (!StealthEngineActive()) {
    return false;
  }
  const nsAutoCString featureSpec = DeclaredFeatures();
  const auto features = StealthParseWebGPUFeatures(featureSpec);
  if (!features) {
    StealthRefuse(
        "stealth: zoom.stealth.webgpu.features names a feature this Firefox "
        "does not have. Engine rule 7: refused rather than answered from the "
        "host's adapter.");
  }
  ffi::WGPULimits limits = aLimits;
  const nsAutoCString limitSpec = DeclaredLimits();
  if (!StealthParseWebGPULimits(limitSpec, aKey, limits)) {
    StealthRefuse(
        "stealth: zoom.stealth.webgpu.limits does not declare every WebGPU "
        "limit exactly once as name=value. Engine rule 7: refused rather than "
        "answered from the host's adapter.");
  }

  RefPtr<SupportedFeatures> declared = new SupportedFeatures(aAdapter);
  IgnoredErrorResult ignored;
  for (const auto feature : *features) {
    declared->Add(feature, ignored);
  }
  aFeatures = std::move(declared);
  aLimits = limits;

  StaticMutexAutoLock lock(sFallbackLock);
  const auto note = PendingRequests().find({aInstance, aId});
  bool fallback = false;
  if (note != PendingRequests().end()) {
    fallback = note->second;
    PendingRequests().erase(note);
  }
  AdapterAnswers()[aAdapter] = fallback;
  return true;
}

void StealthWebGPUClampToReal(ffi::WGPUFfiDeviceDescriptor& aDesc,
                              const ffi::WGPUAdapterInformation& aReal,
                              StealthLimitKey aKey) {
  if (!StealthEngineActive()) {
    return;
  }
  aDesc.required_features &= aReal.features;
  for (const auto limit : MakeInclusiveEnumeratedRange(Limit::_LAST)) {
    const uint64_t asked = GetLimit(aDesc.required_limits, limit);
    const uint64_t real = GetLimit(aReal.limits, limit);
    const bool isMax = aKey(limit).substr(0, 3) == "max";
    SetLimit(&aDesc.required_limits, limit,
             isMax ? std::min(asked, real) : std::max(asked, real));
  }
}

}  // namespace mozilla::webgpu
