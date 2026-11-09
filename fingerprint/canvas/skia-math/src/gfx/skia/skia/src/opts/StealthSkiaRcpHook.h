/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Hook of SkRasterPipeline_opts.h (STEALTH-HOOK(fingerprint/canvas/skia-math)), included
// once after the system includes. It puts a function of ours in place of the four x86
// reciprocal intrinsics that Skia's rcp_approx and rsqrt_approx are written with, so that
// the lines of Skia stay as they are:
//
//   - the engine off: the wrapper calls the intrinsic itself, the instruction of the host;
//   - the engine on: the wrapper computes the model of RCPPS/RSQRTPS of the declared
//     vendor (StealthSkiaMath.h), whatever CPU this is.
//
// The wrappers are defined BEFORE the macros, so that they call the real intrinsics. The
// macros stay defined to the end of the translation unit: nothing after
// SkRasterPipeline_opts.h in SkOpts.cpp or SkOpts_ml3.cpp uses these four.
//
// Only the blocks Mozilla builds on x86 are covered: SSE2/SSE4.1/AVX (128 bits) and AVX2
// (256 bits). Skia's AVX-512 block uses _mm512_rcp14_ps, which Mozilla does not build.

#ifndef STEALTH_SKIA_RCP_HOOK_H_
#define STEALTH_SKIA_RCP_HOOK_H_

#include "src/opts/StealthSkiaMath.h"

#if defined(SKRP_CPU_SSE2) || defined(SKRP_CPU_SSE41) || defined(SKRP_CPU_AVX)

static inline __m128 stealth_mm_rcp_ps(__m128 aV) {
  return StealthSkiaMath::On() ? StealthSkiaMath::RcpLanes(aV) : _mm_rcp_ps(aV);
}
static inline __m128 stealth_mm_rsqrt_ps(__m128 aV) {
  return StealthSkiaMath::On() ? StealthSkiaMath::RsqLanes(aV) : _mm_rsqrt_ps(aV);
}
#  define _mm_rcp_ps stealth_mm_rcp_ps
#  define _mm_rsqrt_ps stealth_mm_rsqrt_ps

#elif defined(SKRP_CPU_AVX2)

static inline __m256 stealth_mm256_rcp_ps(__m256 aV) {
  return StealthSkiaMath::On() ? StealthSkiaMath::RcpLanes(aV) : _mm256_rcp_ps(aV);
}
static inline __m256 stealth_mm256_rsqrt_ps(__m256 aV) {
  return StealthSkiaMath::On() ? StealthSkiaMath::RsqLanes(aV) : _mm256_rsqrt_ps(aV);
}
#  define _mm256_rcp_ps stealth_mm256_rcp_ps
#  define _mm256_rsqrt_ps stealth_mm256_rsqrt_ps

#endif

#endif  // STEALTH_SKIA_RCP_HOOK_H_
