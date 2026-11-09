/*
 * Float AAC decoder made equal to Media Foundation (msauddecmft.dll 10.0.26100.9549, Windows 11).
 * Only the float decoder; included by aacdec_float.c. Reference: invisible_MediaFoundation
 * (reimpl/ffmpeg/aacdec_mf.h and notes/), the same logic on FFmpeg 6.1.1.
 *
 * The sign of zero is part of the output here, so the files that use this one are compiled
 * without contraction (moz.build) and never with -fno-signed-zeros.
 *
 * pow/powf here are idwr_pow/idwr_powf (gfx/idwr, moz.build LOCAL_INCLUDES), bit-exact to
 * ucrtbase.dll (invisible_DWriteCore): never the host's libm, which Media Foundation does not
 * call. sqrtf stays libm: IEEE-754 correct rounding makes it exact on every host.
 */

#ifndef AVCODEC_AAC_AACDEC_MF_H
#define AVCODEC_AAC_AACDEC_MF_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "libavutil/common.h"

#include "idwr_math.h"

#include "mf_tables.h"
#include "mf_imlt.h"

static inline float mf_bits2f(uint32_t u)
{
    union { uint32_t u; float f; } v;
    v.u = u;
    return v.f;
}

/* ExponentTableFl: float(2^(k/4)) except 7 entries stored as 7-digit literals */
static float mf_exp_tab[128];

static void mf_init_tables(void)
{
    int k;
    for (k = 0; k < 128; k++)
        mf_exp_tab[k] = mf_bits2f(mf_ExponentTableFl[k]);
}

/* scaleFactor2Exp */
static float mf_sf2exp(int n)
{
    if (n < 0) {
        if (-n < 128)
            return 1.0f / mf_exp_tab[-n];
        return idwr_powf(2.0f, (float)(-n) * -0.25f);
    }
    if (n < 128)
        return mf_exp_tab[n];
    return idwr_powf(2.0f, (float)n * 0.25f);
}

/* x^(4/3) for |q| >= 1024: InverseQuantTableFl and interpolation */
static float mf_iq_big(int u, float scale16)
{
    static const float it[8] = { 0.f, 0.125f, 0.25f, 0.375f, 0.5f, 0.625f, 0.75f, 0.875f };
    float a = (float)idwr_pow((double)(u >> 3), 4.0 / 3.0);
    float b = (float)idwr_pow((double)((u >> 3) + 1), 4.0 / 3.0);
    return ((b - a) * it[u & 7] + a) * scale16;
}

/* one line: sf is FFmpeg's (negative) scale, q the signed integer value */
static float mf_dequant_one(int q, float sf)
{
    int u = q < 0 ? -q : q;
    float v;
    if (u < 1024)
        v = sf * (float)idwr_pow((double)u, 4.0 / 3.0);
    else
        v = mf_iq_big(u, sf * 16.0f);
    return q < 0 ? -v : v;
}

/* TNS: lattice filter on the reflection coefficients */
static void mf_apply_tns(float *coef, TemporalNoiseShaping *tns, IndividualChannelStream *ics)
{
    const int mmm = FFMIN(ics->tns_max_bands, ics->max_sfb);
    int w, filt;
    if (!mmm)
        return;
    for (w = 0; w < ics->num_windows; w++) {
        int bottom = ics->num_swb, top;
        for (filt = 0; filt < tns->n_filt[w]; filt++) {
            int order = tns->order[w][filt], start, end, size, inc, j;
            float c[TNS_MAX_ORDER], st[TNS_MAX_ORDER + 2];
            float *p;
            top = bottom;
            bottom = FFMAX(0, top - tns->length[w][filt]);
            if (!order)
                continue;
            start = ics->swb_offset[FFMIN(bottom, mmm)];
            end   = ics->swb_offset[FFMIN(top, mmm)];
            if ((size = end - start) <= 0)
                continue;
            for (j = 0; j < order; j++)
                c[j] = tns->coef[w][filt][j];
            memset(st, 0, sizeof(st));
            inc = tns->direction[w][filt] ? -1 : 1;
            p = coef + w * 128 + (inc < 0 ? end - 1 : start);
            for (; size > 0; size--, p += inc) {
                float x = *p - st[order] * c[order - 1];
                for (j = order - 1; j >= 1; j--) {
                    x = x - st[j] * c[j - 1];
                    st[j + 1] = c[j - 1] * x + st[j];
                }
                st[1] = x;
                *p = x;
            }
        }
    }
}

/* IMDCT and windowing: CBlock_FrequencyToTime -> imlt_block. The AVX2/FMA path of the DLL is
 * declared, never read from the host (the common Windows machine). */
#define MF_AVX2_FMA 1

static const mf_dct_tables mf_dt = {
    (const mf_spk *)mf_SineTable1024Fl, (const mf_spk *)mf_SineTable512Fl,
    { 0, 0, 0, 0, 0, 0, 0, (const mf_spk *)mf_SineWindow128Fl, 0, 0,
      (const mf_spk *)mf_SineWindow1024Fl }
};

static const mf_spk *mf_slope(int len, int kbd)
{
    if (len == 1024)
        return (const mf_spk *)(kbd ? mf_KBDWindow1024Fl : mf_SineWindow1024Fl);
    return (const mf_spk *)(kbd ? mf_KBDWindow128Fl : mf_SineWindow128Fl);
}

static void mf_imdct_and_windowing(SingleChannelElement *sce)
{
    const IndividualChannelStream *ics = &sce->ics;
    int nspec = 1, tl = 1024, fl = 1024, fr = 1024, i, kb = ics->use_kb_window[0];
    float spec[1024], out[1024];

    if (!sce->mf_init) {
        mf_mdct_init(&sce->mf_mdct);
        sce->mf_init = 1;
    }
    switch (ics->window_sequence[0]) {
    case LONG_START_SEQUENCE:  fr = 128; break;
    case EIGHT_SHORT_SEQUENCE: nspec = 8; tl = fl = fr = 128; break;
    case LONG_STOP_SEQUENCE:   fl = 128; break;
    default: break;
    }
    for (i = 0; i < 1024; i++)
        spec[i] = 0.0f - sce->coeffs[i];   /* FFmpeg sign -> DLL sign, exact */
    mf_imlt_block(&sce->mf_mdct, out, spec, nspec, 1024, tl, mf_slope(fl, kb), fl,
                  mf_slope(fr, kb), fr, MF_AVX2_FMA, &mf_dt);
    for (i = 0; i < 1024; i++)
        sce->output[i] = out[i] * 3.0517578125e-05f;   /* 2^-15, aacDecoder_DecodeFrame */
}

/* PNS: the generator and the gain of CChannelElement_Decode (inlined CPns_Apply). Called after
 * M/S and intensity stereo. */
static void mf_apply_pns(ChannelElement *cpe, SingleChannelElement *sce, int ch)
{
    static const uint32_t tab_bits[4] = { 0x3f000000, 0x3f1837f0, 0x3f3504f3, 0x3f5744fd };
    const IndividualChannelStream *ics = &sce->ics;
    const uint16_t *offs = ics->swb_offset;
    int g, w, i, win = 0;

    for (g = 0; g < ics->num_window_groups; g++) {
        for (w = 0; w < ics->group_len[g]; w++, win++) {
            float *spec = sce->coeffs + win * 128;
            for (i = 0; i < ics->max_sfb; i++) {
                int idx = g * ics->max_sfb + i, pb = g * 16 + i, k, n, e;
                int32_t *seed;
                float nrg = 0.0f, gain, t;
                if (sce->band_type[idx] != NOISE_BT)
                    continue;
                if (ch > 0 && cpe->mf_pns_corr[pb]) {
                    seed = &cpe->mf_pns_band_seed[pb];
                } else {
                    cpe->mf_pns_band_seed[pb] = cpe->mf_pns_seed;
                    seed = &cpe->mf_pns_seed;
                }
                n = offs[i + 1] - offs[i];
                for (k = 0; k < n; k++) {
                    float r;
                    *seed = (int32_t)((uint32_t)*seed * 1664525u + 1013904223u);
                    r = (float)*seed;
                    spec[offs[i] + k] = r;
                    nrg = nrg + r * r;
                }
                e = sce->mf_pns_nrg[idx];
                t = mf_bits2f(tab_bits[e & 3]);
                gain = mf_bits2f((uint32_t)(((e >> 2) + 1) * 0x800000 + 0x3f800000)) * t;
                gain = gain * (sqrtf(nrg) / nrg);
                gain = -gain; /* FFmpeg spectra carry the opposite sign (negative sf), exact */
                for (k = 0; k < n; k++)
                    spec[offs[i] + k] = gain * spec[offs[i] + k];
            }
        }
    }
}

/* Correlated noise (MapMidSideMaskToPnsCorrelation): with M/S present and PNS in either channel,
 * every band with the M/S bit set is correlated; if both channels are PNS there the M/S bit is
 * cleared. */
static void mf_pns_prepare(ChannelElement *cpe, int ms_present)
{
    memset(cpe->mf_pns_corr, 0, sizeof(cpe->mf_pns_corr));
    if (ms_present) {
        int g, i, any = 0;
        const IndividualChannelStream *ics = &cpe->ch[0].ics;
        for (i = 0; i < ics->num_window_groups * ics->max_sfb; i++)
            any |= cpe->ch[0].band_type[i] == NOISE_BT || cpe->ch[1].band_type[i] == NOISE_BT;
        if (any)
            for (g = 0; g < ics->num_window_groups; g++)
                for (i = 0; i < ics->max_sfb; i++) {
                    int idx = g * ics->max_sfb + i;
                    if (cpe->ms_mask[idx]) {
                        cpe->mf_pns_corr[g * 16 + i] = 1;
                        if (cpe->ch[0].band_type[idx] == NOISE_BT &&
                            cpe->ch[1].band_type[idx] == NOISE_BT)
                            cpe->ms_mask[idx] = 0;
                    }
                }
    }
}

#endif /* AVCODEC_AAC_AACDEC_MF_H */
