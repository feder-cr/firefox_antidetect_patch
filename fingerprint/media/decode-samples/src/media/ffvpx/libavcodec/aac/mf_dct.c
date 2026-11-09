#include <math.h>
#include <string.h>
#include <stdint.h>
#include "mf_dct.h"

static float MF_F(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* fft_16 @0x18000e940, transcribed from the decompilation (straight-line float code) */
void mf_fft16(float *x)
{
    float fStackX_1c, fVar10, fVar11, fVar12, fVar13, fVar14, fVar15, fVar16, fVar17, fVar18, fVar19, fVar2, fVar20, fVar21, fVar22, fVar23, fVar24, fVar25, fVar26, fVar27, fVar28, fVar29, fVar3, fVar30, fVar31, fVar32, fVar33, fVar34, fVar35, fVar36, fVar37, fVar38, fVar39, fVar4, fVar40, fVar41, fVar42, fVar43, fVar44, fVar5, fVar6, fVar7, fVar8, fVar9, local_res18;
  fVar8 = x[16];
  fVar9 = x[24];
  fVar7 = x[17];
  fVar20 = x[25];
  fVar25 = fVar7 + x[1];
  fVar19 = fVar20 + x[9];
  fVar13 = x[20];
  fVar14 = x[18];
  fVar30 = x[23];
  fVar12 = x[22];
  fVar38 = fVar9 + x[8];
  fVar40 = fVar8 + x[0];
  fVar44 = fVar13 + x[4];
  fVar39 = fVar38 - (fVar9 + fVar9);
  fVar2 = fVar14 + x[2];
  fVar41 = fVar40 - (fVar8 + fVar8);
  fVar26 = fVar25 - (fVar7 + fVar7);
  fVar20 = fVar19 - (fVar20 + fVar20);
  fVar8 = x[28];
  fVar15 = fVar8 + x[12];
  fVar9 = x[21];
  fVar7 = x[19];
  fVar43 = fVar9 + x[5];
  fVar3 = fVar7 + x[3];
  fVar4 = fVar20 + fVar41;
  fVar41 = fVar41 - fVar20;
  fVar5 = fVar26 - fVar39;
  fVar39 = fVar39 + fVar26;
  fVar26 = fVar12 + x[6];
  fVar6 = fVar30 + x[7];
  fVar20 = x[29];
  fVar10 = fVar20 + x[13];
  fVar11 = fVar10 - (fVar20 + fVar20);
  fVar20 = x[27];
  fVar21 = fVar44 - (fVar13 + fVar13);
  fVar16 = fVar15 - (fVar8 + fVar8);
  fVar8 = x[26];
  fVar32 = fVar8 + x[10];
  fVar13 = x[31];
  fVar27 = fVar13 + x[15];
  fVar22 = fVar43 - (fVar9 + fVar9);
  fVar17 = fVar20 + x[11];
  fVar9 = x[30];
  fVar31 = fVar9 + x[14];
  fVar18 = fVar17 - (fVar20 + fVar20);
  fVar33 = fVar3 - (fVar7 + fVar7);
  fVar28 = fVar2 - (fVar14 + fVar14);
  fVar7 = fVar18 + fVar28;
  fVar29 = fVar32 - (fVar8 + fVar8);
  fVar20 = fVar33 - fVar29;
  fVar35 = fVar26 - (fVar12 + fVar12);
  fVar36 = fVar27 - (fVar13 + fVar13);
  fVar34 = fVar6 - (fVar30 + fVar30);
  fVar37 = fVar31 - (fVar9 + fVar9);
  fVar23 = (fVar19 + fVar25) - (fVar10 + fVar43);
  fVar12 = (fVar19 + fVar25) + (fVar10 + fVar43);
  fVar28 = fVar28 - fVar18;
  fVar29 = fVar29 + fVar33;
  fVar24 = (fVar40 - fVar38) - (fVar43 - fVar10);
  fVar10 = (fVar40 - fVar38) + (fVar43 - fVar10);
  fVar33 = (fVar38 + fVar40) - (fVar15 + fVar44);
  fVar42 = (fVar38 + fVar40) + (fVar15 + fVar44);
  fVar18 = (fVar25 - fVar19) + (fVar44 - fVar15);
  fVar19 = (fVar25 - fVar19) - (fVar44 - fVar15);
  fVar15 = (fVar17 + fVar3) - (fVar27 + fVar6);
  fVar40 = (fVar17 + fVar3) + (fVar27 + fVar6);
  fVar38 = (fVar32 + fVar2) - (fVar31 + fVar26);
  fVar25 = (fVar32 + fVar2) + (fVar31 + fVar26);
  fVar13 = (fVar22 - fVar16) * MF_F(0x3f3504f3);
  fVar8 = (fVar11 + fVar21) * MF_F(0x3f3504f3);
  fVar14 = fVar13 + fVar8;
  fVar13 = fVar13 - fVar8;
  fVar9 = fVar4 - fVar14;
  fVar4 = fVar4 + fVar14;
  fVar43 = fVar5 + fVar13;
  fVar5 = fVar5 - fVar13;
  fVar30 = (fVar16 + fVar22) * MF_F(0x3f3504f3);
  fVar13 = (fVar21 - fVar11) * MF_F(0x3f3504f3);
  fVar8 = fVar30 - fVar13;
  fVar30 = fVar30 + fVar13;
  fVar16 = fVar41 - fVar8;
  fVar41 = fVar41 + fVar8;
  fVar14 = fVar39 - fVar30;
  fVar39 = fVar39 + fVar30;
  fVar8 = (fVar36 + fVar35) * MF_F(0x3f3504f3);
  fVar11 = (fVar34 - fVar37) * MF_F(0x3f3504f3);
  fVar21 = (fVar37 + fVar34) * MF_F(0x3f3504f3);
  fVar30 = fVar11 - fVar8;
  fVar11 = fVar11 + fVar8;
  fVar13 = fVar20 - fVar30;
  fVar20 = fVar20 + fVar30;
  fVar22 = fVar7 - fVar11;
  fVar7 = fVar7 + fVar11;
  fVar30 = (fVar35 - fVar36) * MF_F(0x3f3504f3);
  fVar8 = fVar21 - fVar30;
  fVar21 = fVar21 + fVar30;
  fVar34 = fVar28 - fVar8;
  fVar30 = fVar29 + fVar21;
  fVar29 = fVar29 - fVar21;
  fVar28 = fVar28 + fVar8;
  x[16] = fVar42 - fVar25;
  x[0] = fVar42 + fVar25;
  x[17] = fVar12 - fVar40;
  x[1] = fVar12 + fVar40;
  x[8] = fVar33 + fVar15;
  x[24] = fVar33 - fVar15;
  x[9] = fVar23 - fVar38;
  x[25] = fVar23 + fVar38;
  fVar8 = MF_F(0x3f3504f3);
  local_res18 = MF_F(0x3f6c835e);
  fStackX_1c = MF_F(0x3ec3ef15);
  fVar12 = local_res18 * fVar20 - fStackX_1c * fVar7;
  fVar15 = ((fVar3 - fVar17) - (fVar26 - fVar31)) * MF_F(0x3f3504f3);
  fVar7 = fStackX_1c * fVar20 + local_res18 * fVar7;
  fVar11 = ((fVar2 - fVar32) + (fVar6 - fVar27)) * MF_F(0x3f3504f3);
  x[19] = fVar43 - fVar12;
  x[3] = fVar43 + fVar12;
  x[2] = fVar4 + fVar7;
  fVar12 = fStackX_1c * fVar13 + local_res18 * fVar22;
  x[18] = fVar4 - fVar7;
  fVar7 = local_res18 * fVar13 - fStackX_1c * fVar22;
  fVar20 = fVar15 - fVar11;
  fVar15 = fVar15 + fVar11;
  x[10] = fVar9 + fVar7;
  x[26] = fVar9 - fVar7;
  x[27] = fVar5 + fVar12;
  x[11] = fVar5 - fVar12;
  x[4] = fVar10 + fVar15;
  x[20] = fVar10 - fVar15;
  fVar7 = ((fVar3 - fVar17) + (fVar26 - fVar31)) * fVar8;
  x[21] = fVar19 - fVar20;
  fVar8 = ((fVar2 - fVar32) - (fVar6 - fVar27)) * fVar8;
  x[5] = fVar19 + fVar20;
  fVar9 = fVar7 - fVar8;
  fVar7 = fVar7 + fVar8;
  x[12] = fVar24 + fVar9;
  x[28] = fVar24 - fVar9;
  x[13] = fVar18 - fVar7;
  
  x[29] = fVar18 + fVar7;
  local_res18 = MF_F(0x3ec3ef15);
  fStackX_1c = MF_F(0x3f6c835e);
  fVar8 = fStackX_1c * fVar29 + local_res18 * fVar28;
  fVar9 = local_res18 * fVar29 - fStackX_1c * fVar28;
  x[6] = fVar41 + fVar8;
  x[22] = fVar41 - fVar8;
  x[23] = fVar14 - fVar9;
  x[7] = fVar14 + fVar9;
  fVar9 = fStackX_1c * fVar30 + local_res18 * fVar34;
  fVar8 = local_res18 * fVar30 - fStackX_1c * fVar34;
  x[14] = fVar16 + fVar8;
  x[30] = fVar16 - fVar8;
  x[15] = fVar39 - fVar9;
  x[31] = fVar39 + fVar9;
}


/* bit reversal of n complex values (scramble @0x18000f7d0) */
void mf_scramble(float *x, int n)
{
    int m, j = 0, k;
    for (m = 1; m < n - 1; m++) {
        for (k = n >> 1; !((j ^= k) & k); k >>= 1)
            ;
        if (j > m) {
            float t;
            t = x[2 * m];     x[2 * m]     = x[2 * j];     x[2 * j]     = t;
            t = x[2 * m + 1]; x[2 * m + 1] = x[2 * j + 1]; x[2 * j + 1] = t;
        }
    }
}

/* FDK cplxMult: re = a_re*w.re - a_im*w.im, im = a_re*w.im + a_im*w.re */
#define CMUL(cre, cim, are, aim, w) do { \
    cre = (are) * (w).re - (aim) * (w).im; \
    cim = (are) * (w).im + (aim) * (w).re; } while (0)

/* four-lane helpers for the SSE code of dit_fft_fl_SSE2 (portable, same rounding and same sign of zero) */
typedef struct { float f[4]; } v4;
static v4 v_ld(const float *p) { v4 r; r.f[0] = p[0]; r.f[1] = p[1]; r.f[2] = p[2]; r.f[3] = p[3]; return r; }
static void v_st(float *p, v4 a) { p[0] = a.f[0]; p[1] = a.f[1]; p[2] = a.f[2]; p[3] = a.f[3]; }
static v4 v_add(v4 a, v4 b) { v4 r; int i; for (i = 0; i < 4; i++) r.f[i] = a.f[i] + b.f[i]; return r; }
static v4 v_sub(v4 a, v4 b) { v4 r; int i; for (i = 0; i < 4; i++) r.f[i] = a.f[i] - b.f[i]; return r; }
static v4 v_mul(v4 a, v4 b) { v4 r; int i; for (i = 0; i < 4; i++) r.f[i] = a.f[i] * b.f[i]; return r; }
static v4 v_shuf(v4 a, v4 b, int m) { v4 r; r.f[0] = a.f[m & 3]; r.f[1] = a.f[(m >> 2) & 3]; r.f[2] = b.f[(m >> 4) & 3]; r.f[3] = b.f[(m >> 6) & 3]; return r; }
static v4 v_unpcklo(v4 a, v4 b) { v4 r; r.f[0] = a.f[0]; r.f[1] = b.f[0]; r.f[2] = a.f[1]; r.f[3] = b.f[1]; return r; }
static v4 v_unpckhi(v4 a, v4 b) { v4 r; r.f[0] = a.f[2]; r.f[1] = b.f[2]; r.f[2] = a.f[3]; r.f[3] = b.f[3]; return r; }

/* dit_fft_fl_SSE2 @0x180010500, first block @0x180010571..0x180010645: eight complex values per pass, the first three
 * stages fused. Constants (@0x18009f960, 0x18009f940, 0x18009f950) are the sign vector and the radix-8 twiddles. */
static void fft_first_block(float *x, int n)
{
    static const v4 K8 = { { 1.0f, 1.0f, 1.0f, -1.0f } };
    static const v4 K9 = { { 1.0f, 0.70710677f, 0.0f, -0.70710677f } };
    static const v4 K10 = { { 0.0f, 0.70710677f, 1.0f, 0.70710677f } };
    int i;
    for (i = 0; i < n * 2; i += 16) {
        v4 x0 = v_ld(x + i + 4), x3 = v_ld(x + i + 12), x1 = v_ld(x + i), x4 = v_ld(x + i + 8), x2, x5, x6, x7;
        x2 = x1;
        x1 = v_shuf(x1, x0, 0xee);
        x2 = v_shuf(x2, x0, 0x44);
        x0 = v_add(x1, x2);
        x2 = v_sub(x2, x1);
        x5 = x0;
        x0 = v_shuf(x0, x2, 0xbe);
        x0 = v_mul(x0, K8);
        x5 = v_shuf(x5, x2, 0x44);
        x7 = v_add(x0, x5);
        x5 = v_sub(x5, x0);
        x0 = x4;
        x0 = v_shuf(x0, x3, 0x44);
        x4 = v_shuf(x4, x3, 0xee);
        x1 = v_add(x4, x0);
        x6 = x7;
        x0 = v_sub(x0, x4);
        x6 = v_shuf(x6, x5, 0x88);
        x7 = v_shuf(x7, x5, 0xdd);
        x2 = x1;
        x1 = v_shuf(x1, x0, 0xbe);
        x2 = v_shuf(x2, x0, 0x44);
        x1 = v_mul(x1, K8);
        x4 = v_add(x1, x2);
        x2 = v_sub(x2, x1);
        x1 = x4;
        x1 = v_shuf(x1, x2, 0x88);
        x4 = v_shuf(x4, x2, 0xdd);
        x0 = v_mul(x1, K10);
        x3 = x4;
        x1 = v_mul(x1, K9);
        x4 = v_mul(x4, K10);
        x3 = v_mul(x3, K9);
        x4 = v_add(x4, x1);
        x3 = v_sub(x3, x0);
        x2 = v_add(x4, x6);
        x6 = v_sub(x6, x4);
        x1 = v_add(x3, x7);
        x7 = v_sub(x7, x3);
        x0 = v_unpcklo(x2, x1);
        v_st(x + i, x0);
        x0 = x6;
        x2 = v_unpckhi(x2, x1);
        x0 = v_unpcklo(x0, x7);
        x6 = v_unpckhi(x6, x7);
        v_st(x + i + 8, x0);
        v_st(x + i + 4, x2);
        v_st(x + i + 12, x6);
    }
}

/* FDK dit_fft: radix-2 DIT, float, no scaling; the first block (3 stages) as the SSE code does it */
void mf_fft_rad2(float *x, int ldn, const mf_spk *trig, int trig_size)
{
    const int n = 1 << ldn;
    int ldm;
    fft_first_block(x, n);
    for (ldm = 4; ldm <= ldn; ldm++) {
        const int m = 1 << ldm, mh = m >> 1;
        const int trigstep = (trig_size << 2) >> ldm;
        int j, r;
        for (r = 0; r < n; r += m) {
            int t1 = r << 1, t2 = t1 + (mh << 1);
            float vr, vi, ur, ui;
            vi = x[t2 + 1]; vr = x[t2]; ur = x[t1]; ui = x[t1 + 1];
            x[t1] = ur + vr; x[t1 + 1] = ui + vi; x[t2] = ur - vr; x[t2 + 1] = ui - vi;
            t1 += mh; t2 = t1 + (mh << 1);
            vr = x[t2 + 1]; vi = x[t2]; ur = x[t1]; ui = x[t1 + 1];
            x[t1] = ur + vr; x[t1 + 1] = ui - vi; x[t2] = ur - vr; x[t2 + 1] = ui + vi;
        }
        for (j = 1; j < mh / 4; j++) {
            const mf_spk cs = trig[j * trigstep];
            for (r = 0; r < n; r += m) {
                int t1 = (r + j) << 1, t2 = t1 + (mh << 1);
                float vr, vi, ur, ui;
                CMUL(vi, vr, x[t2 + 1], x[t2], cs);
                ur = x[t1]; ui = x[t1 + 1];
                x[t1] = ur + vr; x[t1 + 1] = ui + vi; x[t2] = ur - vr; x[t2 + 1] = ui - vi;
                t1 += mh; t2 = t1 + (mh << 1);
                CMUL(vr, vi, x[t2 + 1], x[t2], cs);
                ur = x[t1]; ui = x[t1 + 1];
                x[t1] = ur + vr; x[t1 + 1] = ui - vi; x[t2] = ur - vr; x[t2 + 1] = ui + vi;
                t1 = (r + mh / 2 - j) << 1; t2 = t1 + (mh << 1);
                CMUL(vi, vr, x[t2], x[t2 + 1], cs);
                ur = x[t1]; ui = x[t1 + 1];
                x[t1] = ur + vr; x[t1 + 1] = ui - vi; x[t2] = ur - vr; x[t2 + 1] = ui + vi;
                t1 += mh; t2 = t1 + (mh << 1);
                CMUL(vr, vi, x[t2], x[t2 + 1], cs);
                ur = x[t1]; ui = x[t1 + 1];
                x[t1] = ur - vr; x[t1 + 1] = ui - vi; x[t2] = ur + vr; x[t2 + 1] = ui + vi;
            }
        }
        {
            const mf_spk cs = { 0.70710677f, 0.70710677f };
            j = mh / 4;
            for (r = 0; r < n; r += m) {
                int t1 = (r + j) << 1, t2 = t1 + (mh << 1);
                float vr, vi, ur, ui;
                CMUL(vi, vr, x[t2 + 1], x[t2], cs);
                ur = x[t1]; ui = x[t1 + 1];
                x[t1] = ur + vr; x[t1 + 1] = ui + vi; x[t2] = ur - vr; x[t2 + 1] = ui - vi;
                t1 += mh; t2 = t1 + (mh << 1);
                CMUL(vr, vi, x[t2 + 1], x[t2], cs);
                ur = x[t1]; ui = x[t1 + 1];
                x[t1] = ur + vr; x[t1 + 1] = ui - vi; x[t2] = ur - vr; x[t2 + 1] = ui + vi;
            }
        }
    }
}

/* dct_IV_func1_FLOAT @0x18000f2e0 and dct_IV_func1_AVX2_FMA @0x18000e740 */
static void pre_twiddle(float *x, int N, const mf_spk *tw, int avx)
{
    const int M = N >> 1;
    float *p0 = x, *p1 = x + N - 2;
    int i;
    for (i = 0; i < M - 1; i += 2, p0 += 2, p1 -= 2) {
        const mf_spk a = tw[i], b = tw[i + 1];
        const float xb = p1[1], xf = p0[0], xf2 = p0[1], xb2 = p1[0];
        if (avx) { /* vmulps on the forward value, vfmadd231ps/vfmsub231ps on the backward value */
            p0[0] = fmaf(a.im, xb, a.re * xf);
            p0[1] = fmaf(a.re, xb, -(a.im * xf));
            p1[0] = fmaf(b.im, xb2, b.re * xf2);
            p1[1] = -fmaf(b.re, xb2, -(b.im * xf2));
        } else {
            p0[0] = a.im * xb + a.re * xf;
            p0[1] = a.re * xb - a.im * xf;
            p1[0] = b.im * xb2 + b.re * xf2;
            p1[1] = b.im * xf2 - b.re * xb2;
        }
    }
}

/* dct_IV_func2_FLOAT @0x18000e530, post-twiddle of dct_IV. SSE code, N/8 iterations of 4 head + 4 tail values and two
 * twiddles; written lane by lane. The sign constant is {1,-1,1,-1}; products by it and the lane pairing decide the sign of
 * zero (a scalar version gave +0 for -0 at outputs 0 and N-1). The values of the first lanes use the previous
 * iteration's second twiddle (p5 = B.re, p6 = B.im; before the first: p5 = 1, p6 = 0). */
static void post_twiddle(float *x, int N, const mf_spk *sintab, int step)
{
    static const float K[4] = { 1.0f, -1.0f, 1.0f, -1.0f };
    float *hd = x, *tl = x + N - 4;
    float p5 = 1.0f, p6 = 0.0f;
    int it, idx = step;
    for (it = 0; it < N / 8; it++, idx += 2 * step, hd += 4, tl -= 4) {
        const mf_spk A = sintab[idx], B = sintab[idx + step];
        const float h[4] = { hd[0], hd[1], hd[2], hd[3] }, t[4] = { tl[0], tl[1], tl[2], tl[3] };
        float m[4], x6[4], n[4], X[4];
        m[0] = (p5 * h[0]) * K[0]; m[1] = (p5 * h[1]) * K[1]; m[2] = (A.re * h[2]) * K[2]; m[3] = (A.re * h[3]) * K[3];
        x6[0] = p6 * h[0] + m[1]; x6[1] = p6 * h[1] + m[0]; x6[2] = A.im * h[2] + m[3]; x6[3] = A.im * h[3] + m[2];
        n[0] = (B.im * t[0]) * K[0]; n[1] = (B.im * t[1]) * K[1]; n[2] = (A.im * t[2]) * K[2]; n[3] = (A.im * t[3]) * K[3];
        X[0] = n[1] + B.re * t[0]; X[1] = n[0] + B.re * t[1]; X[2] = n[3] + A.re * t[2]; X[3] = n[2] + A.re * t[3];
        hd[0] = x6[1]; hd[1] = X[2]; hd[2] = x6[3]; hd[3] = X[0];
        tl[0] = X[1]; tl[1] = x6[2]; tl[2] = X[3]; tl[3] = x6[0];
        p5 = B.re; p6 = B.im;
    }
}

static int ld(int N) { int l = 0; while ((1 << l) < N) l++; return l; }

/* FFT of M = N/2 complex values as dct_IV/dst_IV choose it */
static void fft_m(float *x, int M, const mf_dct_tables *t)
{
    if (M == 16) { mf_fft16(x); return; }
    mf_scramble(x, M);
    mf_fft_rad2(x, ld(M), t->sine_table_512, 512);
}

void mf_dct_iv(float *x, int N, int avx, const mf_dct_tables *t)
{
    const int M = N >> 1, l = ld(N), step = 1 << (11 - l);
    pre_twiddle(x, N, t->pre_twiddle[l], avx && M >= 8 && (M & 7) == 0);
    fft_m(x, M, t);
    post_twiddle(x, N, t->sine_table_1024, step);
}

/* dst_IV: dst_IV_func1_FLOAT @0x180036270 / dst_IV_func1_AVX2_FMA @0x18000e840, post dst_IV_func2_FLOAT @0x18000e630 */
static void dst_pre(float *x, int N, const mf_spk *tw, int avx)
{
    const int M = N >> 1;
    float *p0 = x, *p1 = x + N - 2;
    int i;
    for (i = 0; i < M - 1; i += 2, p0 += 2, p1 -= 2) {
        const mf_spk a = tw[i], b = tw[i + 1];
        const float xb = p1[1], xf = -p0[0], xf2 = p0[1], xb2 = -p1[0];
        if (avx) {
            p0[0] = fmaf(a.im, xb, a.re * xf);
            p0[1] = fmaf(a.re, xb, -(a.im * xf));
            p1[0] = fmaf(b.im, xb2, b.re * xf2);
            p1[1] = -fmaf(b.re, xb2, -(b.im * xf2));
        } else {
            p0[0] = a.im * xb + a.re * xf;
            p0[1] = a.re * xb - a.im * xf;
            p1[0] = b.im * xb2 + b.re * xf2;
            p1[1] = b.im * xf2 - b.re * xb2;
        }
    }
}

static void dst_post(float *x, int N, const mf_spk *sintab, int step)
{
    const int M = N >> 1;
    float *p0 = x, *p1 = x + N - 2;
    float a1 = p1[0], a2 = p1[1];
    int i, idx;
    p1[1] = -p0[0];
    p0[0] = p0[1];
    for (idx = step, i = 1; i < (M + 1) >> 1; i++, idx += step) {
        const mf_spk t = sintab[idx];
        float f3, f4;
        p1[0] = -(t.re * a1 - t.im * a2);
        p0[1] = -(t.im * a1 + t.re * a2);
        p0 += 2; p1 -= 2;
        a1 = p1[0]; a2 = p1[1];
        f3 = p0[0]; f4 = p0[1];
        p0[0] = t.re * f4 - t.im * f3;
        p1[1] = -(t.im * f4 + t.re * f3);
    }
    if ((M & 1) == 0) {
        a1 = a1 * 0.70710677f; a2 = a2 * 0.70710677f;
        p0[1] = -a1 - a2;
        p1[0] = -(a1 - a2);   /* not a2 - a1: the DLL gives -0 for an all-zero input */
    }
}

void mf_dst_iv(float *x, int N, int avx, const mf_dct_tables *t)
{
    const int M = N >> 1, l = ld(N), step = 1 << (11 - l);
    dst_pre(x, N, t->pre_twiddle[l], avx && M >= 8 && (M & 7) == 0);
    fft_m(x, M, t);
    dst_post(x, N, t->sine_table_1024, step);
}
