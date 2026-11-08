/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_STEALTH_GFX_FEATURE_STATUS_H_
#define GFX_STEALTH_GFX_FEATURE_STATUS_H_

// The graphics feature decisions (nsIGfxInfo::GetFeatureStatus) come from the
// declaration, zoom.stealth.gfx.feature_status: the status retail Firefox on
// Windows 11 gives each feature for the persona's real GPU, measured by the
// core. Mozilla decides them from the host's adapter (vendor, device, driver,
// OS): on a Windows VM or a GPU-less host it blocks WebGL2, accelerated ANGLE,
// WebGPU, and a page reads the machine through what is missing. Every
// decision, in every process and on every host, is the declared one.
//
// Format: "NAME|STATUS[|FAILURE],..." with NAME the id of a live feature in
// widget/GfxInfoFeatureDefs.inc (WEBGL_OPENGL, GL_NORM16_TEXTURES, ...),
// STATUS a decimal nsIGfxInfo status and FAILURE the retail failure id. Every
// live feature exactly once: a feature Mozilla adds at a port makes the engine
// refuse until the core has measured it.

#include "mozilla/Maybe.h"
#include "nsString.h"
#include "nsTArray.h"

class nsIGfxInfo;

namespace mozilla::gfx {

struct StealthGfxFeatureEntry {
  int32_t mStatus = 0;
  nsCString mFailureId;
};

// One slot per nsIGfxInfo feature id; Nothing() for ids outside the live set.
using StealthGfxFeatureTable = nsTArray<Maybe<StealthGfxFeatureEntry>>;

// Parses and validates a declaration. False with the reason in aError when it
// breaks any rule (see the .cpp); aTable is then left empty.
bool StealthParseGfxFeatureStatus(const nsACString& aDeclaration,
                                  StealthGfxFeatureTable& aTable,
                                  nsACString& aError);

// The answer for one feature from a parsed table. On builds where the only
// WebGL path is native GL (GTK), WEBGL_OPENGL answers the declared WEBGL
// entry, whatever the host's own GL is (or is not): the GL that serves WebGL
// ships with the engine. On Windows that path is ANGLE and WEBGL_OPENGL gates
// a path retail never takes. False for an id outside the live set.
bool StealthAnswerGfxFeature(const StealthGfxFeatureTable& aTable,
                             int32_t aFeature, nsACString& aFailureId,
                             int32_t* aStatus);

// The hook in GfxInfoBase::GetFeatureStatus. False when the engine is off
// (stock Firefox) or the id is outside the live set: Mozilla's path runs.
// Content and GPU processes answer from the parent's feature cache, which the
// parent built from the declaration; every other process parses the pref once
// and refuses to run when it is missing or malformed.
bool StealthGfxDeclaredStatus(nsIGfxInfo* aGfxInfo, int32_t aFeature,
                              nsACString& aFailureId, int32_t* aStatus);

// True when the declaration decides: the downloadable blocklist is not
// evaluated and writes no gfx.blacklist.* pref into the profile.
bool StealthGfxDeclarationActive();

// The suggested driver version (about:support): empty while declared. True
// when it answered.
bool StealthGfxSuggestedDriverVersion(nsAString& aVersion);

// Tests only: forget the parsed declaration so the next call parses again.
void StealthResetGfxFeatureStatusForTesting();

}  // namespace mozilla::gfx

#endif  // GFX_STEALTH_GFX_FEATURE_STATUS_H_
