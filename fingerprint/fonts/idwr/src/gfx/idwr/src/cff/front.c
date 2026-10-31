/* The Adobe front end of the CFF rasterizer (notes/cff.md, group 2 "front"): the font
 * instance (CFFFontInst, 0xF78 bytes), the per-size setup, the proc table, the grow buffers,
 * the three requests (bitmap, metrics, outline), the allocators and the outline client sink.
 *
 *   AdobeInternalGetOutline  0x8aef0    AdobeInternalGetMetrics 0x8c0c0
 *   AdobePostRenderCommon    0x8c340    AdobeInternalGetBitmap  0x8c528
 *   AdobeDoSetUpIfNeeded     0x8cd28    InitGrowBuffs           0x8d4b8
 *   MakeFontDesc             0x8d5c8    InitBuildProcs          0x8d870
 *   GetCharStringPointers    0x8d914    ValidateCharDataDesc    0x8da30
 *   AdobeFreeFontInst        0x8ecdc    AdobeNewFont            0xb45a0
 *   ATMMemoryBufSize         0xe2610    internalRealloc         0xe2bb0
 *   CurveTo                  0xe4a20    LineTo                  0xe5170
 *   internalFree             0xe9380    MoveTo                  0xeaca0
 *   ClosePath                0xecac0    NewChar                 0xede60
 *   internalMalloc           0xef850    GrowPathBuffer          0x182f5c
 *   ManageCString            0x182fc0   ATM_ManageCString       0x183230
 *
 * Ported from the assembly of DWrite.dll 10.0.26100.9711. Every call to another function,
 * inside the group or not, goes through CFF_ORIG (the original entry point, so whatever is
 * swapped in there runs), and so do the function pointers the front end stores (the proc
 * table, the allocator records, the outline callbacks, GetBitmapMemory).
 *
 * Memory: the originals use the CRT heap DWrite imports (_o_malloc / _o_realloc / _o_free,
 * IAT 0x1be3e0 / 0x1be400 / 0x1be3b0) and my_free 0x8de18 (NULL check + _o_free); an
 * instance from a port is freed by an original and the other way round, so in oracle mode
 * the ports call the same imports. In standalone mode they are the C library's.
 *
 * The records on the callers' stacks (_t_T1CharDataDesc, _t_CharIO, the bitmap record, the
 * path procs, _t_BCRendProp) are in front.h. CFFFontInst is accessed by offset (the rdN and
 * wrN helpers, memcpy-based, so neither alignment nor strict aliasing matter).
 *
 * Negative controls (-DCFF_CONTROL_<NAME>, cff_check.py --control <name>): one per function,
 * each documented where it acts. */
#include <stdlib.h>
#include "cff_port.h"
#include "front.h"

#ifdef IDWR_STANDALONE
#define cff_AdobeInternalGetOutline_impl cff_AdobeInternalGetOutline
#define cff_AdobeInternalGetMetrics_impl cff_AdobeInternalGetMetrics
#define cff_AdobePostRenderCommon_impl cff_AdobePostRenderCommon
#define cff_AdobeInternalGetBitmap_impl cff_AdobeInternalGetBitmap
#define cff_AdobeDoSetUpIfNeeded_impl cff_AdobeDoSetUpIfNeeded
#define cff_InitGrowBuffs_impl cff_InitGrowBuffs
#define cff_MakeFontDesc_impl cff_MakeFontDesc
#define cff_InitBuildProcs_impl cff_InitBuildProcs
#define cff_GetCharStringPointers_impl cff_GetCharStringPointers
#define cff_ValidateCharDataDesc_impl cff_ValidateCharDataDesc
#define cff_AdobeFreeFontInst_impl cff_AdobeFreeFontInst
#define cff_AdobeNewFont_impl cff_AdobeNewFont
#define cff_ATMMemoryBufSize_impl cff_ATMMemoryBufSize
#define cff_internalRealloc_impl cff_internalRealloc
#define cff_CurveTo_impl cff_CurveTo
#define cff_LineTo_impl cff_LineTo
#define cff_internalFree_impl cff_internalFree
#define cff_MoveTo_impl cff_MoveTo
#define cff_ClosePath_impl cff_ClosePath
#define cff_NewChar_impl cff_NewChar
#define cff_internalMalloc_impl cff_internalMalloc
#define cff_GrowPathBuffer_impl cff_GrowPathBuffer
#define cff_ManageCString_impl cff_ManageCString
#define cff_ATM_ManageCString_impl cff_ATM_ManageCString
#endif

/* ---------------------------------------------------------------------------------------- */
/* RVAs of everything the front end calls or stores */

enum {
    RVA_AdobeInternalGetOutline = 0x8aef0,
    RVA_GetBitmapMemory = 0x8ace0, /* CffRasterizer::Implementation::GetBitmapMemory = GetOutlineMemory (ICF) */
    RVA_AdobeInternalGetMetrics = 0x8c0c0,
    RVA_AdobePostRenderCommon = 0x8c340,
    RVA_AdobeInternalGetBitmap = 0x8c528,
    RVA_AdobeDoSetUpIfNeeded = 0x8cd28,
    RVA_BCCnctMtx = 0x8d33c,
    RVA_InitGrowBuffs = 0x8d4b8,
    RVA_MakeFontDesc = 0x8d5c8,
    RVA_InitBuildProcs = 0x8d870,
    RVA_GetCharStringPointers = 0x8d914,
    RVA_ValidateCharDataDesc = 0x8da30,
    RVA_my_free = 0x8de18,
    RVA_AdobeCFFFreeBlend = 0x8de3c,
    RVA_AdobeCFFFreeVstore = 0x8de70,
    RVA_AdobeFreeFontInst = 0x8ecdc,
    RVA_ATMSetUpValues = 0xb01c0,
    RVA_BCTfmP = 0xb2460,
    RVA_fixmul = 0xb3340,
    RVA_GetEmSize = 0xb3788,
    RVA_AdobeNewFont = 0xb45a0,
    RVA_AdobeCFFSetupValues1 = 0xb46bc,
    RVA_AdobeCFFSetDictDefaults = 0xb4ff8,
    RVA_AdobeCFFParseDict = 0xb5208,
    RVA_ATMQRedCharBitMap = 0xb76f0,
    RVA_ATMCharOutline = 0xb8da0,
    RVA_ATMBuildBitMap = 0xb9370,
    RVA_AdobeCFFCheckBlendVector = 0x10c428,
    RVA_AdobeCFFGetOne = 0xd0bdc,
    RVA_ATMMemoryBufSize = 0xe2610,
    RVA_internalRealloc = 0xe2bb0,
    RVA_CurveTo = 0xe4a20,
    RVA_LineTo = 0xe5170,
    RVA_internalFree = 0xe9380,
    RVA_MoveTo = 0xeaca0,
    RVA_ClosePath = 0xecac0,
    RVA_NewChar = 0xede60,
    RVA_internalMalloc = 0xef850,
    RVA_GrowPathBuffer = 0x182f5c,
    RVA_ManageCString = 0x182fc0,
    /* the CRT imports (IAT slots) */
    IAT_o_free = 0x1be3b0,
    IAT_o_malloc = 0x1be3e0,
    IAT_o_realloc = 0x1be400,
};

/* .rdata 0x18020a200: the Standard Encoding (code -> SID), 256 bytes, for ManageCString */
static const uint8_t g_std_encoding[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40,
    0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50,
    0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e,
    0x00, 0x6f, 0x70, 0x71, 0x72, 0x00, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x00, 0x7b,
    0x00, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x00, 0x84, 0x85, 0x00, 0x86, 0x87, 0x88,
    0x89, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x8a, 0x00, 0x8b, 0x00, 0x00, 0x00, 0x00, 0x8c, 0x8d, 0x8e, 0x8f, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x90, 0x00, 0x00, 0x00, 0x91, 0x00, 0x00, 0x92, 0x93, 0x94, 0x95, 0x00, 0x00, 0x00, 0x00,
};

/* ---------------------------------------------------------------------------------------- */
/* field access (CFFFontInst and the records of other groups) */

static inline uint8_t rd8(const void *p, size_t o) { return ((const uint8_t *)p)[o]; }
static inline uint16_t rd16(const void *p, size_t o) {
    uint16_t v;
    memcpy(&v, (const uint8_t *)p + o, 2);
    return v;
}
static inline uint32_t rd32(const void *p, size_t o) {
    uint32_t v;
    memcpy(&v, (const uint8_t *)p + o, 4);
    return v;
}
static inline void *rdp(const void *p, size_t o) {
    void *v;
    memcpy(&v, (const uint8_t *)p + o, sizeof v);
    return v;
}
static inline void wr16(void *p, size_t o, uint16_t v) { memcpy((uint8_t *)p + o, &v, 2); }
static inline void wr32(void *p, size_t o, uint32_t v) { memcpy((uint8_t *)p + o, &v, 4); }
static inline void wr64(void *p, size_t o, uint64_t v) { memcpy((uint8_t *)p + o, &v, 8); }
static inline void wrp(void *p, size_t o, const void *v) { memcpy((uint8_t *)p + o, &v, sizeof v); }

#define FN(rva) idwr_orig(rva) /* an original entry point, as a stored function pointer */

/* ---------------------------------------------------------------------------------------- */
/* the heap */

typedef void *(*malloc_fn)(size_t);
typedef void *(*realloc_fn)(void *, size_t);
typedef void (*free_fn)(void *);

#ifdef IDWR_STANDALONE
static void *crt_malloc(size_t n) { return malloc(n); }
static void *crt_realloc(void *p, size_t n) { return realloc(p, n); }
static void my_free(void *p) {
    if (p) free(p);
}
#else
static void *crt_malloc(size_t n) { return (*(malloc_fn *)idwr_orig(IAT_o_malloc))(n); }
static void *crt_realloc(void *p, size_t n) { return (*(realloc_fn *)idwr_orig(IAT_o_realloc))(p, n); }
static void my_free(void *p) { CFF_ORIG(free_fn, RVA_my_free)(p); }
#endif

/* ---------------------------------------------------------------------------------------- */
/* the calls out */

typedef int32_t (*int_inst_fn)(uint8_t *);
typedef void (*void_inst_fn)(uint8_t *);
typedef void (*free_blend_fn)(void *);
typedef void (*get_em_size_fn)(void *, int32_t *);
typedef int32_t (*fixmul_fn)(int32_t, int32_t);
typedef void (*cnct_mtx_fn)(void *, void *, void *);
typedef void (*set_dict_defaults_fn)(uint8_t *, uint32_t);
typedef int32_t (*parse_dict_fn)(uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, int32_t);
typedef int32_t (*check_blend_fn)(void *, uint32_t, uint32_t, void *);
typedef int32_t (*get_one_fn)(uint8_t *, const uint8_t *, const uint8_t *, const uint8_t *);
typedef void (*make_font_desc_fn)(uint8_t *, uint8_t *);
typedef int32_t (*get_cs_ptrs_fn)(uint16_t, uint8_t *, uint8_t **, uint8_t **);
typedef int32_t (*validate_fn)(cff_t1_char_data_desc *, uint8_t *);
typedef void (*post_render_fn)(uint8_t *, cff_char_io *, uint32_t);
typedef int32_t (*setup_if_needed_fn)(uint8_t *, uint16_t, cff_rend_prop *, int32_t *, uint16_t, uint32_t);
typedef void *(*get_memory_fn)(uint32_t);
typedef uint32_t (*grow_path_fn)(cff_grow_buff *);
/* the proc table */
typedef void (*mem_buf_size_fn)(int32_t *, int32_t *, int32_t *, int32_t *, int32_t *, int32_t *, int32_t *,
                                int32_t *, int32_t *);
typedef int32_t (*set_up_values_fn)(void *, void *, void *, cff_rend_prop *, void *, void *, uint32_t, void *);
typedef int32_t (*consumer_fn)(void *, void *, cff_t1_char_data_desc *, cff_char_io *, void *, uint32_t, void *);
typedef int32_t (*qred_fn)(void *, void *, cff_t1_char_data_desc *, cff_char_io *, cff_bitmap_rec *, void *,
                           uint32_t, void *);
typedef uint8_t *(*manage_cstring_fn)(uint32_t, void *, void *, uint8_t **);

#define FONT_DATA(inst) ((uint8_t *)rdp(inst, 0x08))
#define FONT_END(inst) (FONT_DATA(inst) + rd32(inst, 0x10))

/* ---------------------------------------------------------------------------------------- */
/* AdobeNewFont 0xb45a0: malloc the instance, fill it with 0xBD, parse the font
 * (AdobeCFFSetupValues1) and return the glyph count and the em size of the font matrix.
 * Control: the font data size is halved. (Filling the instance with 0 instead of 0xBD
 * changes nothing in the corpus: no field is read before it is written.) */

CFF_API int32_t cff_AdobeNewFont_impl(uint8_t *data, uint32_t size, uint32_t cff2, uint8_t **out,
                                      uint16_t *glyph_count, uint16_t *units_per_em) {
    *glyph_count = 0;
    uint8_t *inst = crt_malloc(0xf78);
    *out = inst;
    if (!inst) return (int32_t)0xfc000000;
    memset(inst, 0xbd, 0xf78);
    wrp(inst, 0x08, data);
#ifdef CFF_CONTROL_ADOBENEWFONT
    size /= 2;
#endif
    wr32(inst, 0x10, size);
    wr32(inst, 0x14, cff2);
    uint16_t upem = *units_per_em;
    wr32(inst, 0xfc, (uint16_t)(upem - 0x10) <= 0x3ff0 ? (uint32_t)upem << 16 : 0x3e80000);
    int32_t r = CFF_ORIG(int_inst_fn, RVA_AdobeCFFSetupValues1)(inst);
    if (r) {
        CFF_ORIG(void_inst_fn, RVA_AdobeFreeFontInst)(*out);
        *out = NULL;
        return r;
    }
    *glyph_count = rd16(inst, 0x90);
    int32_t em = 0;
    CFF_ORIG(get_em_size_fn, RVA_GetEmSize)(inst + 0xdc, &em);
    *units_per_em = (uint16_t)((int32_t)((uint32_t)em + 0x8000) >> 16);
    return 0;
}
CFF_EXPORT_STACK(cff_AdobeNewFont, cff_AdobeNewFont_impl);

/* AdobeFreeFontInst 0x8ecdc. Control: the grow buffers are not freed (a leak: no output
 * changes; front_check.py measures the process heap). */

CFF_API void cff_AdobeFreeFontInst_impl(uint8_t *inst) {
    if (!inst) return;
    wrp(inst, 0x5d0, NULL);
    wr32(inst, 0x5c8, 3);
    for (int i = 0; i < 9; i++) {
#ifndef CFF_CONTROL_ADOBEFREEFONTINST
        my_free(rdp(inst, 0x640 + 16 * i));
#endif
        wr32(inst, 0x648 + 16 * i, 0);
    }
    my_free(rdp(inst, 0xcb8));
    my_free(rdp(inst, 0xf58));
    my_free(rdp(inst, 0x58));
    my_free(rdp(inst, 0x60));
    my_free(rdp(inst, 0x68));
    my_free(rdp(inst, 0x78));
    CFF_ORIG(void_inst_fn, RVA_AdobeCFFFreeVstore)(inst);
    CFF_ORIG(free_blend_fn, RVA_AdobeCFFFreeBlend)(rdp(inst, 0x30));
    CFF_ORIG(free_blend_fn, RVA_AdobeCFFFreeBlend)(rdp(inst, 0x38));
    my_free(rdp(inst, 0x48));
    my_free(rdp(inst, 0x88));
    my_free(inst);
}
CFF_EXPORT(cff_AdobeFreeFontInst, cff_AdobeFreeFontInst_impl);

/* ---------------------------------------------------------------------------------------- */
/* AdobeDoSetUpIfNeeded 0x8cd28: picks the FD of the glyph (FDSelect format 0 or 3), and
 * reruns the setup (proc table, grow buffers, font matrix, device matrix, _t_FontDesc,
 * ATMSetUpValues) when the state is dirty or the FD, the vertical overscale, the transform,
 * the flags or (CFF2) the blend vector changed; a new FD's Private DICT is parsed first.
 * Control: the device transform is not transposed before BCCnctMtx. */

CFF_API int32_t cff_AdobeDoSetUpIfNeeded_impl(uint8_t *inst, uint16_t voverscale, cff_rend_prop *rp,
                                              int32_t *mtx, uint16_t glyph, uint32_t flags) {
    uint8_t *lo = FONT_DATA(inst);
    uint8_t *hi = lo + rd32(inst, 0x10);
    uint8_t *fdsel = rdp(inst, 0x538);
    uint16_t fd = 0;
    if (fdsel) {
        if (fdsel >= hi || fdsel < lo) return (int32_t)0xe011024a;
        uint8_t fmt = fdsel[0];
        if (fmt == 3) {
            uint8_t *q = fdsel + 1;
            uint8_t c = (q >= lo && q < hi) ? *q : 0xff;
            uint16_t nranges = (uint16_t)(c << 8);
            q = fdsel + 2;
            if (q < lo || q >= hi) return (int32_t)0xe0110254;
            nranges |= *q;
            if (fdsel + 5 + (uint32_t)(nranges * 3) > hi) return (int32_t)0xe0110258;
            uint16_t prev = 0;
            uint8_t *r8 = fdsel + 6;
            for (uint16_t i = 0; i < nranges; i++) {
                uint16_t first = (uint16_t)(r8[0] << 8 | r8[1]);
                r8 += 1;
                if (glyph < first && !(prev > glyph)) {
                    fd = r8[-2];
                    break;
                }
                if (first >= rd16(inst, 0x90)) return (int32_t)0xe0110266;
                if ((uint32_t)i == (uint32_t)nranges - 1) return (int32_t)0xe0110266;
                prev = first;
                r8 += 2;
            }
        } else {
            if (fmt != 0) return (int32_t)0xe0110271;
            if (fdsel + rd16(inst, 0x90) + 1 > hi) return (int32_t)0xe011026e;
            fd = fdsel[(size_t)glyph + 1];
        }
    }
    if (rd32(inst, 0x5c8) != 1) {
        if (fd == rd16(inst, 0x558) && rd16(inst, 0) == voverscale && mtx[0] == (int32_t)rd32(inst, 0x970) &&
            mtx[1] == (int32_t)rd32(inst, 0x974) && mtx[2] == (int32_t)rd32(inst, 0x978) &&
            mtx[3] == (int32_t)rd32(inst, 0x97c) && flags == rd32(inst, 0x99c)) {
            if (rd32(inst, 0x14) == 0) return 0;
            if (CFF_ORIG(check_blend_fn, RVA_AdobeCFFCheckBlendVector)(rdp(inst, 0x30), rd32(inst, 0x50),
                                                                       rd32(inst, 0x40), rdp(inst, 0x48)) == 0)
                return 0;
        }
    }
    wr32(inst, 0x5c8, 1);
    if ((rd32(inst, 0x14) != 0 || fdsel) && fd != rd16(inst, 0x558)) {
        /* the FD's Private DICT: FDArray INDEX entry fd */
        if ((uint32_t)fd > rd32(inst, 0x4cc)) return (int32_t)0xe0110284;
        uint16_t offsize = rd16(inst, 0x550);
        uint8_t *p = (uint8_t *)rdp(inst, 0x540) + (uint32_t)(offsize * fd);
        uint32_t off1 = 0, off2 = 0;
        for (int32_t i = 0; i < (int32_t)offsize; i++) {
            if (p < lo || p >= hi) return (int32_t)0xe0110289;
            uint32_t c = *p++ + off1;
            off1 = (i == (int32_t)offsize - 1) ? c : c << 8;
        }
        for (int32_t i = 0; i < (int32_t)offsize; i++) {
            if (p < lo || p >= hi) return (int32_t)0xe0110291;
            uint32_t c = *p++ + off2;
            off2 = (i == (int32_t)offsize - 1) ? c : c << 8;
        }
        wr16(inst, 0x558, 0xffff);
        CFF_ORIG(set_dict_defaults_fn, RVA_AdobeCFFSetDictDefaults)(inst, 0);
        uint8_t *base = rdp(inst, 0x548);
        int32_t r = CFF_ORIG(parse_dict_fn, RVA_AdobeCFFParseDict)(inst, base + off1, lo, hi, base + off2, 1);
        if (r) return r;
        uint8_t *priv = rdp(inst, 0x528);
        if (priv) {
            r = CFF_ORIG(parse_dict_fn, RVA_AdobeCFFParseDict)(inst, priv, lo, hi, rdp(inst, 0x530), 2);
            if (r) return r;
        }
    }
    wr32(inst, 0x970, (uint32_t)mtx[0]);
    wr32(inst, 0x978, (uint32_t)mtx[2]);
    wr32(inst, 0x974, (uint32_t)mtx[1]);
    wr32(inst, 0x97c, (uint32_t)mtx[3]);
    wr16(inst, 0x558, fd);
    wr32(inst, 0x99c, flags);
    wr16(inst, 0, voverscale);
    CFF_ORIG(void_inst_fn, RVA_InitBuildProcs)(inst);
    wr32(inst, 0x608, 0x68);
    wrp(inst, 0x5d8, FN(RVA_ATMSetUpValues));
    wrp(inst, 0x5e8, FN(RVA_ATMBuildBitMap));
    wrp(inst, 0x5f0, FN(RVA_ATMCharOutline));
    wrp(inst, 0x600, FN(RVA_BCTfmP));
    wrp(inst, 0x5f8, FN(RVA_ATMMemoryBufSize));
    int32_t r = CFF_ORIG(int_inst_fn, RVA_InitGrowBuffs)(inst);
    if (r) return r;
    /* the font matrix: Top DICT (+0xac), FD (+0xc4) or their product, into +0xdc */
    if (rd32(inst, 0xf4) != 0) {
        memmove(inst + 0xdc, inst + 0xac, 24);
        if (rd32(inst, 0xf8) != 0) CFF_ORIG(cnct_mtx_fn, RVA_BCCnctMtx)(inst + 0xc4, inst + 0xac, inst + 0xdc);
    } else if (rd32(inst, 0xf8) != 0) {
        memmove(inst + 0xdc, inst + 0xc4, 24);
    } else {
        memmove(inst + 0xdc, inst + 0xac, 24);
    }
    /* the device matrix: font matrix x the transform (transposed), no translation */
#ifdef CFF_CONTROL_ADOBEDOSETUPIFNEEDED
    int32_t dev[6] = {mtx[0], mtx[1], mtx[2], mtx[3], 0, 0};
#else
    int32_t dev[6] = {mtx[0], mtx[2], mtx[1], mtx[3], 0, 0};
#endif
    CFF_ORIG(cnct_mtx_fn, RVA_BCCnctMtx)(inst + 0xdc, dev, inst + 0x980);
    CFF_ORIG(make_font_desc_fn, RVA_MakeFontDesc)(inst, inst + 0x6d0);
    memset(rp->zero_e, 0, 4);
    rp->a0 = 0x16a0a;
    rp->a4 = 0x16a0a;
    rp->one = 1;
    if (voverscale == 5 || voverscale == 4) {
        rp->mode14 = 3;
        rp->voverscale8 = voverscale;
        rp->voverscale_c = voverscale;
    } else {
        rp->mode14 = 0;
        rp->voverscale8 = 1;
        rp->voverscale_c = 1;
    }
    r = ((set_up_values_fn)rdp(inst, 0x5d8))(inst + 0x6d0, inst + 0x980, NULL, rp, inst + 0x610, inst + 0x9a0, flags,
                                             NULL);
    if (r) return r;
    wr32(inst, 0x5c8, 2);
    return 0;
}
CFF_EXPORT_STACK(cff_AdobeDoSetUpIfNeeded, cff_AdobeDoSetUpIfNeeded_impl);

/* InitGrowBuffs 0x8d4b8: the nine sizes from the proc at +0x5f8 (ATMMemoryBufSize); each
 * buffer smaller than its size is reallocated. Control: buffer 3 fails to grow. */

CFF_API int32_t cff_InitGrowBuffs_impl(uint8_t *inst) {
    int32_t s[9];
    ((mem_buf_size_fn)rdp(inst, 0x5f8))(&s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &s[6], &s[7], &s[8]);
    for (int i = 0; i < 9; i++) {
        uint32_t want = (uint32_t)s[i];
        if (want == 0 || rd32(inst, 0x648 + 16 * i) >= want) continue;
        void *q = crt_realloc(rdp(inst, 0x640 + 16 * i), want);
#ifdef CFF_CONTROL_INITGROWBUFFS
        if (i == 3) q = NULL;
#endif
        if (!q) {
            my_free(rdp(inst, 0x640 + 16 * i));
            wrp(inst, 0x640 + 16 * i, NULL);
            wr32(inst, 0x648 + 16 * i, 0);
            return (int32_t)0xfc1101fa;
        }
        wrp(inst, 0x640 + 16 * i, q);
        wr32(inst, 0x648 + 16 * i, want);
    }
    return 0;
}
CFF_EXPORT(cff_InitGrowBuffs, cff_InitGrowBuffs_impl);

/* MakeFontDesc 0x8d5c8: _t_FontDesc (0x200 bytes) from the font matrix and the Private DICT
 * values. Control: the first zone list (count at +0x42) is left empty. */

CFF_API void cff_MakeFontDesc_impl(uint8_t *inst, uint8_t *d) {
    memset(d + 2, 0, 0x1fe);
    wr16(d, 0, 5);
    wr16(d, 2, rd16(inst, 0x99a));
    memcpy(d + 0x04, inst + 0xdc, 16);
    memcpy(d + 0x14, inst + 0xec, 8);
    wr32(d, 0x2c, rd32(inst, 0x45c));
    wr32(d, 0x34, rd32(inst, 0x494));
    wr32(d, 0x3c, rd32(inst, 0x470));
    wr16(d, 0x40, rd16(inst, 0x474));
#ifdef CFF_CONTROL_MAKEFONTDESC
    wr16(d, 0x42, 0);
#else
    wr16(d, 0x42, rd16(inst, 0x1c4));
#endif
    for (int32_t i = 0; i < (int32_t)rd16(d, 0x42); i++) wr32(d, 0x94 + 4 * i, rd32(inst, 0x1c8 + 4 * i));
    wr16(d, 0x44, rd16(inst, 0x100));
    for (int32_t i = 0; i < (int32_t)rd16(d, 0x44); i++) wr32(d, 0xc4 + 4 * i, rd32(inst, 0x104 + 4 * i));
    wr16(d, 0x46, rd16(inst, 0x294));
    for (int32_t i = 0; i < (int32_t)rd16(d, 0x46); i++) {
        wr32(d, 0x104 + 8 * i, rd32(inst, 0x390 + 8 * i));
        wr32(d, 0x100 + 8 * i, rd32(inst, 0x38c + 8 * i));
    }
    wr16(d, 0x48, rd16(inst, 0x296));
    for (int32_t i = 0; i < (int32_t)rd16(d, 0x48); i++) {
        wr32(d, 0x164 + 8 * i, rd32(inst, 0x3f0 + 8 * i));
        wr32(d, 0x160 + 8 * i, rd32(inst, 0x3ec + 8 * i));
    }
    wr16(d, 0x4a, rd16(inst, 0x298));
    wr16(d, 0x4c, 1);
    wr32(d, 0x50, rd32(inst, 0x4c8));
    wr16(d, 0x58, rd16(inst, 0x4c2));
    memcpy(d + 0x60, inst + 0x478, 16);
    wr32(d, 0x70, rd32(inst, 0x598));
    wr32(d, 0x74, rd32(inst, 0x594));
    wr32(d, 0x78, rd32(inst, 0x498));
    wr32(d, 0x7c, rd32(inst, 0x49c));
    wr32(d, 0x80, rd32(inst, 0x4a0));
    wr32(d, 0x84, rd32(inst, 0x4a4));
    memcpy(d + 0x88, inst + 0x4a8, 8);
    wr32(d, 0x90, rd32(inst, 0x4b0));
    wr32(d, 0xf4, rd32(inst, 0x288));
    wr32(d, 0xf8, rd32(inst, 0x28c));
    wr32(d, 0xfc, rd32(inst, 0x290));
    wr32(d, 0x1c0, rd32(inst, 0x588));
    wr32(d, 0x1c4, rd32(inst, 0x468));
    wr32(d, 0x20, rd32(inst, 0x450));
    wr32(d, 0x24, rd32(inst, 0x44c));
    wr32(d, 0x1c8, rd32(inst, 0x14));
    memcpy(d + 0x1d0, inst + 0x38, 8);
    wr32(d, 0x1d8, rd32(inst, 0x50));
    wr32(d, 0x1dc, rd32(inst, 0x40));
    memcpy(d + 0x1e0, inst + 0x48, 8);
    wr32(d, 0x1e8, rd32(inst, 0x80));
    memcpy(d + 0x1f0, inst + 0x78, 8);
    memcpy(d + 0x1f8, inst + 0x88, 8);
}
CFF_EXPORT(cff_MakeFontDesc, cff_MakeFontDesc_impl);

/* InitBuildProcs 0x8d870: the memory proc slots (+0x618 NULL, +0x620 ManageCString) and the
 * two allocator records +0x630 -> +0x8d0 -> +0x8d8 and +0x638 -> +0x920 -> +0x928, each
 * {+0 malloc, +0x10 free, +0x18 malloc, +0x28 realloc, +0x30 realloc} (slots between are
 * left as they are). Control: the realloc slots point to a realloc that always fails. */

#ifdef CFF_CONTROL_INITBUILDPROCS
static void *control_failing_realloc(void *zone, void *p, uint32_t n) {
    (void)zone, (void)p, (void)n;
    return NULL;
}
#endif

CFF_API void cff_InitBuildProcs_impl(uint8_t *inst) {
    void *m = FN(RVA_internalMalloc), *f = FN(RVA_internalFree);
#ifdef CFF_CONTROL_INITBUILDPROCS
    void *re = (void *)control_failing_realloc;
#else
    void *re = FN(RVA_internalRealloc);
#endif
    wrp(inst, 0x618, NULL);
    wrp(inst, 0x620, FN(RVA_ManageCString));
    wrp(inst, 0x630, inst + 0x8d0);
    wrp(inst, 0x8d0, inst + 0x8d8);
    wrp(inst, 0x638, inst + 0x920);
    wrp(inst, 0x920, inst + 0x928);
    wrp(inst, 0x8d8, m);
    wrp(inst, 0x908, re);
    wrp(inst, 0x900, re);
    wrp(inst, 0x958, re);
    wrp(inst, 0x950, re);
    wrp(inst, 0x8f0, m);
    wrp(inst, 0x8e8, f);
    wrp(inst, 0x940, m);
    wrp(inst, 0x928, m);
    wrp(inst, 0x938, f);
}
CFF_EXPORT(cff_InitBuildProcs, cff_InitBuildProcs_impl);

/* ---------------------------------------------------------------------------------------- */
/* GetCharStringPointers 0x8d914: the CharStrings INDEX entry of a glyph (offset array at
 * +0x4e8, offSize at +0x556, data base - 1 at +0x4f0). Control: the end is one byte short. */

static uint32_t read_offset(const uint8_t **q, uint16_t n) {
    uint32_t off = 0;
    for (uint16_t i = 0; i < n; i++) {
        uint32_t c = *(*q)++ + off;
        off = ((uint32_t)i == (uint32_t)n - 1) ? c : c << 8;
    }
    return off;
}

CFF_API int32_t cff_GetCharStringPointers_impl(uint16_t glyph, uint8_t *inst, uint8_t **start, uint8_t **end) {
    if (glyph >= rd16(inst, 0x90)) return (int32_t)0xdd110101;
    uint16_t n = rd16(inst, 0x556);
    const uint8_t *q = (const uint8_t *)rdp(inst, 0x4e8) + (int64_t)(int32_t)((uint32_t)glyph * n);
    if (q + (uint32_t)(2 * n) > FONT_END(inst)) return (int32_t)0xe0110104;
    uint32_t a = read_offset(&q, n);
    *start = (uint8_t *)rdp(inst, 0x4f0) + a;
    uint32_t b = read_offset(&q, rd16(inst, 0x556));
#ifdef CFF_CONTROL_GETCHARSTRINGPOINTERS
    b -= 1;
#endif
    *end = (uint8_t *)rdp(inst, 0x4f0) - 1 + b;
    return 0;
}
CFF_EXPORT(cff_GetCharStringPointers, cff_GetCharStringPointers_impl);

/* ValidateCharDataDesc 0x8da30: the subrs offset arrays and the charstring inside the font
 * data; stores the data bounds into the descriptor. Returns with R8 = the descriptor in the
 * original, which AdobeInternalGetOutline does not actually use (its next use of R8 is
 * `sbb r8d, r8d`, a write): the inventory's hazard is a false positive. Control: the data
 * end stored in the descriptor is the start. */

CFF_API int32_t cff_ValidateCharDataDesc_impl(cff_t1_char_data_desc *d, uint8_t *inst) {
    uint8_t *lo = FONT_DATA(inst);
    uint8_t *hi = lo + rd32(inst, 0x10);
    uint32_t n = rd32(inst, 0x4c8);
    if (n) {
        if ((uint8_t)(d->gsubrs_offsize - 1) > 3 || d->gsubrs_offsets < lo ||
            d->gsubrs_offsets + (uint32_t)(d->gsubrs_offsize * (n + 1)) > hi)
            return (int32_t)0xe01102ea;
    }
    n = rd32(inst, 0x470);
    if (n) {
        if ((uint8_t)(d->lsubrs_offsize - 1) > 3 || d->lsubrs_offsets < lo ||
            d->lsubrs_offsets + (uint32_t)(d->lsubrs_offsize * (n + 1)) > hi)
            return (int32_t)0xe01102f0;
    }
    if (d->cs_start < lo || d->cs_start >= hi || d->cs_end < lo || d->cs_end >= hi) return (int32_t)0xe01102f4;
    d->data_lo = lo;
#ifdef CFF_CONTROL_VALIDATECHARDATADESC
    hi = lo + 1;
#endif
    d->data_hi = hi;
    return 0;
}
CFF_EXPORT(cff_ValidateCharDataDesc, cff_ValidateCharDataDesc_impl);

/* AdobePostRenderCommon 0x8c340: _t_CharIO -> DWrite's GlyphMetrics: the box rounded
 * outward (floor of the min, ceil of the max), the advance (0 for CFF2: from HVAR/VVAR).
 * Control: the advance x is one pixel larger. */

CFF_API void cff_AdobePostRenderCommon_impl(uint8_t *gm, cff_char_io *io, uint32_t cff2) {
    const int32_t *box = io->box;
    int32_t ymax = (int32_t)((uint32_t)box[3] + 0xffff) >> 16, ymin = (int16_t)rd16(box, 6);
    wr32(gm, 0x24, (uint32_t)(ymax - ymin));
    box = io->box;
    wr32(gm, 0x20, (uint32_t)(((int32_t)((uint32_t)box[2] + 0xffff) >> 16) - (int16_t)rd16(box, 2)));
    if (!cff2) {
#ifdef CFF_CONTROL_ADOBEPOSTRENDERCOMMON
        wr32(gm, 0x10, rd32(io->metrics, 0x20) + 0x10000);
#else
        wr32(gm, 0x10, rd32(io->metrics, 0x20));
#endif
        wr32(gm, 0x14, rd32(io->metrics, 0x24));
    } else {
        wr32(gm, 0x10, 0);
        wr32(gm, 0x14, 0);
    }
    wr64(gm, 0x18, 0);
    box = io->box;
    wr32(gm, 0x00, (uint32_t)(int32_t)(int16_t)rd16(box, 2));
    wr64(gm, 0x08, 0);
    wr32(gm, 0x04, (uint32_t)((int32_t)((uint32_t)io->box[3] + 0xffff) >> 16));
}
CFF_EXPORT(cff_AdobePostRenderCommon, cff_AdobePostRenderCommon_impl);

/* ---------------------------------------------------------------------------------------- */
/* the shared start of the three requests: setup, charstring pointers, descriptor */

static int32_t prepare(uint8_t *inst, uint16_t glyph, uint16_t voverscale, cff_rend_prop *rp, const int32_t *transform,
                       uint32_t flags, uint32_t overscale_mode, cff_t1_char_data_desc *d) {
    int32_t mtx[4];
    memcpy(mtx, transform, 16);
    int32_t r = CFF_ORIG(setup_if_needed_fn, RVA_AdobeDoSetUpIfNeeded)(inst, voverscale, rp, mtx, glyph, flags & 1);
    if (r) return r;
    wr32(inst, 0xf70, overscale_mode);
    CFF_ORIG(get_cs_ptrs_fn, RVA_GetCharStringPointers)(glyph, inst, &d->cs_start, &d->cs_end); /* result unused */
    wrp(inst, 0x4e0, d->cs_start);
    d->lsubrs_base = rdp(inst, 0x4f8);
    d->lsubrs_offsets = rdp(inst, 0x500);
    d->gsubrs_base = rdp(inst, 0x508);
    d->gsubrs_offsets = rdp(inst, 0x510);
    d->lsubrs_offsize = rd8(inst, 0x552);
    d->gsubrs_offsize = rd8(inst, 0x554);
    memset(d->zero20, 0, sizeof d->zero20);
    return CFF_ORIG(validate_fn, RVA_ValidateCharDataDesc)(d, inst);
}

static void grow_pointers(uint8_t *inst, cff_grow_buff *gb[9]) {
    for (int i = 0; i < 9; i++) gb[i] = (cff_grow_buff *)(inst + 0x640 + 16 * i);
}

static uint16_t voverscale_of(uint32_t mode) { return mode == 2 ? 5 : mode == 4 ? 4 : 1; }

/* the flags of the consumer call: RenderingFlags bits 0 and 1 */
static uint32_t proc_flags(uint32_t flags) { return ((flags & 2) ? 2u : 0u) | (flags & 1); }

/* AdobeInternalGetOutline 0x8aef0: ATMCharOutline with the client callbacks below, which
 * record the path into grow buffer 8 (+0x6c0): {count, 1, then per element its type
 * (2 move, 3 line, 4 curve, 5 close) and points}. The elements after the two header words
 * are copied into a buffer from GetBitmapMemory (reused when large enough).
 * Control: the last element word is dropped. */

CFF_API int32_t cff_AdobeInternalGetOutline_impl(uint8_t *inst, void *memory_fn, uint16_t glyph,
                                                 const int32_t *transform, uint32_t overscale_mode, uint32_t flags,
                                                 uint8_t *glyph_metrics, int32_t **out, int32_t *out_size) {
    (void)memory_fn;
    cff_t1_char_data_desc d;
    memset(&d, 0, sizeof d);
    cff_rend_prop rp;
    memset(&rp, 0, sizeof rp);
    cff_char_io io;
    memset(&io, 0, sizeof io);
    uint8_t metrics[0x28];
    memset(metrics, 0, sizeof metrics);
    int32_t box[4] = {0, 0, 0, 0};
    if (glyph >= rd16(inst, 0x90)) return (int32_t)0xdd110302;
    int32_t r = prepare(inst, glyph, voverscale_of(overscale_mode), &rp, transform, flags, overscale_mode, &d);
    if (r) return r;
    uint32_t saved = rd32(inst, 0xcdc);
    cff_grow_buff *gb[9];
    grow_pointers(inst, gb);
    io.grow = gb;
    io.metrics = metrics;
    io.box = box;
    cff_path_procs procs = {FN(RVA_NewChar), FN(RVA_MoveTo), FN(RVA_LineTo), FN(RVA_CurveTo), FN(RVA_ClosePath), NULL};
    wrp(inst, 0x610, NULL);
    io.flags18 = 0;
    io.zero28 = 0;
    if (flags & 2) wr32(inst, 0xcdc, 7);
    r = ((consumer_fn)rdp(inst, 0x5f0))(inst + 0x9a0, inst + 0x610, &d, &io, &procs, proc_flags(flags), inst);
    wr32(inst, 0xcdc, saved);
    if (r) return r;
    int32_t *path = rdp(inst, 0x6c0);
    int32_t n = 0;
    if (path) {
        int32_t count = path[0];
        if ((int32_t)((uint32_t)count * 4) > (int32_t)rd32(inst, 0x6c8)) return (int32_t)0xe01104de;
        n = (int32_t)((uint32_t)count * 4 - 8);
#ifdef CFF_CONTROL_ADOBEINTERNALGETOUTLINE
        n -= 4;
#endif
        if (*out == NULL || n > *out_size) {
            *out = CFF_ORIG(get_memory_fn, RVA_GetBitmapMemory)((uint32_t)n);
            if (!*out) return (int32_t)0xde1104eb;
        }
        if (n > 0) memcpy(*out, path + 2, (size_t)(int64_t)n);
    }
    *out_size = n;
    CFF_ORIG(post_render_fn, RVA_AdobePostRenderCommon)(glyph_metrics, &io, rd32(inst, 0x14));
    return 0;
}
CFF_EXPORT_STACK(cff_AdobeInternalGetOutline, cff_AdobeInternalGetOutline_impl);

/* AdobeInternalGetMetrics 0x8c0c0: ATMCharOutline with no memory callback and no path
 * callbacks: only the box and the advance are kept. Control: the vertical overscale is 5. */

CFF_API int32_t cff_AdobeInternalGetMetrics_impl(uint8_t *inst, uint16_t glyph, const int32_t *transform,
                                                 uint32_t overscale_mode, uint32_t flags, uint8_t *glyph_metrics) {
    (void)overscale_mode;
    cff_t1_char_data_desc d;
    memset(&d, 0, sizeof d);
    cff_rend_prop rp;
    memset(&rp, 0, sizeof rp);
    cff_char_io io;
    memset(&io, 0, sizeof io);
    uint8_t metrics[0x28];
    memset(metrics, 0, sizeof metrics);
    int32_t box[4] = {0, 0, 0, 0};
    if (glyph >= rd16(inst, 0x90)) return (int32_t)0xdd110302;
#ifdef CFF_CONTROL_ADOBEINTERNALGETMETRICS
    int32_t r = prepare(inst, glyph, 5, &rp, transform, flags, 0, &d);
#else
    int32_t r = prepare(inst, glyph, 1, &rp, transform, flags, 0, &d);
#endif
    if (r) return r;
    cff_grow_buff *gb[9];
    grow_pointers(inst, gb);
    io.grow = gb;
    io.metrics = metrics;
    io.box = box;
    cff_path_procs procs = {NULL, NULL, NULL, NULL, NULL, NULL};
    io.flags18 = 0;
    io.zero28 = 0;
    wrp(inst, 0x610, NULL);
    r = ((consumer_fn)rdp(inst, 0x5f0))(inst + 0x9a0, inst + 0x610, &d, &io, &procs, proc_flags(flags), inst);
    if (r) return r;
    CFF_ORIG(post_render_fn, RVA_AdobePostRenderCommon)(glyph_metrics, &io, rd32(inst, 0x14));
    return 0;
}
CFF_EXPORT_STACK(cff_AdobeInternalGetMetrics, cff_AdobeInternalGetMetrics_impl);

/* AdobeInternalGetBitmap 0x8c528: CScan (proc +0x5e8, ATMBuildBitMap), or the QReducer
 * (ATMQRedCharBitMap) for bold glyphs above a size per overscale mode and as the fallback for
 * CScan errors of class 0xF9. Bold below those sizes: CScan's rows are widened to the right
 * by k pixels (notes/cff.md "Which scan converter draws a bitmap"), the box and the advance
 * by the largest k. Control: one extra pixel of widening per row. */

static int32_t bold_pixels(uint8_t *inst, uint32_t mode, int32_t dflt) {
    int32_t ppem = (int32_t)rd32(inst, 0xc48);
    fixmul_fn fixmul = CFF_ORIG(fixmul_fn, RVA_fixmul);
    int32_t n, lo, hi;
    switch (mode) {
    case 0: return 1;
    case 1: n = fixmul((int32_t)((uint32_t)ppem - 0x300000), 0xaaa), lo = 3, hi = 6; break;
    case 2: n = fixmul((int32_t)((uint32_t)ppem - 0x300000), 0x3552), lo = 15, hi = 30; break;
    case 3: n = fixmul((int32_t)((uint32_t)ppem - 0x400000), 0xaaa), lo = 4, hi = 8; break;
    case 4: n = fixmul((int32_t)((uint32_t)ppem - 0x200000), 0x2aaa), lo = 8, hi = 16; break;
    default: return dflt;
    }
    n = ((int32_t)((uint32_t)n + 0x8000) >> 16) + lo;
    if (n < lo) n = lo;
    if (n > hi) n = hi;
    return n;
}

static void widen(uint8_t *inst, uint32_t mode, cff_char_io *io, cff_bitmap_rec *bm) {
    int32_t n = (int32_t)rd32(inst, 0xf74);
    int32_t big, small, a;
    if (mode == 2) {
        big = (n + 4) / 5;
        small = n / 5;
        a = big * 5 - n;
    } else if (mode == 4) {
        big = (n + 3) / 4;
        small = n / 4;
        a = big * 4 - n;
    } else {
        big = small = a = n;
    }
    bm->width += (uint32_t)big;
    uint32_t rows = bm->rows;
    for (uint32_t row = 0; row < rows; row++) {
        int32_t k = big;
        if (mode == 2) {
            if (row / 5u < (row + (uint32_t)a) / 5u) k = small;
        } else if (mode == 4) {
            if ((row & ~3u) < ((row + (uint32_t)a) & ~3u)) k = small;
        }
#ifdef CFF_CONTROL_ADOBEINTERNALGETBITMAP
        k += 1;
#endif
        if (k <= 0) continue;
        for (int32_t j = (int32_t)((bm->width + 7) >> 3) - 1; j >= 0; j--) {
            uint8_t *p = bm->bits + (uint32_t)(row * bm->row_bytes + (uint32_t)j);
            uint8_t prev = j ? p[-1] : 0;
            uint8_t cur = *p;
            for (int32_t i = k; i; i--) {
                uint8_t c = (uint8_t)(cur * 2 | cur);
                cur = (prev & 0x80) ? (uint8_t)(c | 1) : c;
                prev = (uint8_t)(prev * 2);
            }
            *p = cur;
        }
    }
    if (rows) io->box[2] = (int32_t)((uint32_t)io->box[2] + ((uint32_t)big << 16));
    if (rd32(io->metrics, 0x20) != 0) wr32(io->metrics, 0x20, rd32(io->metrics, 0x20) + ((uint32_t)big << 16));
}

CFF_API int32_t cff_AdobeInternalGetBitmap_impl(uint8_t *inst, void *memory_fn, uint16_t glyph,
                                                const int32_t *transform, uint32_t mode, uint32_t flags,
                                                uint8_t *glyph_metrics, int32_t *out_row_bytes, uint8_t **bits,
                                                int32_t *size) {
    (void)memory_fn;
    cff_rend_prop rp;
    memset(&rp, 0, sizeof rp);
    cff_t1_char_data_desc d;
    memset(&d, 0, sizeof d);
    cff_char_io io;
    memset(&io, 0, sizeof io);
    uint8_t metrics[0x28];
    memset(metrics, 0, sizeof metrics);
    int32_t box[4] = {0, 0, 0, 0};
    cff_bitmap_rec bm;
    memset(&bm, 0, sizeof bm);
    if (glyph >= rd16(inst, 0x90)) return (int32_t)0xdd110302;
    int32_t r = prepare(inst, glyph, voverscale_of(mode), &rp, transform, flags, mode, &d);
    if (r) return r;
    cff_grow_buff *gb[9];
    grow_pointers(inst, gb);
    io.grow = gb;
    io.metrics = metrics;
    io.box = box;
    io.flags18 = 0;
    wrp(inst, 0x610, FN(RVA_GetBitmapMemory));
    bm.capacity = (uint32_t)*size;
    bm.bits = *bits;
    io.zero28 = 0;
    bm.row_bytes = 0;
    bm.zero28 = 0;
    if (rd32(inst, 0x14) != 0) io.flags18 = 0x2000;
    int qred = 0;
    int32_t bold = (flags & 2) != 0;
    if (bold) {
        int32_t ppem = (int32_t)(((uint32_t)rd32(inst, 0xc48) + 0x8000) & 0xffff0000u);
        switch (mode) {
        case 0: qred = ppem > 0x320000; break;
        case 1:
        case 2: qred = ppem > 0x12c0000; break;
        case 3: qred = ppem > 0x1900000; break;
        case 4: qred = ppem > 0xc80000; break;
        default: break;
        }
    }
    if (!qred) {
        int32_t n = (mode == 2 || mode == 4) ? -1 : 0;
        if (bold) n = bold_pixels(inst, mode, n);
        wr32(inst, 0xf74, (uint32_t)n);
        r = ((consumer_fn)rdp(inst, 0x5e8))(inst + 0x9a0, inst + 0x610, &d, &io, &bm, proc_flags(flags), inst);
        if (r != 0 && ((uint32_t)r & 0xff000000u) != 0xf9000000u) return r;
        if (rd32(inst, 0xf74) != 0 && r == 0) widen(inst, mode, &io, &bm);
        if (((uint32_t)r & 0xff000000u) == 0xf9000000u) qred = 1;
    }
    if (qred) {
        uint32_t saved = rd32(inst, 0xcdc);
        if (flags & 2) wr32(inst, 0xcdc, 7);
        uint8_t procs[0x38];
        memcpy(procs, inst + 0x5d8, sizeof procs);
        r = CFF_ORIG(qred_fn, RVA_ATMQRedCharBitMap)(inst + 0x9a0, inst + 0x610, &d, &io, &bm, inst, flags & 3, procs);
        wr32(inst, 0xcdc, saved);
        if (r) return r;
    }
    CFF_ORIG(post_render_fn, RVA_AdobePostRenderCommon)(glyph_metrics, &io, rd32(inst, 0x14));
    *out_row_bytes = (int32_t)bm.row_bytes;
    *size = (int32_t)bm.capacity;
    *bits = bm.bits;
    return 0;
}
CFF_EXPORT_STACK(cff_AdobeInternalGetBitmap, cff_AdobeInternalGetBitmap_impl);

/* ---------------------------------------------------------------------------------------- */
/* the proc table's memory sizes and the allocators */

/* ATMMemoryBufSize 0xe2610: the initial sizes of the nine grow buffers.
 * Control: buffer 0 starts at 0x10 bytes instead of 0x4000 (the consumers grow their
 * buffers, so only front_check.py sees it). */
CFF_API void cff_ATMMemoryBufSize_impl(int32_t *a, int32_t *b, int32_t *c, int32_t *d, int32_t *e, int32_t *f,
                                       int32_t *g, int32_t *h, int32_t *i) {
#ifdef CFF_CONTROL_ATMMEMORYBUFSIZE
    *a = 0x10;
#else
    *a = 0x4000;
#endif
    *b = 0x800;
    *c = 0xc00;
    *d = 0x1000;
    *e = 0xc00;
    *f = 0x400;
    *g = 0;
    *h = 0;
    *i = 0;
}
CFF_EXPORT_STACK(cff_ATMMemoryBufSize, cff_ATMMemoryBufSize_impl);

/* internalMalloc 0xef850, internalRealloc 0xe2bb0, internalFree 0xe9380: the zone argument
 * is ignored. Controls: malloc fails; realloc drops the old content; free leaks (no output
 * changes: front_check.py measures the heap). */
CFF_API void *cff_internalMalloc_impl(void *zone, size_t n) {
    (void)zone;
#ifdef CFF_CONTROL_INTERNALMALLOC
    (void)n;
    return NULL;
#else
    return crt_malloc(n);
#endif
}
CFF_EXPORT(cff_internalMalloc, cff_internalMalloc_impl);

CFF_API void *cff_internalRealloc_impl(void *zone, void *p, uint32_t n) {
    (void)zone;
#ifdef CFF_CONTROL_INTERNALREALLOC
    void *q = crt_malloc(n);
    if (q) memset(q, 0x5a, n);
    my_free(p);
    return q;
#else
    return crt_realloc(p, n);
#endif
}
CFF_EXPORT(cff_internalRealloc, cff_internalRealloc_impl);

CFF_API void cff_internalFree_impl(void *zone, void *p) {
    (void)zone;
#ifndef CFF_CONTROL_INTERNALFREE
    my_free(p);
#else
    (void)p;
#endif
}
CFF_EXPORT(cff_internalFree, cff_internalFree_impl);

/* ---------------------------------------------------------------------------------------- */
/* the outline client sink (AdobeInternalGetOutline's path procs): the context is the
 * CFFFontInst, the path goes into grow buffer 8 (+0x6c0 pointer, +0x6c8 size in bytes) as
 * int32 words, word 0 = the number of words used */

static int32_t *path_room(uint8_t *inst, int32_t words) {
    int32_t *buf = rdp(inst, 0x6c0);
    if (!buf) return NULL;
    if (buf[0] + words >= (int32_t)rd32(inst, 0x6c8) >> 2) {
        if (!CFF_ORIG(grow_path_fn, RVA_GrowPathBuffer)((cff_grow_buff *)(inst + 0x6c0))) return NULL;
        buf = rdp(inst, 0x6c0);
    }
    return buf;
}

/* CurveTo 0xe4a20. Control: the type word is 3 instead of 4. */
CFF_API uint32_t cff_CurveTo_impl(int32_t *cur, int32_t *p1, int32_t *p2, int32_t *p3, void *font, uint8_t *inst) {
    (void)cur, (void)font;
    int32_t *buf = path_room(inst, 7);
    if (!buf) return 0;
    int32_t at = buf[0];
#ifdef CFF_CONTROL_CURVETO
    buf[at] = 3;
#else
    buf[at] = 4;
#endif
    buf[at + 1] = p1[0];
    buf[at + 2] = p1[1];
    buf[at + 3] = p2[0];
    buf[at + 4] = p2[1];
    buf[at + 5] = p3[0];
    buf[at + 6] = p3[1];
    buf[0] += 7;
    return 1;
}
CFF_EXPORT_STACK(cff_CurveTo, cff_CurveTo_impl);

/* LineTo 0xe5170. Control: x is one unit larger. */
CFF_API uint32_t cff_LineTo_impl(int32_t *p, void *font, uint8_t *inst) {
    (void)font;
    int32_t *buf = path_room(inst, 3);
    if (!buf) return 0;
    int32_t at = buf[0];
    buf[at] = 3;
#ifdef CFF_CONTROL_LINETO
    buf[at + 1] = p[0] + 1;
#else
    buf[at + 1] = p[0];
#endif
    buf[at + 2] = p[1];
    buf[0] += 3;
    return 1;
}
CFF_EXPORT(cff_LineTo, cff_LineTo_impl);

/* MoveTo 0xeaca0. Control: y is one unit larger. */
CFF_API uint32_t cff_MoveTo_impl(int32_t *p, void *font, uint8_t *inst) {
    (void)font;
    int32_t *buf = path_room(inst, 3);
    if (!buf) return 0;
    int32_t at = buf[0];
    buf[at] = 2;
    buf[at + 1] = p[0];
#ifdef CFF_CONTROL_MOVETO
    buf[at + 2] = p[1] + 1;
#else
    buf[at + 2] = p[1];
#endif
    buf[0] += 3;
    return 1;
}
CFF_EXPORT(cff_MoveTo, cff_MoveTo_impl);

/* ClosePath 0xecac0. Control: nothing is recorded and the call fails. */
CFF_API uint32_t cff_ClosePath_impl(void *font, uint8_t *inst) {
    (void)font;
    int32_t *buf = path_room(inst, 1);
    if (!buf) return 0;
#ifdef CFF_CONTROL_CLOSEPATH
    return 0;
#endif
    buf[buf[0]] = 5;
    buf[0] += 1;
    return 1;
}
CFF_EXPORT(cff_ClosePath, cff_ClosePath_impl);

/* NewChar 0xede60: (re)allocates the path buffer to at least 0x2000 bytes and resets it to
 * the two header words {2, 1}. Control: the call fails after the reset. */
CFF_API uint32_t cff_NewChar_impl(void *font, uint8_t *inst) {
    (void)font;
    int32_t *buf = rdp(inst, 0x6c0);
    if (!buf || rd32(inst, 0x6c8) < 0x2000) {
        my_free(buf);
        buf = crt_malloc(0x2000);
        wrp(inst, 0x6c0, buf);
        wr32(inst, 0x6c8, 0x2000);
    }
    if (!buf) {
        wr32(inst, 0x6c8, 0);
        return 0;
    }
    buf[0] = 2;
    buf[1] = 1;
#ifdef CFF_CONTROL_NEWCHAR
    return 0;
#endif
    return 1;
}
CFF_EXPORT(cff_NewChar, cff_NewChar_impl);

/* GrowPathBuffer 0x182f5c: + 0x2000 bytes; on failure (or a size overflow) the buffer is freed.
 * Never reached by the corpus (no outline outgrows the first 0x2000 bytes). Control: + 0x1000. */
CFF_API uint32_t cff_GrowPathBuffer_impl(cff_grow_buff *b) {
#ifdef CFF_CONTROL_GROWPATHBUFFER
    uint32_t n = b->size + 0x1000;
#else
    uint32_t n = b->size + 0x2000;
#endif
    if (n >= b->size) {
        void *q = crt_realloc(b->ptr, n);
        if (q) {
            b->ptr = q;
            b->size = n;
            return 1;
        }
    }
    my_free(b->ptr);
    b->ptr = NULL;
    b->size = 0;
    return 0;
}
CFF_EXPORT(cff_GrowPathBuffer, cff_GrowPathBuffer_impl);

/* ---------------------------------------------------------------------------------------- */
/* ManageCString 0x182fc0 (the +0x620 proc, for seac): the glyph of a Standard Encoding code
 * (the u8 at +0x40 of the request) through the charset (+0x518, formats 0, 1, 2; identity
 * without one), then its charstring. Not for CID fonts (FDSelect set) nor when +0x464 != 0.
 * Never reached by the corpus (no seac). Control: format 1 ranges lose their last code. */

CFF_API uint8_t *cff_ManageCString_impl(uint32_t type, uint8_t *req, uint8_t *inst, uint8_t **end) {
    (void)type;
    get_one_fn get_one = CFF_ORIG(get_one_fn, RVA_AdobeCFFGetOne);
    uint8_t *start = NULL;
    uint8_t b48 = 0, b50 = 0;
    const uint8_t *lo = FONT_DATA(inst), *hi = FONT_END(inst);
    const uint8_t *cs = rdp(inst, 0x518);
    uint16_t code = g_std_encoding[(uint8_t)rd16(req, 0x40)];
    uint16_t gid = 0;
    if (!cs) {
        if (rd16(inst, 0x464) > 0) goto fail;
        gid = code;
    } else {
        if (rdp(inst, 0x538)) goto fail;
        if (code == 0) goto found;
        if (!get_one(&b48, cs, lo, hi)) goto fail;
        if (b48 == 0) {
            uint16_t count = rd16(inst, 0x90);
            gid = 1;
            for (int32_t i = 1; i < (int32_t)count; i++) {
                cs += 1;
                get_one(&b48, cs, lo, hi);
                uint16_t sid = (uint16_t)(b48 << 8);
                cs += 1;
                if (!get_one(&b48, cs, lo, hi)) {
                    gid = 0;
                    goto found;
                }
                sid |= b48;
                if (code == sid) goto found;
                gid = (uint16_t)(gid + 1);
                if (i == (int32_t)count - 1) gid = 0;
            }
        } else if (b48 == 1) {
            gid = 1;
            for (;;) {
                cs += 1;
                get_one(&b48, cs, lo, hi);
                uint16_t first = (uint16_t)(b48 << 8);
                cs += 1;
                get_one(&b48, cs, lo, hi);
                first |= b48;
                cs += 1;
                if (!get_one(&b50, cs, lo, hi)) {
                    gid = 0;
                    goto found;
                }
#ifdef CFF_CONTROL_MANAGECSTRING
                if (code >= first && (uint32_t)code < (uint32_t)b50 + first) {
#else
                if (code >= first && (uint32_t)code <= (uint32_t)b50 + first) {
#endif
                    gid = (uint16_t)(gid + (uint16_t)(code - first));
                    goto found;
                }
                gid = (uint16_t)(gid + (uint16_t)(b50 + 1));
                if (gid >= rd16(inst, 0x90)) {
                    gid = 0;
                    goto found;
                }
            }
        } else if (b48 == 2) {
            gid = 1;
            for (;;) {
                cs += 1;
                get_one(&b48, cs, lo, hi);
                uint16_t first = (uint16_t)(b48 << 8);
                cs += 1;
                get_one(&b48, cs, lo, hi);
                first |= b48;
                cs += 1;
                get_one(&b48, cs, lo, hi);
                uint16_t nleft = (uint16_t)(b48 << 8);
                cs += 1;
                if (!get_one(&b48, cs, lo, hi)) {
                    gid = 0;
                    goto found;
                }
                nleft |= b48;
                if (code >= first && (uint32_t)code <= (uint32_t)nleft + first) {
                    gid = (uint16_t)(gid + (uint16_t)(code - first));
                    goto found;
                }
                gid = (uint16_t)(gid + (uint16_t)(nleft + 1));
                if (gid >= rd16(inst, 0x90)) {
                    gid = 0;
                    goto found;
                }
            }
        } else {
            goto fail;
        }
    }
found:
    if (CFF_ORIG(get_cs_ptrs_fn, RVA_GetCharStringPointers)(gid, inst, &start, end) == 0) return start;
fail:
    *end = NULL;
    return NULL;
}
CFF_EXPORT(cff_ManageCString, cff_ManageCString_impl);

/* ATM_ManageCString 0x183230 (CScan's seac hook): ctx = {inst, procs}, calls procs->+0x10
 * (the ManageCString slot, CFFFontInst + 0x620) with the instance. Never reached by the
 * corpus. Control: the returned start is one byte late. */
CFF_API uint8_t *cff_ATM_ManageCString_impl(uint32_t type, void *req, void **ctx, uint8_t **end) {
    manage_cstring_fn f = (manage_cstring_fn)rdp(ctx[1], 0x10);
#ifdef CFF_CONTROL_ATM_MANAGECSTRING
    uint8_t *r = f(type, req, ctx[0], end);
    return r ? r + 1 : r;
#else
    return f(type, req, ctx[0], end);
#endif
}
CFF_EXPORT(cff_ATM_ManageCString, cff_ATM_ManageCString_impl);
