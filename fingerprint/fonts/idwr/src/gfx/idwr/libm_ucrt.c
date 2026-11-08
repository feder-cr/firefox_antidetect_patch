/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * libxul's own sin, cos, tan, sinf, cosf, tanf, powf and hypot, outside Windows.
 *
 * Windows Firefox takes these from ucrtbase.dll; elsewhere the same calls went to the host's
 * libm, whose last bits differ (glibc's tanf is correctly rounded, ucrtbase's is not; see
 * invisible_DWriteCore notes/math.md). Defined here, hidden, every reference inside libxul
 * binds to these at link time: C and C++ (js/src/builtin/Math.cpp's std::sin/cos/tan,
 * gfx/2d's rotations, Skia's SkScalarPow) and Rust (style's calc functions, WebRender's
 * GammaLut and compute_scale), with no edit at any call site. The bodies are idwr_math's,
 * the ucrtbase 10.0.26100.9711 FMA3 code, bit for bit.
 *
 * - Hidden: the definitions are not exported. libxul's dynamic symbol table keeps no entry
 *   for these names, neither an import from libm nor an export that a page-side probe of
 *   the process could find.
 * - Built with -fno-lto and -fno-builtin (moz.build): a native object, so LTO neither drops
 *   nor internalizes a definition that code generation may still call (a sin and a cos of
 *   the same value become one sincos call after LTO), and the compiler sees plain functions,
 *   not library calls to fold or rewrite.
 * - sincos and sincosf: on GNU targets LLVM merges sin(x) and cos(x) of one argument into a
 *   sincos call; Windows has no sincos and makes two calls. Defined here as those two calls.
 * - Not on Windows: ucrtbase there is the reference, and idwr_math equals it.
 *
 * Checks: gfx/idwr/tests/gtest/TestIdwrLibm.cpp (the values, through libxul) and
 * gfx/idwr/tests/check_libxul_math.py (no dynamic symbol of these names in libxul).
 */

#include "idwr/idwr_math.h"

#if defined(__GNUC__) || defined(__clang__)
#define LIBM_UCRT_HIDDEN __attribute__((visibility("hidden")))
#else
#define LIBM_UCRT_HIDDEN
#endif

LIBM_UCRT_HIDDEN double sin(double x) { return idwr_sin(x); }
LIBM_UCRT_HIDDEN double cos(double x) { return idwr_cos(x); }
LIBM_UCRT_HIDDEN double tan(double x) { return idwr_tan(x); }
LIBM_UCRT_HIDDEN float sinf(float x) { return idwr_sinf(x); }
LIBM_UCRT_HIDDEN float cosf(float x) { return idwr_cosf(x); }
LIBM_UCRT_HIDDEN float tanf(float x) { return idwr_tanf(x); }
LIBM_UCRT_HIDDEN float powf(float x, float y) { return idwr_powf(x, y); }
LIBM_UCRT_HIDDEN double hypot(double x, double y) { return idwr_hypot(x, y); }

LIBM_UCRT_HIDDEN void sincos(double x, double* s, double* c) {
  *s = idwr_sin(x);
  *c = idwr_cos(x);
}

LIBM_UCRT_HIDDEN void sincosf(float x, float* s, float* c) {
  *s = idwr_sinf(x);
  *c = idwr_cosf(x);
}
