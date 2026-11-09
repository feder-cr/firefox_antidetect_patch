#include <string.h>
#include "mf_imlt.h"

void mf_mdct_init(mf_mdct *m)
{
    memset(m, 0, sizeof(*m));
    m->ov_size = MF_OV_SIZE;
}

/* imdct_adapt_parameters @0x180041cd0 */
static void adapt(mf_mdct *m, int *pfl, int *pnl, int tl, const mf_spk *wls, int no_out)
{
    int fl = *pfl, nl = *pnl, diff, use_cur = 0, use_prev = 0;
    (void)tl;
    if (m->prev_tl == 0) {
        m->prev_wrs = wls;
        m->prev_fr = fl;
        m->prev_nr = (no_out - fl) >> 1;
        m->prev_tl = no_out;
        m->ov_offset = 0;
        use_cur = 1;
    }
    diff = (m->prev_fr - fl) >> 1;
    if (m->prev_nr + diff > 0) use_cur = 1;
    if (nl - diff > 0) use_prev = 1;
    if (use_cur && use_prev && fl < m->prev_fr) use_cur = 0;
    if (use_cur) {
        m->prev_nr += diff;
        m->prev_fr = fl;
        m->prev_wrs = wls;
    } else {
        nl -= diff;
        fl = m->prev_fr;
    }
    *pfl = fl;
    *pnl = nl;
}

int mf_imlt_block(mf_mdct *m, float *out, float *spec, int nspec, int no_out, int tl,
                  const mf_spk *wls, int fl, const mf_spk *wrs, int fr, int avx,
                  const mf_dct_tables *t)
{
    const float scale = 1.0f / (float)tl;   /* gain 1.0: (1/tl) * 1.0 */
    const int nr = (tl - fr) >> 1;
    int nl = (tl - fl) >> 1;
    float *ovl, *o0 = out, *o1;
    int w, i, nsamp = 0;

    if (m->prev_fr != fl)
        adapt(m, &fl, &nl, tl, wls, no_out);
    ovl = m->overlap + m->ov_size - 1;
    if (no_out > 0) {
        for (i = 0; i < m->ov_offset; i++)
            *o0++ = m->overlap[i];
        nsamp = m->ov_offset;
        m->ov_offset = 0;
    }
    for (w = 0; w < nspec; w++) {
        float *ps = spec + w * tl, *pc;
        const mf_spk *win;
        if (m->prev_fr != fl)
            adapt(m, &fl, &nl, tl, wls, no_out);
        win = m->prev_wrs;
        mf_dct_iv(ps, tl, avx, t);
        for (i = 0; i < tl; i++)
            ps[i] = ps[i] * scale;
        if (no_out <= nsamp) {
            o0 = m->overlap + m->ov_offset;
            m->ov_offset += m->prev_nr + fl / 2;
        } else {
            nsamp += m->prev_nr + fl / 2;
        }
        for (i = 0; i < m->prev_nr; i++)
            *o0++ = -(*ovl--);
        if (no_out <= nsamp) {
            o1 = m->overlap + m->ov_offset + fl / 2 - 1;
            m->ov_offset += fl / 2 + nl;
        } else {
            o1 = o0 + (fl - 1);
            nsamp += fl / 2 + nl;
        }
        pc = ps + tl - fl / 2;
        for (i = 0; i < fl / 2; i++) {
            const float c = *pc++, v = *ovl--;
            *o0++ = c * win[i].im - v * win[i].re;
            *o1-- = -(v * win[i].im + c * win[i].re);
        }
        o0 += fl / 2 + nl;            /* skip the samples just written backwards and the nl part */
        o1 = o0 - nl;                 /* nl samples after the crossing: -current[FL/2..0] */
        pc = ps + tl - fl / 2 - 1;
        for (i = 0; i < nl; i++)
            *o1++ = -(*pc--);
        ovl = ps + tl / 2 - 1;
        m->prev_nr = nr;
        m->prev_fr = fr;
        m->prev_tl = tl;
        m->prev_wrs = wrs;
    }
    memcpy(m->overlap + m->ov_size - tl / 2, spec + (nspec - 1) * tl, (tl / 2) * sizeof(float));
    return nsamp;
}
