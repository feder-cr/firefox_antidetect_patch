/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Hook of SkRasterPipeline_opts.h (STEALTH-HOOK(fingerprint/canvas/skia-math)): included
// before the `#if defined(SKRP_CPU_SCALAR) || defined(SKRP_CPU_SSE2)` that defines
// rcp_fast and rsqrt, and followed by StealthSkiaRcpFastEnd.h after its #endif. The two
// are renamed for the block and defined again by the End file.
// Include guards are intentionally omitted.

#define rcp_fast stealth_stock_rcp_fast
#define rsqrt stealth_stock_rsqrt
