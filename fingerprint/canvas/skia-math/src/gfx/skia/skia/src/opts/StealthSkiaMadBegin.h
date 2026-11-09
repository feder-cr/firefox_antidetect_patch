/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Hook of SkRasterPipeline_opts.h (STEALTH-HOOK(fingerprint/canvas/skia-math)): included
// before the lines of the SSE2/SSE4.1/AVX block that define mad, nmad and rcp_precise, and
// followed by StealthSkiaMadEnd.h after them. The three are renamed for these lines (the
// stock definitions, unchanged, keep a name of their own) and defined again by the End
// file, which chooses between them at run time. No line of Skia is rewritten.
// Include guards are intentionally omitted.

#define mad stealth_stock_mad
#define nmad stealth_stock_nmad
#define rcp_precise stealth_stock_rcp_precise
