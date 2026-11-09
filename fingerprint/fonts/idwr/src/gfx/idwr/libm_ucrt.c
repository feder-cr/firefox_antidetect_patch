/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * libxul's own sin, cos, tan, sinf, cosf, tanf, powf, hypot, pow, expf, logf, asinf, acosf,
 * atanf and atan2f, outside Windows.
 *
 * Windows Firefox takes these from ucrtbase.dll; elsewhere the same calls went to the host's
 * libm, whose last bits differ (glibc's tanf is correctly rounded, ucrtbase's is not; see
 * invisible_DWriteCore notes/math.md). Defined here, hidden, every reference inside libxul
 * binds to these at link time: C and C++ (js/src/builtin/Math.cpp's std::sin/cos/tan/pow,
 * gfx/2d's rotations, Skia's SkScalarPow) and Rust (style's calc functions, WebRender's
 * GammaLut and compute_scale), with no edit at any call site. The bodies are idwr_math's,
 * the ucrtbase 10.0.26100.9711 FMA3 code, bit for bit.
 *
 * - The calc() functions of servo/components/style/values/generics/calc.rs run in f32:
 *   f32::sin/cos/tan, asin, acos, atan, atan2, powf, ln (and log, as ln(a) / ln(b)), exp, which
 *   Rust lowers to sinf, cosf, tanf, asinf, acosf, atanf, atan2f, powf, logf and expf. A page
 *   reads them with getComputedStyle (word-spacing, rotate), written to 6 significant digits,
 *   so a last-bit difference in glibc's asinf, acosf or logf changes the text.
 * - Each name here is one idwr_math function proved equal to ucrtbase on every float input
 *   (expf, logf, asinf, acosf, atanf, atan2f(y, 1) and atan2f(1, x): 2^32 each), on random
 *   pairs (atan2f, 2^30; pow, 2^30 double pairs) and on every pair of special values; the
 *   numbers are in invisible_DWriteCore's notes/math.md. pow (double) is std::pow in
 *   js/src/builtin/Math.cpp (Math.pow) and f64::powf in Rust; on Windows both are ucrtbase's.
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
LIBM_UCRT_HIDDEN double pow(double x, double y) { return idwr_pow(x, y); }
LIBM_UCRT_HIDDEN float expf(float x) { return idwr_expf(x); }
LIBM_UCRT_HIDDEN float logf(float x) { return idwr_logf(x); }
LIBM_UCRT_HIDDEN float asinf(float x) { return idwr_asinf(x); }
LIBM_UCRT_HIDDEN float acosf(float x) { return idwr_acosf(x); }
LIBM_UCRT_HIDDEN float atanf(float x) { return idwr_atanf(x); }
LIBM_UCRT_HIDDEN float atan2f(float y, float x) { return idwr_atan2f(y, x); }

LIBM_UCRT_HIDDEN void sincos(double x, double* s, double* c) {
  *s = idwr_sin(x);
  *c = idwr_cos(x);
}

LIBM_UCRT_HIDDEN void sincosf(float x, float* s, float* c) {
  *s = idwr_sinf(x);
  *c = idwr_cosf(x);
}
