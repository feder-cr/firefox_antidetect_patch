/* Adobe fixed-point math leaves of the CFF rasterizer (notes/cff.md, group "math"):
 *
 *   fixmul   0x1800b3340  16.16 x 16.16 -> 16.16   (29 callers)
 *   fixdiv   0x1800b316c  16.16 / 16.16 -> 16.16   (14 callers)
 *   fracmul  0x1800b34d8  x 2.30         -> same   (5 callers)
 *   fixratio 0x1800b3544  a / b          -> 2.30   (3 callers)
 *
 * DWrite's ATM computes these in double precision, not with 64-bit integers: both operands
 * are converted exactly (cvtdq2pd), multiplied or divided once (mulsd/divsd), scaled by a
 * power of two (exact), then rounded half away from zero by adding +-0.5 and truncating
 * (cvttsd2si). The result saturates to 0x7fffffff / 0x80000000. Constants (.rdata):
 * 0x1d7e08 65536.0, 0x1d7e18 2^-16, 0x1d7e38 2^-30, 0x1d7e40 2^30, 0x1d7e30 0.5,
 * 0x1d86b8 -0.5, 0x1d7e28 2147483647.0, 0x1d7e10 -2147483648.0.
 *
 * The sign test differs: fixmul branches on x < 0.0 (comisd x, 0; jb), the others on
 * 0.0 > x (comisd 0, x; ja). Both take the -0.5 path for negative x only, and x is never NaN.
 * Division by zero (fixdiv, fixratio) returns 0x7fffffff for a >= 0 and 0x80000000 for a < 0.
 *
 * Negative control: -DCFF_CONTROL_FIXMUL rounds negative products half up (+0.5) instead
 * of half away from zero. */
#include "cff_port.h"

#ifdef IDWR_STANDALONE
#define cff_fixmul_impl cff_fixmul
#define cff_fixdiv_impl cff_fixdiv
#define cff_fracmul_impl cff_fracmul
#define cff_fixratio_impl cff_fixratio
#endif

/* the shared tail of fixdiv, fracmul and fixratio: x += +-0.5, then saturate and truncate */
static int32_t round_sat(double x) {
    x += (0.0 > x) ? -0.5 : 0.5;
    if (x >= 2147483647.0)
        return 0x7fffffff;
    if (-2147483648.0 >= x)
        return (int32_t)0x80000000;
    return (int32_t)x;
}

CFF_API int32_t cff_fixmul_impl(int32_t a, int32_t b) {
    double x = (double)a * (double)b * (1.0 / 65536.0);
    if (x < 0.0) {
#ifdef CFF_CONTROL_FIXMUL
        x += 0.5;
#else
        x += -0.5;
#endif
        return x > -2147483648.0 ? (int32_t)x : (int32_t)0x80000000;
    }
    x += 0.5;
    return 2147483647.0 > x ? (int32_t)x : 0x7fffffff;
}
CFF_EXPORT(cff_fixmul, cff_fixmul_impl);

CFF_API int32_t cff_fixdiv_impl(int32_t a, int32_t b) {
    if (b == 0)
        return (int32_t)(0x7fffffffu + ((uint32_t)a >> 31));
    return round_sat((double)a / (double)b * 65536.0);
}
CFF_EXPORT(cff_fixdiv, cff_fixdiv_impl);

CFF_API int32_t cff_fracmul_impl(int32_t a, int32_t b) {
    return round_sat((double)a * (double)b * (1.0 / 1073741824.0));
}
CFF_EXPORT(cff_fracmul, cff_fracmul_impl);

CFF_API int32_t cff_fixratio_impl(int32_t a, int32_t b) {
    if (b == 0)
        return (int32_t)(0x7fffffffu + ((uint32_t)a >> 31));
    return round_sat((double)a / (double)b * 1073741824.0);
}
CFF_EXPORT(cff_fixratio, cff_fixratio_impl);
