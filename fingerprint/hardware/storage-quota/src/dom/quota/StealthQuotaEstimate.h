/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_STEALTHQUOTAESTIMATE_H_
#define DOM_QUOTA_STEALTHQUOTAESTIMATE_H_

#include <cstdint>

#include "mozilla/StaticPrefs_zoom.h"

namespace mozilla::dom::quota {

// The quota navigator.storage.estimate() reports: zoom.stealth.storage.quota_mb
// in MB when it is > 0, the limit Mozilla computed otherwise (-1, the default).
// Only the reported value changes: eviction keeps Mozilla's limits.
// Called by EstimateOp::GetResponse (OriginOperations.cpp), where the answer
// to the page is written; see fingerprint/hardware/storage-quota/FEATURE.md.
inline uint64_t StealthEstimateLimit(uint64_t aLimit) {
  const int32_t mb = StaticPrefs::zoom_stealth_storage_quota_mb();
  return mb > 0 ? uint64_t(mb) * 1024 * 1024 : aLimit;
}

}  // namespace mozilla::dom::quota

#endif  // DOM_QUOTA_STEALTHQUOTAESTIMATE_H_
