/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "StealthGfxFeatureStatus.h"

#include "GfxInfoBase.h"
#include "StealthDeclarationGate.h"
#include "mozilla/StaticMutex.h"
#include "mozilla/StaticPrefs_zoom.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/gfx/StealthRefuse.h"
#include "nsCharSeparatedTokenizer.h"
#include "nsIGfxInfo.h"
#include "nsXULAppAPI.h"

namespace mozilla::gfx {

namespace {

using widget::GfxInfoBase;

struct FeatureName {
  const char* mName;
  int32_t mId;
};

// Every live feature, by the id the core writes. Built from the same .inc as
// nsIGfxInfo, so it cannot drift from the feature ids.
constexpr FeatureName kFeatures[] = {
#define GFXINFO_FEATURE(id, pref) {#id, nsIGfxInfo::FEATURE_##id},
#define GFXINFO_FEATURE_RETIRED(id, pref)
#define GFXINFO_FEATURE_ALLOWLIST(id, pref) GFXINFO_FEATURE(id, pref)
#define GFXINFO_FEATURE_MISMATCHED(id, name, pref) GFXINFO_FEATURE(id, pref)
#include "mozilla/widget/GfxInfoFeatureDefs.inc"
#undef GFXINFO_FEATURE
#undef GFXINFO_FEATURE_RETIRED
#undef GFXINFO_FEATURE_ALLOWLIST
#undef GFXINFO_FEATURE_MISMATCHED
};

// FEATURE_ALL and FEATURE_OPTIONAL sit before FEATURE_START: they are
// blocklist wildcards, not features a caller asks about.
bool IsLive(int32_t aId) {
  return aId >= nsIGfxInfo::FEATURE_START &&
         aId < nsIGfxInfo::FEATURE_COUNT && !GfxInfoBase::IsFeatureRetired(aId);
}

int32_t IdOf(const nsACString& aName) {
  for (const auto& f : kFeatures) {
    if (IsLive(f.mId) && aName.EqualsASCII(f.mName)) {
      return f.mId;
    }
  }
  return nsIGfxInfo::FEATURE_INVALID;
}

const char* NameOf(int32_t aId) {
  for (const auto& f : kFeatures) {
    if (f.mId == aId) {
      return f.mName;
    }
  }
  return "?";
}

bool IsFailureId(const nsACString& aId) {
  if (aId.IsEmpty() || aId.Length() > 96) {
    return false;
  }
  for (uint32_t i = 0; i < aId.Length(); ++i) {
    const char c = aId.CharAt(i);
    const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == '_';
    if (!ok) {
      return false;
    }
  }
  return true;
}

// The statuses retail's matcher can give a feature. An allowlist feature
// (zero-copy video) is never OK: it is DENIED, ALLOW_* or blocked, and its
// consumers need exactly ALLOW_ALWAYS. Any other feature is never DENIED or
// ALLOW_*. INVALID and UNKNOWN are never an answer.
bool StatusAllowed(int32_t aFeature, int32_t aStatus) {
  if (aStatus < nsIGfxInfo::FEATURE_STATUS_OK ||
      aStatus > nsIGfxInfo::FEATURE_BLOCKED_PLATFORM_TEST ||
      aStatus == nsIGfxInfo::FEATURE_STATUS_UNKNOWN) {
    return false;
  }
  const bool allowStatus = aStatus == nsIGfxInfo::FEATURE_DENIED ||
                           GfxInfoBase::MatchingAllowStatus(aStatus);
  if (GfxInfoBase::IsFeatureAllowlisted(aFeature)) {
    return aStatus != nsIGfxInfo::FEATURE_STATUS_OK;
  }
  return !allowStatus;
}

bool ParseStatus(const nsACString& aText, int32_t* aStatus) {
  if (aText.IsEmpty() || aText.Length() > 2) {
    return false;
  }
  int32_t value = 0;
  for (uint32_t i = 0; i < aText.Length(); ++i) {
    const char c = aText.CharAt(i);
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  *aStatus = value;
  return true;
}

StaticMutex sMutex MOZ_UNANNOTATED;
StaticAutoPtr<StealthGfxFeatureTable> sTable;

}  // namespace

bool StealthParseGfxFeatureStatus(const nsACString& aDeclaration,
                                  StealthGfxFeatureTable& aTable,
                                  nsACString& aError) {
  aTable.Clear();
  StealthGfxFeatureTable table;
  table.SetLength(nsIGfxInfo::FEATURE_COUNT);
  if (aDeclaration.IsEmpty()) {
    aError.AssignLiteral("the declaration is empty");
    return false;
  }
  for (const auto& entry : aDeclaration.Split(',')) {
    nsTArray<nsCString> fields;
    for (const auto& field : entry.Split('|')) {
      fields.AppendElement(field);
    }
    if (fields.Length() < 2 || fields.Length() > 3) {
      aError = "an entry is not NAME|STATUS[|FAILURE]: \""_ns + entry + "\""_ns;
      return false;
    }
    for (const auto& field : fields) {
      if (field.IsEmpty() || field.FindChar(' ') != kNotFound) {
        aError = "an entry has an empty or spaced field: \""_ns + entry + "\""_ns;
        return false;
      }
    }
    const int32_t id = IdOf(fields[0]);
    if (id == nsIGfxInfo::FEATURE_INVALID) {
      aError = "unknown or retired feature "_ns + fields[0];
      return false;
    }
    if (table[id].isSome()) {
      aError = "feature declared twice: "_ns + fields[0];
      return false;
    }
    int32_t status = 0;
    if (!ParseStatus(fields[1], &status) || !StatusAllowed(id, status)) {
      aError = "status "_ns + fields[1] + " is not one retail gives to "_ns +
               fields[0];
      return false;
    }
    const bool hasFailure = fields.Length() == 3;
    if (status == nsIGfxInfo::FEATURE_STATUS_OK && hasFailure) {
      aError = "an OK feature carries a failure id: "_ns + fields[0];
      return false;
    }
    if (status != nsIGfxInfo::FEATURE_STATUS_OK &&
        status != nsIGfxInfo::FEATURE_DENIED && !hasFailure) {
      aError = "a non-OK feature has no failure id: "_ns + fields[0];
      return false;
    }
    if (hasFailure && !IsFailureId(fields[2])) {
      aError = "malformed failure id for "_ns + fields[0];
      return false;
    }
    StealthGfxFeatureEntry declared;
    declared.mStatus = status;
    if (hasFailure) {
      declared.mFailureId = fields[2];
    }
    table[id] = Some(std::move(declared));
  }
  for (int32_t id = nsIGfxInfo::FEATURE_START; id < nsIGfxInfo::FEATURE_COUNT;
       ++id) {
    if (IsLive(id) && table[id].isNothing()) {
      aError = "feature not declared: "_ns + nsDependentCString(NameOf(id));
      return false;
    }
  }
  aTable = std::move(table);
  return true;
}

bool StealthAnswerGfxFeature(const StealthGfxFeatureTable& aTable,
                             int32_t aFeature, nsACString& aFailureId,
                             int32_t* aStatus) {
  if (!IsLive(aFeature) || size_t(aFeature) >= aTable.Length() ||
      aTable[aFeature].isNothing()) {
    return false;
  }
  int32_t asked = aFeature;
#ifdef MOZ_WIDGET_GTK
  // WebGL on GTK draws on the GL the engine ships (llvmpipe behind EGL), never
  // on the host's, so whether the host has a GL (glxtest) says nothing about
  // it: on a host with no Mesa WebGL2 must exist all the same.
  if (aFeature == nsIGfxInfo::FEATURE_WEBGL_OPENGL) {
    asked = nsIGfxInfo::FEATURE_WEBGL;
  }
#endif
  const StealthGfxFeatureEntry& entry = *aTable[asked];
  *aStatus = entry.mStatus;
  aFailureId = entry.mFailureId;
  return true;
}

bool StealthGfxDeclaredStatus(nsIGfxInfo* /* aGfxInfo */, int32_t aFeature,
                              nsACString& aFailureId, int32_t* aStatus) {
  if (!StealthEngineActive() || !IsLive(aFeature)) {
    return false;
  }
  if (XRE_IsContentProcess() || XRE_IsGPUProcess()) {
    // The parent's answers, which it took from the declaration.
    if (!GfxInfoBase::sFeatureStatus) {
      return false;
    }
    for (const auto& fs : *GfxInfoBase::sFeatureStatus) {
      if (fs.feature() == aFeature) {
        aFailureId = fs.failureId();
        *aStatus = fs.status();
        return true;
      }
    }
    return false;
  }
  StaticMutexAutoLock lock(sMutex);
  if (!sTable) {
    nsAutoCString declaration;
    {
      auto pref = StaticPrefs::zoom_stealth_gfx_feature_status();
      declaration = *pref;
    }
    auto table = MakeUnique<StealthGfxFeatureTable>();
    nsAutoCString error;
    if (!StealthParseGfxFeatureStatus(declaration, *table, error)) {
      StealthRefuse(
          "stealth: zoom.stealth.gfx.feature_status is not a valid "
          "declaration (%s). The graphics feature decisions are the retail "
          "ones for the declared GPU and come only from invisible_core; "
          "without them Mozilla would decide from this machine's adapter. "
          "Launch through invisible_core, or clear zoom.stealth.fpp.hw_seed "
          "to run as stock Firefox.",
          error.get());
    }
    sTable = table.release();
  }
  return StealthAnswerGfxFeature(*sTable, aFeature, aFailureId, aStatus);
}

bool StealthGfxDeclarationActive() { return StealthEngineActive(); }

bool StealthGfxSuggestedDriverVersion(nsAString& aVersion) {
  if (!StealthEngineActive()) {
    return false;
  }
  aVersion.Truncate();
  return true;
}

void StealthResetGfxFeatureStatusForTesting() {
  StaticMutexAutoLock lock(sMutex);
  sTable = nullptr;
}

}  // namespace mozilla::gfx
