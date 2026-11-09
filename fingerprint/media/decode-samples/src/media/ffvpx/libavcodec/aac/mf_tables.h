/* Tables of the AAC decoder of Media Foundation (msauddecmft.dll 10.0.26100.9549), as bit
 * patterns: see mf_tables.c. */
#ifndef AVCODEC_AAC_MF_TABLES_H
#define AVCODEC_AAC_MF_TABLES_H

#include <stdint.h>

extern const uint32_t mf_SineTable1024Fl[2048];
extern const uint32_t mf_SineTable512Fl[1024];
extern const uint32_t mf_SineWindow1024Fl[1024];
extern const uint32_t mf_SineWindow128Fl[128];
extern const uint32_t mf_KBDWindow1024Fl[1024];
extern const uint32_t mf_KBDWindow128Fl[128];
extern const uint32_t mf_ExponentTableFl[128];
extern const uint32_t mf_tnsCoeff3Fl[8];
extern const uint32_t mf_tnsCoeff4Fl[16];

/* TNS reflection coefficient of the DLL (CDKaacDec_fl_tnsCoeff3/4): res is 3 or 4 bits, idx the
 * sign-extended code */
float mf_tns_coef(int res, int idx);

#endif
