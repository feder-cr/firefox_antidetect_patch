/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The other half of StealthSkiaRcpFastBegin.h, inside namespace SK_OPTS_NS.
//
// In the SSE2 block Skia's rcp_fast and rsqrt are Newton steps from the estimate ("we
// always use precise math"), where the AVX2 block uses the raw estimate. With the engine
// on the SSE2 block uses the estimate too (of the declared vendor's model: rcp_approx is
// the wrapper of StealthSkiaRcpHook.h); with it off the stock functions. Every other
// block keeps its stock definition: the SSE4.1, AVX and AVX2 blocks already are the
// estimate, and the wrapper makes it the model with the engine on.
// Include guards are intentionally omitted.

#undef rcp_fast
#undef rsqrt

#if defined(SKRP_CPU_SSE2)
SI F rcp_fast(F v) {
  return StealthSkiaMath::On() ? rcp_approx(v) : stealth_stock_rcp_fast(v);
}
SI F rsqrt(F v) {
  return StealthSkiaMath::On() ? rsqrt_approx(v) : stealth_stock_rsqrt(v);
}
#else
SI F rcp_fast(F v) { return stealth_stock_rcp_fast(v); }
SI F rsqrt(F v) { return stealth_stock_rsqrt(v); }
#endif
