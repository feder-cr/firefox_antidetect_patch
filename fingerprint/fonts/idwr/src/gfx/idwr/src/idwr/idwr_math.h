/*
 * idwr_math: the C library math that Firefox's text path calls around idwr's glyph masks (and
 * the sin/cos of the rotations that reach glyph transforms),
 * reproduced bit for bit from the Windows CRT (ucrtbase.dll 10.0.26100.9711, x64, the FMA3
 * code path every Windows 11 CPU takes). Portable C99, no libm: the same bits on Windows,
 * Linux and macOS, x86-64 and aarch64.
 *
 * Call sites, domains and the proof: notes/math.md. Checks: reimpl/math_check.py.
 *
 * Build like the rest of idwr: -ffp-contract=off (fp_exact.h also forbids contraction), no
 * -ffast-math, SSE2 or aarch64 floating point (not x87).
 */
#ifndef IDWR_MATH_H
#define IDWR_MATH_H

#ifndef IDWR_API
#if defined(IDWR_STATIC)
#define IDWR_API
#elif defined(_WIN32)
#define IDWR_API __declspec(dllexport)
#else
#define IDWR_API __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ucrtbase powf: WebRender GammaLut (gamma_lut.rs), Skia SkMaskGamma (SkScalarPow). */
IDWR_API float idwr_powf(float x, float y);

/* ucrtbase tanf: WebRender SyntheticItalics::to_skew (webrender_api/src/font.rs). */
IDWR_API float idwr_tanf(float x);

/* ucrtbase tan: Gecko gfxFont::SkewForSyntheticOblique (custom oblique angles). */
IDWR_API double idwr_tan(double x);

/* ucrtbase sinf, cosf: gfx::Matrix::Rotation / PreRotate (Matrix.cpp), from COLRv1 PaintRotate*
   (COLRFonts.cpp) and canvas rotate(). */
IDWR_API float idwr_sinf(float x);
IDWR_API float idwr_cosf(float x);

/* ucrtbase sin, cos: Matrix4x4 RotateX/Y/Z and SetRotateAxisAngle (Matrix.h), from CSS transforms
   (rotate*, StyleAngle::ToRadians); MatrixDouble::Rotation; SafeTangent (CSS skew). */
IDWR_API double idwr_sin(double x);
IDWR_API double idwr_cos(double x);

/* ucrtbase _hypot (hypot jumps to it): WebRender FontTransform::compute_scale. */
IDWR_API double idwr_hypot(double x, double y);

/* fma(a, b, c) rounded once, round-to-nearest-even, in software (used by the ports above;
   exported for the checks). */
IDWR_API double idwr_fma(double a, double b, double c);

#ifdef __cplusplus
}
#endif
#endif
