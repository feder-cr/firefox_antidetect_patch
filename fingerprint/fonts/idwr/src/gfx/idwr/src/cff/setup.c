/* CFF rasterizer, group "setup" (notes/cff.md group 5): the per-size setup of the ATM font
 * state, the device matrix and its transform procs, blue zones, stem snap, the slope tables
 * of the hinted transform (Map / TfmLockPt*) and the floor/round modulo helpers. Ported
 * from the assembly of DWrite.dll 10.0.26100.9711; the four math leaves (fixmul, fixdiv,
 * fracmul, fixratio) are in cff_math.c.
 *
 *   ATMSetUpValues      0x1800b01c0  proc table +0x5d8: BCSetUpValues with ATMSetUpMtx
 *   BCSetUpValues       0x1800b1cd4  the whole per-size setup of the state
 *   ATMSetUpMtx         0x1800b3890  device matrix: range check, axis swap and sign flags
 *   BCSetMtx            0x1800b24ec  installs the matrix record and the four transform procs
 *   FixMtxInvert        0x1800b25f0  2x3 inverse
 *   InvertBody          0x1800b26a4  2x2 inverse (recursive with the rows swapped)
 *   BCCnctMtx           0x18008d33c  matrix product in 2.30 (fracmul)
 *   BCTfmP              0x1800b2460  point through the device matrix, then erectToDev
 *   d_bcz_fixed_tfm     0x1800cf950  diagonal transform (fixmul inlined)
 *   inv_d_bcz_fixed_tfm 0x1800b3300  diagonal inverse
 *   d_gen_fixed_tfm     0x1800e45f0  general transform
 *   inv_d_gen_fixed_tfm 0x1800ebc30  general inverse
 *   SetUpBlueValues     0x1800afcf8  blue zones, family blues
 *   BlueBoostSetUp      0x1800affe0  blue boost below 0x23 units, then BlueBoost (hint)
 *   FntTfmY             0x1800affa4  y through the transform proc
 *   SetUpBBox           0x1800b3a10  font bbox widened by the em size
 *   TfmBBox             0x1800b3da0  bbox through the transform, flags undone
 *   TfmWidths           0x1800b4768  stem widths through the transform
 *   SetUpStemSnap       0x1800b3c98  StdHW/StdVW and the snap tables
 *   SetUpStemW          0x1800b4118  one direction of the stem snap tables
 *   SortStems           0x1800b3af0  sorts StemSnap arrays, drops duplicates and <= 0
 *   AdjustBlueScale     0x1800b3bd0  BlueScale against the tallest zone
 *   LinesPerEm          0x1800b3c34  ppem in x and y
 *   GetEmSize           0x1800b3788  em size from the font matrix (sqrt path)
 *   ApproxDLen          0x1800b4864  approximate length of a transformed vector
 *   Map                 0x1800b326c  piecewise linear map through a slope table
 *   InitSlopes          0x1800b33a8  builds the x and y slope tables
 *   initslope           0x1800b300c  one slope table
 *   fixupmap            0x1801849dc  separates hinted stems that came too close
 *   TfmLockPt_noTfm     0x1800b31f0  hinted point, no transform
 *   TfmLockPt1          0x1800e0a80  hinted point, then the transform proc
 *   TfmLockPt3          0x1800e1320  transform proc only
 *   FFloorFModN         0x1800b1c1c  floor modulo n (fixmul inlined in one branch)
 *   FFloorFModNR        0x1800b1bdc  floor modulo n with an offset
 *   FRoundFModNR        0x1800afc98  round modulo n with an offset
 *
 * The ATM font state (`st`, CFFFontInst + 0x9a0) is accessed at its raw offsets; the fields
 * this group writes are listed in notes/cff.md. Every call to another function, inside or
 * outside the group, goes through CFF_ORIG (the original entry, hooked or not), except where
 * the original inlined the callee (fixmul in FFloorFModN and d_bcz_fixed_tfm). Function
 * addresses stored into the state (the transform procs, TfmLockPt at +0x428, ATMSetUpMtx
 * passed to BCSetUpValues) are the original entries, idwr_orig(rva), so comparisons against
 * them in the original code keep working.
 *
 * Negative controls: -DCFF_CONTROL_<NAME> (the function name in upper case, e.g.
 * CFF_CONTROL_SETUPSTEMW) breaks the port of that function; one per function.
 */
#include "cff_port.h"

#ifdef IDWR_STANDALONE
#define cff_ATMSetUpValues_impl cff_ATMSetUpValues
#define cff_BCSetUpValues_impl cff_BCSetUpValues
#define cff_ATMSetUpMtx_impl cff_ATMSetUpMtx
#define cff_BCSetMtx_impl cff_BCSetMtx
#define cff_FixMtxInvert_impl cff_FixMtxInvert
#define cff_InvertBody_impl cff_InvertBody
#define cff_BCCnctMtx_impl cff_BCCnctMtx
#define cff_BCTfmP_impl cff_BCTfmP
#define cff_d_bcz_fixed_tfm_impl cff_d_bcz_fixed_tfm
#define cff_inv_d_bcz_fixed_tfm_impl cff_inv_d_bcz_fixed_tfm
#define cff_d_gen_fixed_tfm_impl cff_d_gen_fixed_tfm
#define cff_inv_d_gen_fixed_tfm_impl cff_inv_d_gen_fixed_tfm
#define cff_SetUpBlueValues_impl cff_SetUpBlueValues
#define cff_BlueBoostSetUp_impl cff_BlueBoostSetUp
#define cff_FntTfmY_impl cff_FntTfmY
#define cff_SetUpBBox_impl cff_SetUpBBox
#define cff_TfmBBox_impl cff_TfmBBox
#define cff_TfmWidths_impl cff_TfmWidths
#define cff_SetUpStemSnap_impl cff_SetUpStemSnap
#define cff_SetUpStemW_impl cff_SetUpStemW
#define cff_SortStems_impl cff_SortStems
#define cff_AdjustBlueScale_impl cff_AdjustBlueScale
#define cff_LinesPerEm_impl cff_LinesPerEm
#define cff_GetEmSize_impl cff_GetEmSize
#define cff_ApproxDLen_impl cff_ApproxDLen
#define cff_Map_impl cff_Map
#define cff_InitSlopes_impl cff_InitSlopes
#define cff_initslope_impl cff_initslope
#define cff_fixupmap_impl cff_fixupmap
#define cff_TfmLockPt_noTfm_impl cff_TfmLockPt_noTfm
#define cff_TfmLockPt1_impl cff_TfmLockPt1
#define cff_TfmLockPt3_impl cff_TfmLockPt3
#define cff_FFloorFModN_impl cff_FFloorFModN
#define cff_FFloorFModNR_impl cff_FFloorFModNR
#define cff_FRoundFModNR_impl cff_FRoundFModNR
#endif

/* CFF_EXPORT_STK: CFF_EXPORT for functions with more than four arguments. The thunk of
 * cff_port.h puts its own frame between the caller and the port, so the port would read its
 * fifth and later arguments from the wrong slots; this variant also copies six stack
 * argument slots (enough for BCSetUpValues' nine arguments) below its frame. It saves the
 * same registers (RCX, RDX, R8-R11, XMM1-XMM5). */
#ifdef IDWR_STANDALONE
#define CFF_EXPORT_STK(name, impl) CFF_EXPORT(name, impl)
#else
#define CFF_EXPORT_STK(name, impl)                                                     \
    __asm__(".text\n"                                                                  \
            ".globl " #name "\n"                                                       \
            ".def " #name "; .scl 2; .type 32; .endef\n"                               \
            ".p2align 4\n" #name ":\n"                                                 \
            ".seh_proc " #name "\n"                                                    \
            "pushq %rcx\n.seh_pushreg %rcx\n"                                          \
            "pushq %rdx\n.seh_pushreg %rdx\n"                                          \
            "pushq %r8\n.seh_pushreg %r8\n"                                            \
            "pushq %r9\n.seh_pushreg %r9\n"                                            \
            "pushq %r10\n.seh_pushreg %r10\n"                                          \
            "pushq %r11\n.seh_pushreg %r11\n"                                          \
            "subq $0xa8, %rsp\n.seh_stackalloc 0xa8\n"                                 \
            "movaps %xmm1, 0x50(%rsp)\n.seh_savexmm %xmm1, 0x50\n"                     \
            "movaps %xmm2, 0x60(%rsp)\n.seh_savexmm %xmm2, 0x60\n"                     \
            "movaps %xmm3, 0x70(%rsp)\n.seh_savexmm %xmm3, 0x70\n"                     \
            "movaps %xmm4, 0x80(%rsp)\n.seh_savexmm %xmm4, 0x80\n"                     \
            "movaps %xmm5, 0x90(%rsp)\n.seh_savexmm %xmm5, 0x90\n"                     \
            ".seh_endprologue\n"                                                       \
            "movq 0x100(%rsp), %rax\nmovq %rax, 0x20(%rsp)\n"                          \
            "movq 0x108(%rsp), %rax\nmovq %rax, 0x28(%rsp)\n"                          \
            "movq 0x110(%rsp), %rax\nmovq %rax, 0x30(%rsp)\n"                          \
            "movq 0x118(%rsp), %rax\nmovq %rax, 0x38(%rsp)\n"                          \
            "movq 0x120(%rsp), %rax\nmovq %rax, 0x40(%rsp)\n"                          \
            "movq 0x128(%rsp), %rax\nmovq %rax, 0x48(%rsp)\n"                          \
            "callq " #impl "\n"                                                        \
            "movaps 0x50(%rsp), %xmm1\n"                                               \
            "movaps 0x60(%rsp), %xmm2\n"                                               \
            "movaps 0x70(%rsp), %xmm3\n"                                               \
            "movaps 0x80(%rsp), %xmm4\n"                                               \
            "movaps 0x90(%rsp), %xmm5\n"                                               \
            "addq $0xa8, %rsp\n"                                                       \
            "popq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n"         \
            "retq\n"                                                                   \
            ".seh_endproc\n"                                                           \
            ".section .drectve,\"yn\"\n"                                               \
            ".ascii \" -export:" #name "\"\n"                                          \
            ".text\n")
#endif

/* ---- raw field access (may_alias: the state is read and written with several widths) ---- */
typedef int32_t __attribute__((may_alias)) i32a;
typedef uint32_t __attribute__((may_alias)) u32a;
typedef uint16_t __attribute__((may_alias)) u16a;
typedef uint8_t __attribute__((may_alias)) u8a;
typedef uint64_t __attribute__((may_alias)) u64a;
typedef uint8_t *__attribute__((may_alias)) pa;

#define I32(p, o) (*(i32a *)((uint8_t *)(p) + (o)))
#define U32(p, o) (*(u32a *)((uint8_t *)(p) + (o)))
#define U16(p, o) (*(u16a *)((uint8_t *)(p) + (o)))
#define U8(p, o) (*(u8a *)((uint8_t *)(p) + (o)))
#define U64(p, o) (*(u64a *)((uint8_t *)(p) + (o)))
#define PTR(p, o) (*(pa *)((uint8_t *)(p) + (o)))

/* a point passed by value in one 64-bit register: x in the low half, y in the high half */
#define PT(x, y) ((uint64_t)(uint32_t)(x) | ((uint64_t)(uint32_t)(y) << 32))
#define PTX(p) ((int32_t)(uint32_t)(p))
#define PTY(p) ((int32_t)(uint32_t)((p) >> 32))

static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t neg32(int32_t a) { return (int32_t)(0u - (uint32_t)a); }
static inline int32_t abs32(int32_t v) { return v < 0 ? neg32(v) : v; } /* NEG/CMOVS: INT_MIN stays */
static inline int32_t sar32(int32_t v, int n) { return v >> n; }       /* arithmetic (clang, gcc) */

/* ---- the originals this group calls ---- */
#define RVA_fixmul 0xb3340
#define RVA_fixdiv 0xb316c
#define RVA_fracmul 0xb34d8
#define RVA_fixratio 0xb3544
#define RVA_erectToDev 0xd9118
#define RVA_BlueBoost 0xb0098
#define RVA_ATMSetUpValues 0xb01c0
#define RVA_BCSetUpValues 0xb1cd4
#define RVA_ATMSetUpMtx 0xb3890
#define RVA_BCSetMtx 0xb24ec
#define RVA_FixMtxInvert 0xb25f0
#define RVA_InvertBody 0xb26a4
#define RVA_d_bcz_fixed_tfm 0xcf950
#define RVA_inv_d_bcz_fixed_tfm 0xb3300
#define RVA_d_gen_fixed_tfm 0xe45f0
#define RVA_inv_d_gen_fixed_tfm 0xebc30
#define RVA_SetUpBlueValues 0xafcf8
#define RVA_BlueBoostSetUp 0xaffe0
#define RVA_FntTfmY 0xaffa4
#define RVA_SetUpBBox 0xb3a10
#define RVA_TfmBBox 0xb3da0
#define RVA_TfmWidths 0xb4768
#define RVA_SetUpStemSnap 0xb3c98
#define RVA_SetUpStemW 0xb4118
#define RVA_SortStems 0xb3af0
#define RVA_AdjustBlueScale 0xb3bd0
#define RVA_LinesPerEm 0xb3c34
#define RVA_GetEmSize 0xb3788
#define RVA_ApproxDLen 0xb4864
#define RVA_Map 0xb326c
#define RVA_InitSlopes 0xb33a8
#define RVA_initslope 0xb300c
#define RVA_fixupmap 0x1849dc
#define RVA_TfmLockPt_noTfm 0xb31f0
#define RVA_TfmLockPt1 0xe0a80
#define RVA_FFloorFModN 0xb1c1c
#define RVA_FFloorFModNR 0xb1bdc

typedef int32_t (*ii_fn)(int32_t, int32_t);
typedef void (*tfm_fn)(uint64_t pt, int32_t *out, uint8_t *st); /* the procs at st + 0x580..0x598 */

static inline int32_t fixmul(int32_t a, int32_t b) { return CFF_ORIG(ii_fn, RVA_fixmul)(a, b); }
static inline int32_t fixdiv(int32_t a, int32_t b) { return CFF_ORIG(ii_fn, RVA_fixdiv)(a, b); }
static inline int32_t fracmul(int32_t a, int32_t b) { return CFF_ORIG(ii_fn, RVA_fracmul)(a, b); }
static inline int32_t fixratio(int32_t a, int32_t b) { return CFF_ORIG(ii_fn, RVA_fixratio)(a, b); }

/* the fixmul body as the original inlines it (identical to cff_fixmul) */
static inline int32_t fixmul_inl(int32_t a, int32_t b) {
    double x = (double)a * (double)b * (1.0 / 65536.0);
    if (x < 0.0) {
        x += -0.5;
        return x > -2147483648.0 ? (int32_t)x : (int32_t)0x80000000;
    }
    x += 0.5;
    return 2147483647.0 > x ? (int32_t)x : 0x7fffffff;
}

static inline void tfm_call(uint8_t *st, int off, uint64_t pt, int32_t *out) {
    tfm_fn f = (tfm_fn)PTR(st, off);
    f(pt, out, st);
}
static inline uint64_t ld_pt(const int32_t *p) { return PT(p[0], p[1]); }

static inline int32_t ApproxDLen(int32_t *pt, uint8_t *st) {
    return CFF_ORIG(int32_t (*)(int32_t *, uint8_t *), RVA_ApproxDLen)(pt, st);
}
static inline int32_t Map(uint8_t *sl, int32_t v) {
    return CFF_ORIG(int32_t (*)(uint8_t *, int32_t), RVA_Map)(sl, v);
}
static inline void InitSlopes(int32_t sx, int32_t sy, uint8_t *st) {
    CFF_ORIG(void (*)(int32_t, int32_t, uint8_t *), RVA_InitSlopes)(sx, sy, st);
}
static inline int32_t FFloorFModN(int32_t x, uint32_t n) {
    return CFF_ORIG(int32_t (*)(int32_t, uint32_t), RVA_FFloorFModN)(x, n);
}

/* ======================================================================================== */
/* floor / round modulo n                                                                  */

/* FFloorFModN(x, n): the largest multiple of n (16.16, n an integer in dx) at or below x */
CFF_API int32_t cff_FFloorFModN_impl(int32_t x, uint32_t n) {
    uint32_t nn = n & 0xffff, half = nn >> 1;
    int32_t nf = (int32_t)(nn << 16);
    if (x > 0) {
        int32_t q = (int32_t)((uint32_t)fixdiv(sub32(x, (int32_t)half), nf) & 0xffff0000u);
#ifdef CFF_CONTROL_FFLOORFMODN
        /* one step lower: still at or below x (a floor above x overruns the bitmap) */
        return fixmul_inl(q, nf) - nf;
#endif
        return fixmul_inl(q, nf);
    }
    int32_t t = (int32_t)((nn << 16) - half - (uint32_t)x - 1u);
    int32_t q = (int32_t)((uint32_t)fixdiv(t, nf) & 0xffff0000u);
    return neg32(fixmul(q, nf));
}
CFF_EXPORT(cff_FFloorFModN, cff_FFloorFModN_impl);

CFF_API int32_t cff_FFloorFModNR_impl(int32_t x, uint32_t n, uint32_t off) {
    uint32_t nn = n & 0xffff;
    int32_t r = add32((int32_t)((uint32_t)(int32_t)(int16_t)off << 16), FFloorFModN(x, nn));
#ifdef CFF_CONTROL_FFLOORFMODNR
    r += 1;
#endif
    if (r > x)
        r = sub32(r, (int32_t)(nn << 16));
    return r;
}
CFF_EXPORT(cff_FFloorFModNR, cff_FFloorFModNR_impl);

CFF_API int32_t cff_FRoundFModNR_impl(int32_t x, uint32_t n, uint32_t off) {
    uint32_t nn = n & 0xffff;
    int32_t r = CFF_ORIG(int32_t (*)(int32_t, uint32_t, uint32_t), RVA_FFloorFModNR)(add32((int32_t)(nn << 15), x), nn, off);
    int32_t nf = (int32_t)(nn << 16);
    if (r <= x) {
        int32_t d = sub32(x, r);
        if (sub32(nf, d) < d)
            r = add32(r, nf);
    } else {
        int32_t c = sub32(r, x);
        if (sub32(nf, c) < c)
            r = sub32(r, nf);
    }
#ifdef CFF_CONTROL_FROUNDFMODNR
    r = add32(r, 0x4000);
#endif
    return r;
}
CFF_EXPORT(cff_FRoundFModNR, cff_FRoundFModNR_impl);

/* ======================================================================================== */
/* the transform procs: (point, out, state), the matrix record at *(st + 0x578):            */
/* +0 type of the matrix (1 diagonal, 3 general), +4 type of the inverse, +8..+0x1c the      */
/* matrix (a, b, c, d, tx, ty), +0x20..+0x34 the inverse                                   */

CFF_API void cff_d_bcz_fixed_tfm_impl(uint64_t pt, int32_t *out, uint8_t *st) {
    uint8_t *m = PTR(st, 0x578);
    int32_t x = fixmul_inl(I32(m, 8), PTX(pt));
    int32_t y = fixmul_inl(I32(m, 0x14), PTY(pt));
#ifdef CFF_CONTROL_D_BCZ_FIXED_TFM
    y += 1;
#endif
    out[0] = x;
    out[1] = y;
}
CFF_EXPORT(cff_d_bcz_fixed_tfm, cff_d_bcz_fixed_tfm_impl);

CFF_API void cff_inv_d_bcz_fixed_tfm_impl(uint64_t pt, int32_t *out, uint8_t *st) {
    uint8_t *m = PTR(st, 0x578);
    int32_t y = fixmul(PTY(pt), I32(m, 0x2c));
    int32_t x = fixmul(PTX(pt), I32(m, 0x20));
#ifdef CFF_CONTROL_INV_D_BCZ_FIXED_TFM
    x += 1;
#endif
    out[0] = x;
    out[1] = y;
}
CFF_EXPORT(cff_inv_d_bcz_fixed_tfm, cff_inv_d_bcz_fixed_tfm_impl);

CFF_API void cff_d_gen_fixed_tfm_impl(uint64_t pt, int32_t *out, uint8_t *st) {
    uint8_t *m = PTR(st, 0x578);
    int32_t x = PTX(pt), y = PTY(pt);
    int32_t rx = fixmul(y, I32(m, 0x10));
    rx = add32(rx, fixmul(x, I32(m, 8)));
    int32_t ry = fixmul(y, I32(m, 0x14));
    ry = add32(ry, fixmul(x, I32(m, 0xc)));
#ifdef CFF_CONTROL_D_GEN_FIXED_TFM
    ry += 1;
#endif
    out[0] = rx;
    out[1] = ry;
}
CFF_EXPORT(cff_d_gen_fixed_tfm, cff_d_gen_fixed_tfm_impl);

CFF_API void cff_inv_d_gen_fixed_tfm_impl(uint64_t pt, int32_t *out, uint8_t *st) {
    uint8_t *m = PTR(st, 0x578);
    int32_t x = PTX(pt), y = PTY(pt);
    int32_t rx = fixmul(y, I32(m, 0x28));
    rx = add32(rx, fixmul(x, I32(m, 0x20)));
    int32_t ry = fixmul(y, I32(m, 0x2c));
    ry = add32(ry, fixmul(x, I32(m, 0x24)));
#ifdef CFF_CONTROL_INV_D_GEN_FIXED_TFM
    rx += 0x8000;
#endif
    out[0] = rx;
    out[1] = ry;
}
CFF_EXPORT(cff_inv_d_gen_fixed_tfm, cff_inv_d_gen_fixed_tfm_impl);

/* BCTfmP(state, point, out): the point through the device matrix at st + 8 (with its
 * translation), then erectToDev (interp), which undoes the sign/swap flags of +0x2bc */
CFF_API uint64_t cff_BCTfmP_impl(uint8_t *st, uint64_t pt, int32_t *out) {
    int32_t x = PTX(pt), y = PTY(pt);
    int32_t rx = fixmul(y, I32(st, 0x10));
    rx = add32(rx, fixmul(x, I32(st, 8)));
    rx = add32(rx, I32(st, 0x18));
    int32_t ry = fixmul(y, I32(st, 0x14));
    ry = add32(ry, fixmul(x, I32(st, 0xc)));
    ry = add32(ry, I32(st, 0x1c));
#ifdef CFF_CONTROL_BCTFMP
    rx += 0x8000;
    ry += 0x8000;
#endif
    return CFF_ORIG(uint64_t (*)(uint64_t, int32_t *, uint8_t *), RVA_erectToDev)(PT(rx, ry), out, st);
}
CFF_EXPORT(cff_BCTfmP, cff_BCTfmP_impl);

/* ======================================================================================== */
/* matrices                                                                                */

CFF_API int32_t cff_InvertBody_impl(int32_t *m, int32_t *out) {
    int32_t a = m[0], c = m[2];
    if (abs32(a) < abs32(c)) {
        /* rows swapped, inverted, then the columns of the result swapped back */
        int32_t loc[6] = {c, m[3], a, m[1], 0, 0};
        if (!CFF_ORIG(int32_t (*)(int32_t *, int32_t *), RVA_InvertBody)(loc, out))
            return 0;
        int32_t t = out[0];
        out[0] = out[1];
        out[1] = t;
        t = out[2];
        out[2] = out[3];
        out[3] = t;
        return 1;
    }
    int32_t r8 = fixratio(c, a);
    if (r8 == 0x7fffffff || r8 == (int32_t)0x80000000)
        return 0;
    int32_t e = sub32(m[3], fracmul(m[1], r8));
    if (abs32(e) < 0x10000) {
        int32_t q = fixdiv(0x10000, e);
        out[3] = q;
        if (q == (int32_t)0x80000000 || q == 0x7fffffff)
            return 0;
        out[2] = neg32(fracmul(r8, q));
    } else {
        int32_t q = fixratio(0x10000, e);
        if (q == 0x7fffffff || q == (int32_t)0x80000000)
            return 0;
        out[3] = sar32(add32(q, 0x2000), 14);
        int32_t v = neg32(fracmul(r8, q));
        out[2] = v;
        out[2] = sar32(add32(v, 0x2000), 14);
    }
    int32_t aa = abs32(m[0]);
    int32_t o3 = out[3];
    int32_t res;
    if (abs32(m[1]) < aa) {
        int32_t q = fixratio(m[1], m[0]);
        res = fracmul(o3, q);
    } else if (abs32(o3) < aa) {
        int32_t q = fixratio(o3, m[0]);
        res = fracmul(m[1], q);
    } else {
        int32_t q = fixdiv(m[1], m[0]);
        res = fixmul(o3, q);
        out[1] = res;
        if (res == 0x7fffffff || res == (int32_t)0x80000000)
            return 0;
    }
    int32_t o2 = out[2];
    out[1] = neg32(res);
#ifdef CFF_CONTROL_INVERTBODY
    out[1] += 1;
#endif
    int32_t b = m[1];
    int32_t p = fixmul(b, o2);
    if (p == 0x7fffffff || p == (int32_t)0x80000000) {
        int32_t x, y;
        if (abs32(b) <= abs32(o2)) {
            y = b;
            x = out[2];
        } else {
            y = out[2];
            x = b;
        }
        int32_t q = fixdiv(x, m[0]);
        int32_t v = fixmul(q, y);
        out[0] = v;
        if (v == 0x7fffffff || v == (int32_t)0x80000000)
            return 0;
        out[0] = neg32(v);
        return 1;
    }
    int32_t q = fixdiv(sub32(0x10000, p), m[0]);
    out[0] = q;
    if (q == (int32_t)0x80000000 || q == 0x7fffffff)
        return 0;
    return 1;
}
CFF_EXPORT(cff_InvertBody, cff_InvertBody_impl);

static inline int32_t ovf(int32_t v) { return v == 0x7fffffff || v == (int32_t)0x80000000; }

CFF_API int32_t cff_FixMtxInvert_impl(int32_t *m, int32_t *out) {
    if (!CFF_ORIG(int32_t (*)(int32_t *, int32_t *), RVA_InvertBody)(m, out))
        return 0;
    int32_t r9 = neg32(fixmul(m[4], out[0]));
    if (ovf(r9))
        return 0;
    r9 = sub32(r9, fixmul(m[5], out[2]));
    if (ovf(r9))
        return 0;
    int32_t r8 = neg32(fixmul(m[4], out[1]));
    if (ovf(r8))
        return 0;
    r8 = sub32(r8, fixmul(m[5], out[3]));
    if (ovf(r8))
        return 0;
#ifdef CFF_CONTROL_FIXMTXINVERT
    r9 += 0x8000;
    out[0] += 0x1000;
#endif
    out[4] = r9;
    out[5] = r8;
    return 1;
}
CFF_EXPORT(cff_FixMtxInvert, cff_FixMtxInvert_impl);

/* BCSetMtx(matrix, state): copies the 2x3 matrix into the record at st + 0x540, inverts it
 * and installs the transform procs: +0x580/+0x590 forward, +0x588/+0x598 inverse */
CFF_API int32_t cff_BCSetMtx_impl(int32_t *src, uint8_t *st) {
    if (!src)
        return 0;
    uint8_t *rec = st + 0x540;
    PTR(st, 0x578) = rec;
    U64(rec, 8) = U64(src, 0);
    U64(rec, 0x10) = U64(src, 8);
    U64(rec, 0x18) = U64(src, 0x10);
    uint8_t *m = PTR(st, 0x578);
    int32_t type = 3;
    if (I32(m, 0xc) == 0 && I32(m, 0x10) == 0)
        type = 1;
#ifdef CFF_CONTROL_BCSETMTX
    type = 3;
#endif
    I32(m, 0) = type;
    m = PTR(st, 0x578);
    if (!CFF_ORIG(int32_t (*)(uint8_t *, uint8_t *), RVA_FixMtxInvert)(m + 8, m + 0x20))
        return (int32_t)0xfd050048;
    m = PTR(st, 0x578);
    I32(m, 4) = (I32(m, 0x24) != 0 || I32(m, 0x28) != 0) ? 3 : 1;
    m = PTR(st, 0x578);
    void *f = idwr_orig(I32(m, 0) == 1 ? RVA_d_bcz_fixed_tfm : RVA_d_gen_fixed_tfm);
    PTR(st, 0x590) = f;
    PTR(st, 0x580) = f;
    void *g = idwr_orig(I32(m, 4) == 1 ? RVA_inv_d_bcz_fixed_tfm : RVA_inv_d_gen_fixed_tfm);
    PTR(st, 0x598) = g;
    PTR(st, 0x588) = g;
    return 0;
}
CFF_EXPORT(cff_BCSetMtx, cff_BCSetMtx_impl);

/* BCCnctMtx(A, B, out): out = A x B in 2.30 (fracmul), with the translation of B added */
CFF_API void cff_BCCnctMtx_impl(int32_t *A, int32_t *B, int32_t *out) {
    if (A[1] == 0 && A[2] == 0 && B[1] == 0 && B[2] == 0) {
        out[0] = fracmul(B[0], A[0]);
        U64(out, 4) = 0;
        out[3] = fracmul(B[3], A[3]);
        if (A[4] == 0 && A[5] == 0) {
            out[4] = B[4];
            out[5] = B[5];
        } else {
            out[4] = add32(fracmul(B[0], A[4]), B[4]);
            int32_t t = fracmul(B[3], A[5]);
            out[5] = add32(t, B[5]);
        }
#ifdef CFF_CONTROL_BCCNCTMTX
        out[0] += 1;
#endif
        return;
    }
    int32_t t = fracmul(B[2], A[1]);
    out[0] = add32(t, fracmul(B[0], A[0]));
    t = fracmul(B[3], A[1]);
    out[1] = add32(t, fracmul(B[1], A[0]));
    t = fracmul(B[2], A[3]);
    out[2] = add32(t, fracmul(B[0], A[2]));
    t = fracmul(B[1], A[2]);
    out[3] = add32(t, fracmul(B[3], A[3]));
    t = fracmul(B[2], A[5]);
    out[4] = add32(add32(t, fracmul(B[0], A[4])), B[4]);
    t = fracmul(B[1], A[4]);
    out[5] = add32(add32(t, fracmul(B[3], A[5])), B[5]);
#ifdef CFF_CONTROL_BCCNCTMTX
    out[0] += 1;
#endif
}
CFF_EXPORT(cff_BCCnctMtx, cff_BCCnctMtx_impl);

/* ATMSetUpMtx(src 2x2, em size, out 2x3, -, *c_is_zero, *b_is_zero, *flags, state):
 * checks the range against 0x7fffffff / (2.5 em), swaps the axes when a or d is zero (flag
 * bit 16), makes a and d positive (bits 19 and 22), then BCSetMtx */
CFF_API int32_t cff_ATMSetUpMtx_impl(int32_t *src, int32_t em, int32_t *out, void *unused, int32_t *p5, int32_t *p6,
                                     uint32_t *flags, uint8_t *st) {
    (void)unused;
    U64(out, 0) = U64(src, 0);
    U64(out, 8) = U64(src, 8);
    U64(out, 0x10) = 0;
    int32_t lim = fixdiv(0x7fffffff, fixmul(em, 0x28000));
    int32_t r11 = out[0];
    const int32_t MIN = (int32_t)0x80000000;
    if (r11 == MIN || out[1] == MIN || out[2] == MIN || out[3] == MIN)
        return (int32_t)0xf90205f9;
    int32_t edx = out[1];
    if (abs32(r11) >= sub32(lim, abs32(edx)))
        return (int32_t)0xf90205f9;
    lim = sub32(lim, abs32(out[2]));
    if (abs32(out[3]) >= lim)
        return (int32_t)0xf90205f9;
    if (r11 == 0 || out[3] == 0) {
        *flags |= 1u << 16;
        int32_t o0 = out[0], o1 = out[1], o2 = out[2], o3 = out[3], o4 = out[4], o5 = out[5];
        out[3] = o2;
        out[2] = o3;
        out[4] = o5;
        out[5] = o4;
        out[0] = o1;
        out[1] = o0;
        r11 = o1;
        edx = o0;
    }
    if (r11 < 0) {
        *flags |= 1u << 19;
        out[0] = neg32(out[0]);
        edx = out[1];
        out[2] = neg32(out[2]);
        out[4] = neg32(out[4]);
    }
    if (out[3] < 0) {
        *flags |= 1u << 22;
        out[3] = neg32(out[3]);
        edx = neg32(out[1]);
        out[1] = edx;
        out[5] = neg32(out[5]);
    }
    *p6 = edx == 0;
    *p5 = out[2] == 0;
#ifdef CFF_CONTROL_ATMSETUPMTX
    *p6 = *p5 = 0; /* reported as skewed: no hinting */
#endif
    return CFF_ORIG(int32_t (*)(int32_t *, uint8_t *), RVA_BCSetMtx)(out, st);
}
CFF_EXPORT_STK(cff_ATMSetUpMtx, cff_ATMSetUpMtx_impl);

/* GetEmSize(2x2 matrix, *out): 1/scale in 16.16 from the font matrix */
CFF_API void cff_GetEmSize_impl(int32_t *m, int32_t *out) {
    int32_t a = abs32(m[0]), b = abs32(m[1]), c = abs32(m[2]), d = abs32(m[3]);
    int32_t x = a, y = d, r = 0;
    if (b != 0 && c != 0) {
        if (a == 0 || d == 0) {
            x = b;
            y = c;
        } else {
            if (a >= 0x40000000 || b >= 0x40000000 || c >= 0x40000000 || d >= 0x40000000) {
                *out = 0x3e80000;
                return;
            }
            int32_t p = fracmul(a, d);
            p = sub32(p, fracmul(b, c));
            double s = __builtin_sqrt((double)abs32(p) * (1.0 / 1073741824.0));
            x = (int32_t)(int64_t)(s * 1073741824.0 + 0.5);
            y = 0; /* the original compares against 0 here: this path always gives 0 */
        }
    }
    if (y < x) {
        if (y != 0)
            r = fixdiv(0x40000000, y);
    } else if (x != 0) {
        r = fixdiv(0x40000000, x);
    }
#ifdef CFF_CONTROL_GETEMSIZE
    r >>= 3;
#endif
    *out = r;
}
CFF_EXPORT(cff_GetEmSize, cff_GetEmSize_impl);

/* ======================================================================================== */
/* lengths and widths through the transform                                                */

CFF_API int32_t cff_ApproxDLen_impl(int32_t *pt, uint8_t *st) {
    tfm_call(st, 0x590, ld_pt(pt), pt);
    int32_t x = pt[0];
    if (x < 0) {
        x = neg32(x);
        pt[0] = x;
    }
    int32_t y = pt[1];
    if (y < 0) {
        y = neg32(y);
        pt[1] = y;
    }
#ifdef CFF_CONTROL_APPROXDLEN
    y += 0x8000;
#endif
    if (x < y)
        return add32(fixmul(x, 0x561e), y);
    return add32(fixmul(y, 0x561e), x);
}
CFF_EXPORT(cff_ApproxDLen, cff_ApproxDLen_impl);

CFF_API void cff_LinesPerEm_impl(void *unused, int32_t v, int32_t *ox, int32_t *oy, uint8_t *st) {
    (void)unused;
    int32_t p[2] = {v, 0};
    *ox = ApproxDLen(p, st);
    int32_t q[2] = {0, v};
#ifdef CFF_CONTROL_LINESPEREM
    q[1] = v + 0x8000;
#endif
    *oy = ApproxDLen(q, st);
}
CFF_EXPORT_STK(cff_LinesPerEm, cff_LinesPerEm_impl);

CFF_API void cff_TfmWidths_impl(int32_t *src, uint32_t n, int32_t vert, int32_t tfm, int32_t *out, uint8_t *st) {
    if (n == 0)
        return;
    if (tfm == 0) {
        for (uint32_t i = 0; i < n; i++) {
            int32_t p[2] = {src[i], 0};
            out[i] = ApproxDLen(p, st);
        }
    } else if (vert == 0) {
        for (uint32_t i = 0; i < n; i++) {
            int32_t p[2] = {src[i], 0};
            tfm_call(st, 0x590, ld_pt(p), p);
            out[i] = abs32(p[0]);
        }
    } else {
        for (uint32_t i = 0; i < n; i++) {
            int32_t p[2] = {0, src[i]};
            tfm_call(st, 0x590, ld_pt(p), p);
            out[i] = abs32(p[1]);
        }
    }
#ifdef CFF_CONTROL_TFMWIDTHS
    out[0] += 0x4000;
#endif
}
CFF_EXPORT_STK(cff_TfmWidths, cff_TfmWidths_impl);

CFF_API void cff_FntTfmY_impl(int32_t y, int32_t *out, uint8_t *st) {
    int32_t p[2] = {0, y};
    tfm_call(st, 0x580, ld_pt(p), p);
#ifdef CFF_CONTROL_FNTTFMY
    p[1] += 0x4000;
#endif
    *out = p[1];
}
CFF_EXPORT(cff_FntTfmY, cff_FntTfmY_impl);

/* ======================================================================================== */
/* bounding box                                                                            */

CFF_API void cff_SetUpBBox_impl(int32_t *src, int32_t em, int32_t scale, int32_t *out) {
    int32_t hi, lo; /* ebx = ymax, r11 = ymin */
    if (src[0] == 0 && src[1] == 0 && src[2] == 0 && src[3] == 0) {
        int32_t e = fixmul(em, 0x28000);
        out[3] = e;
        out[2] = e;
        out[1] = neg32(e);
        out[0] = neg32(e);
        hi = e;
        lo = neg32(e);
    } else {
        U64(out, 0) = U64(src, 0);
        U64(out, 8) = U64(src, 8);
        hi = out[3];
        lo = out[1];
    }
    int32_t x0 = out[0];
    int32_t t = fixmul(sub32(out[2], x0), scale);
    t = (int32_t)((uint32_t)add32(t, 0x8000) & 0xffff0000u);
    if (t > 0x1f0000)
        t = 0x1f0000;
    out[2] = add32(out[2], t);
    out[0] = sub32(x0, t);
    t = fixmul(sub32(hi, lo), scale);
    t = (int32_t)((uint32_t)add32(t, 0x8000) & 0xffff0000u);
    if (t > 0x1f0000)
        t = 0x1f0000;
#ifdef CFF_CONTROL_SETUPBBOX
    t = 0; /* no widening, and a zero-width box */
    out[2] = out[0];
#endif
    out[1] = sub32(lo, t);
    out[3] = add32(t, hi);
}
CFF_EXPORT(cff_SetUpBBox, cff_SetUpBBox_impl);

static inline void undo_flags(int32_t *p, uint32_t f) {
    if (f & 0x80000)
        p[0] = neg32(p[0]);
    if (f & 0x400000)
        p[1] = neg32(p[1]);
    if (f & 0x10000) {
        int32_t t = p[0];
        p[0] = p[1];
        p[1] = t;
    }
}
static inline int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }

/* TfmBBox(state, bbox, out corners, out box): the bbox corners through the +0x580 proc;
 * out1 = (xmin, ymin) and (xmax, ymax) transformed, out2 = the box of the four corners with
 * the swap/sign flags of +0x2bc undone */
CFF_API void cff_TfmBBox_impl(uint8_t *st, int32_t *src, int32_t *out1, int32_t *out2) {
    int32_t P0[2] = {src[2], src[1]}, P1[2] = {src[0], src[3]}, P2[2] = {0, 0}, P3[2] = {0, 0};
    tfm_call(st, 0x580, ld_pt(src), P2);
    tfm_call(st, 0x580, ld_pt(P0), P0);
    tfm_call(st, 0x580, ld_pt(P1), P1);
    tfm_call(st, 0x580, ld_pt(src + 2), P3);
    out1[0] = P2[0];
    out1[1] = P2[1];
    out1[2] = P3[0];
    out1[3] = P3[1];
    uint32_t f = U32(st, 0x2bc);
    int32_t A[2] = {P2[0], P2[1]}, B[2] = {P0[0], P0[1]}, C[2] = {P1[0], P1[1]}, D[2] = {P3[0], P3[1]};
    undo_flags(A, f);
    undo_flags(B, f);
    undo_flags(C, f);
    undo_flags(D, f);
    out2[0] = min32(min32(A[0], B[0]), min32(C[0], D[0]));
    out2[1] = min32(min32(A[1], B[1]), min32(C[1], D[1]));
    out2[2] = max32(max32(A[0], B[0]), max32(C[0], D[0]));
    out2[3] = max32(max32(A[1], B[1]), max32(C[1], D[1]));
#ifdef CFF_CONTROL_TFMBBOX
    out2[3] = out2[1];
    out1[3] = out1[1];
#endif
}
CFF_EXPORT(cff_TfmBBox, cff_TfmBBox_impl);

/* ======================================================================================== */
/* stems                                                                                   */

/* SortStems(n, widths, *flags): sorts in place, zeroes duplicates, drops the leading values
 * <= 0; returns the count left. Sets flag bit 12 whenever it changed something. */
CFF_API uint32_t cff_SortStems_impl(uint32_t n, int32_t *arr, uint32_t *flags) {
    if (n == 0)
        return 0;
    uint32_t last = n - 1, i = last;
    while (i != 0) {
        uint32_t hi = i, lo = i - 1;
        int32_t a = arr[lo], b = arr[hi];
        if (a == b && a != 0) {
            arr[lo] = 0;
            *flags |= 1u << 12;
        } else if (a > b) {
            arr[hi] = a;
            arr[lo] = b;
            *flags |= 1u << 12;
            if (i < last)
                i++;
        } else {
            i = lo;
        }
    }
#ifdef CFF_CONTROL_SORTSTEMS
    if (n > 1 && arr[0] > 0)
        return n - 1;
#endif
    if (arr[0] > 0)
        return n;
    uint32_t left = 0, k = 1;
    if (k < n) {
        while (k < n && !(arr[k] > 0))
            k++;
        if (k < n) {
            left = n - k;
            for (uint32_t j = 0; j < left; j++)
                arr[j] = arr[k + j];
        }
    }
    *flags |= 1u << 12;
    return left;
}
CFF_EXPORT(cff_SortStems, cff_SortStems_impl);

/* SetUpStemW(font desc, state, horizontal, use transform, *std width): one direction of the
 * stem snap tables. flag 1: count st+0xe4, table st+0xe8, StdHW fd+0x70, StemSnapH fd+0x94;
 * flag 0: st+0x20, st+0x24, StdVW fd+0x74, StemSnapV fd+0xc4. The table holds 12 design
 * widths D, the transformed widths T (+0x30), and two parameters A (+0x60), B (+0x90). */
CFF_API void cff_SetUpStemW_impl(uint8_t *fd, uint8_t *st, int32_t flag, int32_t x, int32_t *outw) {
    uint32_t r11w = U16(st, 0);
    int32_t edi = 0, edx;
    int32_t r12;
    uint8_t *r14, *rbx, *r13;
    if (flag) {
        r12 = I32(fd, 0x10);
        r14 = st + 0xe4;
        rbx = st + 0xe8;
        r13 = fd + 0x34;
    } else {
        r12 = I32(fd, 0x14);
        r14 = st + 0x20;
        rbx = st + 0x24;
        r13 = fd + 0x64;
    }
#define RB(i) I32(rbx, 4 * (uint64_t)(uint32_t)(i))
#define SRC(i) I32(r13, 4 * (uint64_t)(uint32_t)(i))
    uint32_t r15 = U32(r14, 0);
    if (r15 == 0) {
        U32(r14, 0) = 0;
        *outw = 0;
        return;
    }
    for (uint32_t r8 = 1; r8 < r15; r8++)
        if (SRC(r8 - 1) >= SRC(r8)) {
            U32(r14, 0) = 0;
            *outw = 0;
            U32(st, 0x2c0) |= 1u << 12;
            return;
        }
    if (r12 != 0) {
        if (x == 0) {
            int32_t p[2] = {r12, 0};
            edi = ApproxDLen(p, st);
        } else if (flag != 0) {
            int32_t p[2] = {0, r12};
            tfm_call(st, 0x590, ld_pt(p), p);
            edi = abs32(p[1]);
        } else {
            int32_t p[2] = {r12, 0};
            tfm_call(st, 0x590, ld_pt(p), p);
            edi = abs32(p[0]);
        }
    }
#ifdef CFF_CONTROL_SETUPSTEMW
    edi = add32(edi, 0x4000);
#endif
    edx = 0;
    if (edi != 0 && edi < (int32_t)(r11w * 3u << 15))
        goto single;
    if (r15 <= 1)
        goto single;
    U32(r14, 0) = r15;
    CFF_ORIG(void (*)(uint8_t *, uint32_t, int32_t, int32_t, uint8_t *, uint8_t *), RVA_TfmWidths)(r13, r15, flag, x, rbx + 0x30,
                                                                                                    st);
    {
        uint32_t r8 = 0xffffffffu;
        for (uint32_t i = 0; i < r15; i++) {
            RB(i) = SRC(i);
            RB(i + 0x24) = 0x7fffffff;
            RB(i + 0x18) = 0x7fffffff;
            if (SRC(i) == r12) {
                r8 = i;
                RB(i + 0x24) = 0;
                RB(i + 0x18) = 0;
            }
        }
        if (r8 == 0xffffffffu) {
            U32(r14, 0) = 0;
            *outw = 0;
            U32(st, 0x2c0) |= 1u << 12;
            return;
        }
        *outw = edi;
        if (r8 != 0) {
            int32_t d = sub32(edi, RB(0xc));
            if (d <= 0x5700) {
                int32_t q = fixdiv(0x5700, d);
                RB(0x24) = q;
                int32_t a = fixmul(RB(0xc), q);
                RB(0x18) = a;
                if (a == 0x7fffffff)
                    RB(0x18) = 0x7ffffffe;
            } else {
                RB(0x24) = 0;
                RB(0x18) = 0;
            }
        }
        if (r8 + 1 < r15) {
            int32_t t = RB(r15 + 0xb);
            int32_t d = sub32(t, edi);
            if (d <= 0xae00) {
                int32_t q = fixdiv(0xae00, d);
                RB(r15 + 0x23) = q;
                int32_t a = fixmul(t, q);
                RB(r15 + 0x17) = a;
                if (a == 0x7fffffff)
                    RB(r15 + 0x17) = 0x7ffffffe;
            } else {
                RB(r15 + 0x17) = 0;
                RB(r15 + 0x23) = 0;
            }
        }
    }
    for (;;) { /* b434c: fill the A/B parameters of every run of unset entries */
        uint32_t k = 1;
        for (;;) {
            if (RB(k + 0x18) == 0x7fffffff)
                break;
            k++;
            if (k >= r15)
                break;
        }
        if (k == r15) {
            U32(r14, 0) = 0;
            uint32_t cnt = 0;
            for (uint32_t j = 0;;) {
                int32_t t = RB(j + 0xc);
                if (t >= 0xff0000)
                    break;
                if (RB(j + 0x18) > 0)
                    RB(j + 0xc) = neg32(t);
                else
                    cnt++;
                j++;
                U32(r14, 0) = j;
                if (j >= r15)
                    break;
            }
            if (cnt == 0)
                U32(r14, 0) = 0;
            return;
        }
        uint32_t r8 = k, e = k;
        if (e < r15) {
            do {
                if (RB(e + 0x18) != 0x7fffffff)
                    break;
                e++;
            } while (e < r15);
        }
        int32_t r9 = 0;
        uint32_t best = 0xffffffffu;
        int32_t dlo = RB(r8 - 1), tlo = RB(r8 + 0xb), dhi = RB(e), thi = RB(e + 0xc);
        for (; r8 < e; r8++) {
            int32_t dv = RB(r8);
            int32_t up = sub32(dhi, dv), dn = sub32(dv, dlo);
            up = add32(up, up);
            if (up < dn) {
                if (up > r9) {
                    best = r8;
                    r9 = up;
                }
            } else if (dn > r9) {
                r9 = dn;
                best = r8;
            }
        }
        int32_t tb = RB(best + 0xc);
        uint32_t ai = best + 0x18, bi = best + 0x24;
        int32_t gap_hi = sub32(thi, tb), gap_lo = sub32(tb, tlo);
        int32_t num, den;
        if (add32(gap_hi, gap_hi) < gap_lo) {
            if (gap_hi > 0x5700)
                goto zero;
            den = gap_hi;
            num = 0x5700;
        } else {
            if (gap_lo > 0xae00)
                goto zero;
            den = gap_lo;
            num = 0xae00;
        }
        {
            int32_t q = fixdiv(num, den);
            RB(bi) = q;
            int32_t a = fixmul(tb, q);
            RB(ai) = a;
            if (I32(rbx, 0x64) == 0x7fffffff)
                I32(rbx, 0x64) = 0x7ffffffe;
        }
        continue;
    zero:
        RB(bi) = 0;
        RB(ai) = 0;
    }
single:
    U32(r14, 0) = 1;
    RB(0) = r12;
    RB(0xc) = edi;
    *outw = edi;
    RB(0x24) = edx;
    RB(0x18) = edx;
    if (edi > 0xff0000)
        U32(r14, 0) = (uint32_t)edx;
#undef RB
#undef SRC
}
CFF_EXPORT_STK(cff_SetUpStemW, cff_SetUpStemW_impl);

CFF_API void cff_SetUpStemSnap_impl(uint8_t *fd, uint8_t *st, int32_t *out, int32_t x, int32_t *out5) {
    int32_t w1 = 0, w2 = 0;
    I32(st, 0x378) = (int32_t)0x80000000;
    I32(st, 0x374) = (int32_t)0x80000000;
    I32(st, 0x380) = I32(fd, 0x10);
    I32(st, 0x37c) = I32(fd, 0x14);
    typedef void (*stemw_fn)(uint8_t *, uint8_t *, int32_t, int32_t, int32_t *);
    CFF_ORIG(stemw_fn, RVA_SetUpStemW)(fd, st, 1, x, &w2);
    CFF_ORIG(stemw_fn, RVA_SetUpStemW)(fd, st, 0, x, &w1);
    *out = 0;
    int32_t r8 = 0, ecx = 0;
    if (w1 != 0) {
        ecx = (int32_t)((uint32_t)add32(w1, 0x8000) & 0xffff0000u);
        if (ecx == 0)
            ecx = 0x10000;
        r8 = w1;
        *out = ecx;
    }
    if (w2 != 0) {
        r8 = add32(r8, w2);
        int32_t e = (int32_t)((uint32_t)add32(w2, 0x8000) & 0xffff0000u);
        if (e == 0)
            e = 0x10000;
        ecx = add32(ecx, e);
        *out = ecx;
    }
    if (w1 != 0 && w2 != 0) {
        r8 = sar32(r8, 1);
        ecx = sar32(ecx, 1);
        *out = ecx;
    }
#ifdef CFF_CONTROL_SETUPSTEMSNAP
    r8 += 0x4000;
#endif
    I32(st, 0x2e0) = r8;
    *out5 = w2;
    I32(st, 0x2e8) = w1 > w2 ? w1 : w2;
}
CFF_EXPORT_STK(cff_SetUpStemSnap, cff_SetUpStemSnap_impl);

/* ======================================================================================== */
/* blues                                                                                   */

CFF_API int32_t cff_AdjustBlueScale_impl(int32_t scale, uint32_t n, int32_t *z) {
    if (n == 0)
        return scale;
    int32_t m = 0;
    for (uint32_t k = n; k != 0; k--) {
        int32_t d = sub32(z[0], z[1]);
        z += 5;
        if (d <= m)
            d = m;
        m = d;
    }
    if (m == 0)
        return scale;
    int32_t p = fixmul(m, scale);
#ifdef CFF_CONTROL_ADJUSTBLUESCALE
    p = 0x20000;
#endif
    if (p > 0x10000)
        return fixdiv(0x10000, m) - 1;
    if (p < 0x8000)
        return fixdiv(0x8000, m) - 1;
    return scale;
}
CFF_EXPORT(cff_AdjustBlueScale, cff_AdjustBlueScale_impl);

CFF_API void cff_BlueBoostSetUp_impl(uint8_t *st) {
    int32_t p[2] = {0, 0x230000};
    tfm_call(st, 0x590, ld_pt(p), p);
    int32_t y = p[1];
#ifdef CFF_CONTROL_BLUEBOOSTSETUP
    y = 0;
#endif
    if (y >= (int32_t)((uint32_t)U16(st, 0) << 17)) {
        I32(st, 0x3e0) = 0;
        return;
    }
    I32(st, 0x3e0) = 1;
    if (U32(st, 0x1b4) > 0) {
        uint16_t k = 0;
        do {
            uint32_t z = (uint32_t)k * 5;
            k++;
            I32(st, 0x1c8 + 4 * (uint64_t)z) = I32(st, 0x1c0 + 4 * (uint64_t)z);
        } while ((uint32_t)k < U32(st, 0x1b4));
    }
    U16(st, 0x3e4) = 0xffff;
    U16(st, 0x338) = 0;
    I32(st, 0x3d8) = y;
    CFF_ORIG(void (*)(uint8_t *), RVA_BlueBoost)(st);
}
CFF_EXPORT(cff_BlueBoostSetUp, cff_BlueBoostSetUp_impl);

/* SetUpBlueValues(state, family count, family mask, font desc): zones at st + 0x1b8, 0x14
 * bytes each (design top, bottom, transformed top, bottom, -); a zone takes the family zone
 * of the same kind whose transformed edges are nearest, when both edges agree and are less
 * than a pixel away (st + 0 is the ppem as u16) */
CFF_API void cff_SetUpBlueValues_impl(uint8_t *st, uint32_t nfam, uint32_t mask, uint8_t *fd) {
    typedef void (*ftfm_fn)(int32_t, int32_t *, uint8_t *);
    ftfm_fn FntTfmY = CFF_ORIG(ftfm_fn, RVA_FntTfmY);
    int32_t loc[24];
    if (nfam > 12 || U32(st, 0x1b4) > 12)
        goto err;
    for (uint32_t i = 0; i < U32(st, 0x1b4); i++) {
        int32_t a = I32(fd, 0xa0 + 8 * (uint64_t)i);
        if (a < I32(fd, 0xa4 + 8 * (uint64_t)i))
            goto err;
#ifdef CFF_CONTROL_SETUPBLUEVALUES
        a = add32(a, 0x1e0000); /* 30 units */
#endif
        uint64_t z = 4 * (uint64_t)i * 5;
        I32(st, 0x1b8 + z) = a;
        FntTfmY(a, &I32(st, 0x1c0 + z), st);
        int32_t b = I32(fd, 0xa4 + 8 * (uint64_t)i);
        I32(st, 0x1bc + z) = b;
        FntTfmY(b, &I32(st, 0x1c4 + z), st);
    }
    if (nfam == 0)
        goto boost;
    for (uint32_t j = 0; j < nfam; j++) {
        int32_t a = I32(fd, 0x100 + 8 * (uint64_t)j);
        if (a < I32(fd, 0x104 + 8 * (uint64_t)j))
            goto err;
        FntTfmY(a, &loc[2 * j], st);
        FntTfmY(I32(fd, 0x104 + 8 * (uint64_t)j), &loc[2 * j + 1], st);
    }
    for (uint32_t i = 0; i < U32(st, 0x1b4); i++) {
        uint64_t z = 4 * (uint64_t)i * 5;
        uint32_t kind = (U32(st, 0x2dc) >> (i & 31)) & 1;
        int32_t bestA = 0x3e80000, bestB = 0x3e80000;
        uint32_t ia = 12, ib = 12;
        for (uint32_t j = 0;;) {
            if (kind == ((mask >> (j & 31)) & 1)) {
                int32_t da = abs32(sub32(loc[2 * j], I32(st, 0x1c0 + z)));
                int32_t db = abs32(sub32(loc[2 * j + 1], I32(st, 0x1c4 + z)));
                if (da < bestA) {
                    bestA = da;
                    ia = j;
                }
                if (db < bestB) {
                    ib = j;
                    bestB = db;
                }
            }
            j++;
            if (j >= nfam)
                break;
        }
        if (ia != ib || ib >= 12 || ia >= 12)
            continue;
        int32_t lim = (int32_t)((uint32_t)U16(st, 0) << 16);
        if (bestA >= lim) {
            if (bestB >= lim)
                continue;
            int32_t v = loc[2 * ib + 1];
            if (v > I32(st, 0x1c0 + z))
                continue;
            I32(st, 0x1c4 + z) = v;
        } else {
            int32_t v = loc[2 * ia];
            if (bestB >= lim) {
                if (v < I32(st, 0x1c4 + z))
                    continue;
                I32(st, 0x1c0 + z) = v;
            } else {
                I32(st, 0x1c0 + z) = v;
                I32(st, 0x1c4 + z) = loc[2 * ib + 1];
            }
        }
    }
boost:
    CFF_ORIG(void (*)(uint8_t *), RVA_BlueBoostSetUp)(st);
    return;
err:
    U32(st, 0x1b4) = 0;
    I32(st, 0x3e0) = 0;
    U32(st, 0x2c0) |= 1u << 12;
}
CFF_EXPORT(cff_SetUpBlueValues, cff_SetUpBlueValues_impl);

/* ======================================================================================== */
/* the setup                                                                               */

/* BCSetUpValues(font desc, matrix, -, rendering props, -, memory record, state, hint, -) */
CFF_API int32_t cff_BCSetUpValues_impl(uint8_t *fd, int32_t *mtx, void *a3, uint8_t *rp, void *a5, uint8_t *mem, uint8_t *st,
                                       int32_t hint, void *a9) {
    (void)a3;
    (void)a5;
    (void)a9;
    uint8_t *f60 = fd + 0x60;
    int32_t L48 = 0, L58 = 0, L60 = 0, L68 = 0, L78 = 0;
    int32_t bbox[4] = {0, 0, 0, 0};
    U16(st, 0x3e6) = U16(fd, 2);
    I32(mem, 0x18) = 0x10000;
    U32(st, 0x2c0) = 0;
    int32_t eax, edx = I32(f60, 0x10);
    if (edx == 0 && I32(f60, 0x14) != 0) {
        eax = I32(f60, 0x14);
        I32(f60, 0x10) = eax;
    } else {
        eax = edx;
        if (I32(f60, 0x14) == 0 && edx != 0)
            I32(f60, 0x14) = edx;
    }
    if (U16(fd, 0x42) == 0 && eax != 0) {
        U16(fd, 0x42) = 1;
        I32(f60, 0x34) = I32(f60, 0x10);
    }
    if (U16(fd, 0x44) == 0 && I32(f60, 0x14) != 0) {
        U16(fd, 0x44) = 1;
        I32(f60, 0x64) = I32(f60, 0x14);
    }
    if (I32(f60, 0x10) == 0 && U16(fd, 0x42) != 0)
        I32(f60, 0x10) = I32(f60, 0x34);
    if (I32(f60, 0x14) == 0 && U16(fd, 0x44) != 0)
        I32(f60, 0x14) = I32(f60, 0x64);
    typedef uint32_t (*sort_fn)(uint32_t, uint8_t *, uint8_t *);
    U32(st, 0xe4) = CFF_ORIG(sort_fn, RVA_SortStems)(U16(fd, 0x42), f60 + 0x34, st + 0x2c0);
    U32(st, 0x20) = CFF_ORIG(sort_fn, RVA_SortStems)(U16(fd, 0x44), f60 + 0x64, st + 0x2c0);
    if (I32(f60, 0x10) <= 0)
        I32(f60, 0x10) = U32(st, 0xe4) != 0 ? I32(f60, 0x34) : 0;
    if (I32(f60, 0x14) <= 0)
        I32(f60, 0x14) = U32(st, 0x20) != 0 ? I32(f60, 0x64) : 0;
    I32(st, 0x2c4) = I32(fd, 0x3c);
    I32(st, 0x32c) = I32(fd, 0x50);
    uint16_t cx;
    if (!rp) {
        I32(st, 0x2ec) = 0x16a0a;
        I32(st, 0x2f0) = 0x16a0a;
        cx = 0;
        goto defaults;
    }
    I32(st, 0x2ec) = I32(rp, 0);
    I32(st, 0x2f0) = I32(rp, 4);
    if (I32(rp, 0x14) == 0) {
        cx = 0;
        goto defaults;
    }
    U16(st, 0) = U16(rp, 8);
    I32(st, 0x33c) = I32(rp, 0x14);
    U16(st, 2) = U16(rp, 0xa);
    U16(st, 0x330) = U16(rp, 0xc);
    U16(st, 0x332) = U16(rp, 0xe);
    cx = U16(rp, 0x10);
    goto props;
defaults:
    U32(st, 0x330) = 1;
    I32(st, 0x33c) = 0;
    U32(st, 0) = 0x10001;
props:
    U16(st, 0x334) = cx;
    I32(st, 0x324) = U16(fd, 0x58);
    if (U16(st, 0x3e6) == 2) {
        uint32_t v = U32(st, 0x2c4);
        I32(st, 0x320) = v < 0x4d8 ? 0x6b : v < 0x846c ? 0x46b : 0x8000;
        v = U32(st, 0x32c);
        I32(st, 0x328) = v < 0x4d8 ? 0x6b : v < 0x846c ? 0x46b : 0x8000;
    } else {
        I32(st, 0x320) = U16(fd, 0x54);
        I32(st, 0x328) = U16(fd, 0x56);
    }
    I32(st, 0x2f4) = I32(fd, 0x34);
    CFF_ORIG(void (*)(uint8_t *, int32_t *), RVA_GetEmSize)(fd + 4, &L58);
    int32_t em = L58;
    U32(st, 0x2bc) = 0;
    typedef int32_t (*mtx_fn)(int32_t *, int32_t, uint8_t *, uint8_t *, int32_t *, int32_t *, uint8_t *, uint8_t *);
    int32_t r = CFF_ORIG(mtx_fn, RVA_ATMSetUpMtx)(mtx, em, st + 8, rp, &L78, &L48, st + 0x2bc, st);
    if (r != 0)
        return r;
    if (U32(st, 0x2bc) & 0x10000) {
        uint16_t w2 = U16(st, 2), w330 = U16(st, 0x330), w332 = U16(st, 0x332), w334 = U16(st, 0x334);
        U16(st, 2) = w330;
        U16(st, 0x330) = w2;
        U16(st, 0x332) = w334;
        U16(st, 0x334) = w332;
        int32_t t = I32(st, 0x2ec);
        I32(st, 0x2ec) = I32(st, 0x2f0);
        I32(st, 0x2f0) = t;
    }
    CFF_ORIG(void (*)(uint8_t *, int32_t, int32_t, int32_t *), RVA_SetUpBBox)(f60, em, I32(fd, 0x2c), bbox);
    CFF_ORIG(void (*)(uint8_t *, int32_t *, uint8_t *, uint8_t *), RVA_TfmBBox)(st, bbox, st + 0x360, st + 0x2cc);
    CFF_ORIG(void (*)(void *, int32_t, uint8_t *, uint8_t *, uint8_t *), RVA_LinesPerEm)(NULL, em, st + 0x2a8, st + 0x2ac, st);
    int32_t ppem = I32(st, 0x2a8);
    if (ppem >= I32(st, 0x2ac))
        ppem = I32(st, 0x2ac);
    int32_t e4;
    if (ppem <= 0x80000)
        e4 = 0x9000;
    else if (ppem < 0x320000)
        e4 = add32(0x19000, fracmul(sub32(ppem, 0x80000), 0x1861861));
    else
        e4 = 0x19000;
#ifdef CFF_CONTROL_BCSETUPVALUES
    e4 += 0x1000;
#endif
    I32(st, 4) = e4;
    int32_t ok = (L78 == 0 && L48 == 0) ? 0 : ppem >= 0x40000;
    int32_t ebx = hint != 0 ? ok : 0;
    I32(st, 0x2c8) = U16(fd, 0x40);
    I32(st, 0x2b8) = U16(fd, 0x40) == 2 ? I32(fd, 0x28) : 0;
    int32_t big = fixmul(L58, 0x28000);
    if (I32(f60, 0x10) > big) {
        I32(f60, 0x10) = 0;
        U32(st, 0x2c0) |= 0x1000;
    }
    if (I32(f60, 0x14) > big) {
        I32(f60, 0x14) = 0;
        U32(st, 0x2c0) |= 0x1000;
    }
    CFF_ORIG(void (*)(uint8_t *, uint8_t *, int32_t *, int32_t, int32_t *), RVA_SetUpStemSnap)(f60, st, &L60, ebx, &L68);
    if (ebx == 0) {
        U32(st, 0x1b4) = 0;
    } else {
        U32(st, 0x1b4) = U16(fd, 0x46);
        I32(st, 0x2dc) = I32(fd, 0x20);
        CFF_ORIG(void (*)(uint8_t *, uint32_t, uint32_t, uint8_t *), RVA_SetUpBlueValues)(st, U16(fd, 0x48), U32(fd, 0x24), f60);
    }
    I32(st, 0x1ac) = I32(f60, 0x98);
    uint32_t nblues = U32(st, 0x1b4);
    I32(st, 0x1b0) = I32(f60, 0x9c);
    I32(st, 0x370) = I32(f60, 0x160);
    I32(st, 0x1a8) = CFF_ORIG(int32_t (*)(int32_t, uint32_t, uint8_t *), RVA_AdjustBlueScale)(I32(f60, 0x94), nblues, st + 0x1b8);
    I32(st, 0x2b4) = I32(fd, 0x2c);
    I32(st, 0x2b0) = I32(fd, 0x1c);
    for (int k = 0; k < 7; k++)
        I32(st, 0x2f8 + 4 * k) = I32(f60, 0x18 + 4 * k);
    uint32_t r9 = 0, r8 = 0, dx = 0, c20 = 0, r14 = 0;
    if (ebx) {
        if (L78 != 0)
            r9 = 0x40000000;
        if (L48 != 0)
            r8 = 0x80000000;
        dx = (uint32_t)sub32(ppem, 0x68001) <= 0xafffe ? 0x10 : 0;
    }
    if (U16(fd, 0x4a) == 1 && ebx)
        c20 = 0x20;
    if (!(L60 > 0x10000) && L60 != 0 && I32(st, 0x2e0) != 0)
        r14 = 0x40;
    U32(st, 0x2bc) = ((U32(f60, 0x164) & 1) << 7) | U32(st, 0x2bc) | r14 | c20 | dx | r8 | r9;
    I32(st, 0x384) = I32(fd, 0x1c8);
    U64(st, 0x388) = U64(fd, 0x1d0);
    I32(st, 0x390) = I32(fd, 0x1d8);
    I32(st, 0x394) = I32(fd, 0x1dc);
    U64(st, 0x398) = U64(fd, 0x1e0);
    I32(st, 0x3a0) = I32(fd, 0x1e8);
    U64(st, 0x3a8) = U64(fd, 0x1f0);
    U64(st, 0x3b0) = U64(fd, 0x1f8);
    return 0;
}
CFF_EXPORT_STK(cff_BCSetUpValues, cff_BCSetUpValues_impl);

/* ATMSetUpValues: the proc at CFFFontInst + 0x5d8; BCSetUpValues with ATMSetUpMtx as its
 * fifth argument (BCSetUpValues ignores it and calls ATMSetUpMtx directly) */
CFF_API int32_t cff_ATMSetUpValues_impl(void *a1, void *a2, void *a3, void *a4, void *a5, void *a6, int32_t a7, void *a8) {
    typedef int32_t (*bcsv_fn)(void *, void *, void *, void *, void *, void *, void *, int32_t, void *);
#ifdef CFF_CONTROL_ATMSETUPVALUES
    a7 = !a7;
#endif
    return CFF_ORIG(bcsv_fn, RVA_BCSetUpValues)(a1, a2, a3, a4, idwr_orig(RVA_ATMSetUpMtx), a5, a6, a7, a8);
}
CFF_EXPORT_STK(cff_ATMSetUpValues, cff_ATMSetUpValues_impl);

/* ======================================================================================== */
/* slope tables and the hinted point transforms                                             */
/* A slope table: +0 count, +8 the current segment, segments of 0x20 bytes from +0x18:       */
/* +0 slope, +4 offset, +8 x start, +0xc y, +0x10 flags, +0x18 link                         */

CFF_API int32_t cff_Map_impl(uint8_t *sl, int32_t v) {
    uint8_t *seg = PTR(sl, 8);
    int32_t r9 = v > (int32_t)0xd8300001 ? v : (int32_t)0xd8300002;
    if (r9 >= 0x27cfffff)
        r9 = 0x27cffffe;
    if (r9 >= I32(seg, 0x28)) {
        if (r9 == 0x7fffffff)
            return 0x7fffffff;
        do
            seg += 0x20;
        while (r9 >= I32(seg, 0x28));
        PTR(sl, 8) = seg;
    } else if (r9 < I32(seg, 8)) {
        do
            seg -= 0x20;
        while (r9 < I32(seg, 8));
        PTR(sl, 8) = seg;
    }
    int32_t r;
    if (((uint32_t)r9 & 0xf0007fffu) == 0)
        r = sar32((int32_t)((uint32_t)(uint16_t)(r9 >> 15) * (uint32_t)I32(seg, 0)), 1);
    else
        r = fixmul(I32(seg, 0), r9);
#ifdef CFF_CONTROL_MAP
    r += 1;
#endif
    return add32(r, I32(seg, 4));
}
CFF_EXPORT(cff_Map, cff_Map_impl);

CFF_API void cff_initslope_impl(uint8_t *t, int32_t s) {
    int32_t n = I32(t, 0);
    uint8_t *rbp = t + 0x38;
    uint8_t *r10 = (uint8_t *)((uintptr_t)rbp - 0x20 + (uintptr_t)((int64_t)n * 32));
    uint8_t *r9 = rbp;
    int32_t n1 = add32(n, 1);
    I32(t, 0) = n1;
    I32(rbp, 0) = s;
    if (n1 <= 2) {
        I32(rbp, 4) = 0;
    } else {
        int32_t d = sub32(I32(rbp, 0x2c), I32(rbp, 0x28));
        I32(rbp, 0xc) = sub32(d, 0x27cfffff);
        I32(rbp, 4) = fixmul(d, s);
        int32_t d2 = sub32(I32(r10, 0xc), I32(r10, 8));
        I32(r10, 0) = s;
        I32(r10, 4) = fixmul(d2, s);
    }
    r9 += 0x20;
    I32(rbp, 8) = (int32_t)0x80000000;
    I32(r10, 0x28) = 0x7fffffff;
    while ((uintptr_t)r9 < (uintptr_t)r10) {
        int32_t nx = I32(r9, 0x28), x0 = I32(r9, 8);
        int32_t dx = sub32(nx, x0);
        int32_t slope, res;
        if (dx <= 0) {
            int32_t mid = sar32(add32(nx, x0), 1);
            int32_t h = sar32(dx, 1);
            int32_t ny = add32(I32(r9, 0xc), h);
            I32(r9, 8) = mid;
            I32(r9, 0x2c) = sub32(I32(r9, 0x2c), h);
            I32(r9, 0xc) = ny;
            I32(r9, 0x28) = mid;
            res = fixmul(ny, s);
            slope = 0;
        } else {
            int32_t ny1 = I32(r9, 0x2c), y0 = I32(r9, 0xc);
            if (ny1 <= y0) {
                I32(r9, 0x2c) = y0;
                res = fixmul(y0, s);
                slope = 0;
            } else {
                int32_t q = fixdiv(sub32(ny1, y0), dx);
                int32_t m = fixmul(s, q);
                slope = m > 0x2ae14 ? 0x2ae14 : m;
                int32_t a = fixmul(x0, slope);
                res = sub32(fixmul(y0, s), a);
            }
        }
#ifdef CFF_CONTROL_INITSLOPE
        res += 0x100;
#endif
        I32(r9, 4) = res;
        I32(r9, 0) = slope;
        r9 += 0x20;
    }
    PTR(t, 8) = rbp;
}
CFF_EXPORT(cff_initslope, cff_initslope_impl);

CFF_API void cff_fixupmap_impl(uint8_t *t, uint8_t *st) {
    int32_t r10 = I32(t, 0x10);
    uint8_t *r8 = t + 0x58;
    int32_t r11 = add32(sar32(r10, 1), r10);
#ifdef CFF_CONTROL_FIXUPMAP
    r11 = add32(r11, r10);
#endif
    int64_t idx = (int64_t)sub32(I32(t, 0), 1) - 2;
    uint8_t *r9 = (uint8_t *)((uintptr_t)r8 + (uintptr_t)(idx * 32));
    uint8_t *r13 = r8;
    while ((uintptr_t)r8 < (uintptr_t)r9) {
        uint8_t *rbx = PTR(r8, 0x18);
        uint8_t *rdx = r8 + 0x20;
        if (rdx == rbx || (U32(r8, 0x10) & 0x200) || ((U32(rdx, 0x10) | U32(r8, 0x10)) & 0x10))
            goto next;
        {
            uint8_t *rsi = PTR(rdx, 0x18);
            int32_t r12 = I32(rbx, 0xc), edi = I32(r8, 0xc), r15 = I32(rdx, 0xc);
            int32_t r14 = sar32(add32(I32(rsi, 0xc), r15), 1);
            int32_t c = sar32(add32(r12, edi), 1);
            if (sub32(r14, c) >= r11)
                goto next;
            if (!(U8(rdx, 0x10) & 1)) {
                if (rdx == r9)
                    goto up;
                uint8_t *rax = PTR(rdx, 0x58);
                int32_t e = sar32(add32(I32(rax, 0xc), I32(rdx, 0x4c)), 1);
                if (add32(add32(r14, r11), r10) < e)
                    goto up;
            }
            if (U8(r8, 0x10) & 1)
                goto next;
            if (r8 != r13 + 0x20) {
                uint8_t *rax = PTR(r8, -0x28);
                int32_t e = sar32(add32(I32(rax, 0xc), I32(r8, -0x34)), 1);
                int32_t f = sub32(sub32(sar32(add32(r12, edi), 1), r11), r10);
                if (f <= e)
                    goto next;
            }
            I32(r8, 0xc) = sub32(edi, r10);
            I32(rbx, 0xc) = sub32(I32(rbx, 0xc), r10);
            goto set;
        up:
            I32(rdx, 0xc) = add32(r15, r10);
            I32(rsi, 0xc) = add32(I32(rsi, 0xc), r10);
        set:
            I32(st, 0x43c) = 1;
        }
    next:
        r8 = rdx;
    }
}
CFF_EXPORT(cff_fixupmap, cff_fixupmap_impl);

CFF_API void cff_InitSlopes_impl(int32_t sx, int32_t sy, uint8_t *st) {
    typedef void (*fix_fn)(uint8_t *, uint8_t *);
    typedef void (*init_fn)(uint8_t *, int32_t);
    if (I32(st, 0x438) != 0 && I32(st, 0x440) != 0) {
        CFF_ORIG(fix_fn, RVA_fixupmap)(PTR(st, 0x468), st);
        CFF_ORIG(fix_fn, RVA_fixupmap)(PTR(st, 0x470), st);
    }
#ifdef CFF_CONTROL_INITSLOPES
    sx = add32(sx, 0x1000);
#endif
    CFF_ORIG(init_fn, RVA_initslope)(PTR(st, 0x468), sx);
    CFF_ORIG(init_fn, RVA_initslope)(PTR(st, 0x470), sy);
    uint8_t *sl = PTR(st, 0x468);
    int32_t top = I32(sl, (int64_t)I32(sl, 0) * 32);
    if (top <= I32(st, 0x3c4))
        return;
    uint8_t *cur = PTR(sl, 8);
    I32(st, 0x3c4) = top;
    int32_t v = I32(PTR(PTR(st, 0x358), 8), 8);
    int32_t r;
    if (PTR(st, 0x428) != (uint8_t *)idwr_orig(RVA_TfmLockPt1)) {
        r = Map(sl, sub32(v, I32(st, 0x3d0)));
    } else {
        int32_t q[2] = {Map(sl, sub32(v, I32(st, 0x3d0))), 0};
        tfm_call(st, 0x580, ld_pt(q), q);
        r = q[0];
    }
    I32(PTR(PTR(st, 0x358), 8), 0x20) = r;
    PTR(PTR(st, 0x468), 8) = cur;
}
CFF_EXPORT(cff_InitSlopes, cff_InitSlopes_impl);

CFF_API void cff_TfmLockPt_noTfm_impl(int32_t *p, int32_t *out, uint8_t *st) {
    if (PTR(st, 0x428) == NULL) {
        PTR(st, 0x428) = (uint8_t *)idwr_orig(RVA_TfmLockPt_noTfm);
        InitSlopes(I32(st, 0x548), I32(st, 0x554), st);
    }
    int32_t x = Map(PTR(st, 0x468), p[0]);
#ifdef CFF_CONTROL_TFMLOCKPT_NOTFM
    x += 1;
#endif
    out[0] = x;
    out[1] = Map(PTR(st, 0x470), p[1]);
}
CFF_EXPORT(cff_TfmLockPt_noTfm, cff_TfmLockPt_noTfm_impl);

CFF_API void cff_TfmLockPt1_impl(int32_t *p, int32_t *out, uint8_t *st) {
    if (I32(st, 0x460) == 0) {
        U64(out, 0) = U64(p, 0);
        return;
    }
    if (PTR(st, 0x428) == NULL) {
        PTR(st, 0x428) = (uint8_t *)idwr_orig(RVA_TfmLockPt1);
        InitSlopes(0x10000, 0x10000, st);
    }
    int32_t q[2];
    q[0] = Map(PTR(st, 0x468), p[0]);
    q[1] = Map(PTR(st, 0x470), p[1]);
#ifdef CFF_CONTROL_TFMLOCKPT1
    q[1] += 1;
#endif
    tfm_call(st, 0x580, ld_pt(q), out);
}
CFF_EXPORT(cff_TfmLockPt1, cff_TfmLockPt1_impl);

CFF_API void cff_TfmLockPt3_impl(int32_t *p, int32_t *out, uint8_t *st) {
    uint64_t pt = ld_pt(p);
#ifdef CFF_CONTROL_TFMLOCKPT3
    pt += 1;
#endif
    tfm_call(st, 0x580, pt, out);
}
CFF_EXPORT(cff_TfmLockPt3, cff_TfmLockPt3_impl);
