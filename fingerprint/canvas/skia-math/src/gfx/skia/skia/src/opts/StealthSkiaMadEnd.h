/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The other half of StealthSkiaMadBegin.h, included inside namespace SK_OPTS_NS after the
// stock mad, nmad, rcp_precise and rsqrt_approx of the SSE2/SSE4.1/AVX block, where F and
// SI are those of the block.
//
//   - the engine off: the stock functions (a+f*m, a-f*m, the Newton step);
//   - the engine on: what the AVX2 block of Skia computes, on any CPU: mad and nmad are an
//     exact fused multiply-add (StealthSkiaMath::Fma per lane), and rcp_precise is the AVX2
//     one, a fused step from the estimate (which is the model, through the wrappers of
//     StealthSkiaRcpHook.h).
// Include guards are intentionally omitted.

#undef mad
#undef nmad
#undef rcp_precise

SI F mad(F f, F m, F a) {
  return StealthSkiaMath::On() ? StealthSkiaMath::FmaLanes(f, m, a) : stealth_stock_mad(f, m, a);
}
SI F nmad(F f, F m, F a) {
  return StealthSkiaMath::On() ? StealthSkiaMath::FmaLanes(-f, m, a) : stealth_stock_nmad(f, m, a);
}
SI F rcp_precise(F v) {
  if (!StealthSkiaMath::On()) {
    return stealth_stock_rcp_precise(v);
  }
  // As the AVX2 block: fnmadd(v, e, 2) * e.
  F e = rcp_approx(v);
  F two = F() + 2.0f;
  return StealthSkiaMath::FmaLanes(-v, e, two) * e;
}
