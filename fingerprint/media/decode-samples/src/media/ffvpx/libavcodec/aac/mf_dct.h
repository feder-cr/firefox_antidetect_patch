/* DCT-IV of msauddecmft.dll 10.0.26100.9549 (dct_IV @0x18000d560), portable C.
 * Tables are passed in by the caller, never read from the host at run time in production. */
#ifndef MF_DCT_H
#define MF_DCT_H
typedef struct { float re, im; } mf_spk;
typedef struct {
    const mf_spk *sine_table_1024;  /* SineTable1024Fl @0x1800a2c00: post-twiddle */
    const mf_spk *sine_table_512;   /* SineTable512Fl  @0x18009f120: FFT twiddles */
    const mf_spk *pre_twiddle[11];  /* [log2 N] = windowSlopesFl[log2 N - 2] (sine window of length N):
                                       N=1024 SineWindow1024Fl, 128 SineWindow128Fl, 64, 32 (QMF) */
} mf_dct_tables;
void mf_scramble(float *x, int n);
void mf_fft_rad2(float *x, int ldn, const mf_spk *trig, int trig_size);
void mf_dct_iv(float *x, int N, int avx2_fma, const mf_dct_tables *t);  /* N = 32, 64, 128, 1024 */
void mf_dst_iv(float *x, int N, int avx2_fma, const mf_dct_tables *t);  /* dst_IV @0x18000dd30 */
void mf_fft16(float *x);
#endif
