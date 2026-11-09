/* imlt_block @0x18000cbe0 of msauddecmft.dll 10.0.26100.9549 (FDK imlt_block, float), AAC-LC path
 * (no FAC, no asymmetric overlap). Output is in the decoder's internal scale (x 2^15). */
#ifndef MF_IMLT_H
#define MF_IMLT_H
#include "mf_dct.h"
#define MF_OV_SIZE 2048
typedef struct {
    float overlap[MF_OV_SIZE];   /* mdct_t_fl +0x00 (time and freq halves share it) */
    const mf_spk *prev_wrs;      /* +0x08 */
    int prev_tl;                 /* +0x10 */
    int prev_nr;                 /* +0x14 */
    int prev_fr;                 /* +0x18 */
    int ov_offset;               /* +0x1c */
    int ov_size;                 /* +0x20 */
} mf_mdct;
void mf_mdct_init(mf_mdct *m);
int mf_imlt_block(mf_mdct *m, float *out, float *spec, int nspec, int no_out, int tl,
                  const mf_spk *wls, int fl, const mf_spk *wrs, int fr, int avx2_fma,
                  const mf_dct_tables *t);
#endif
