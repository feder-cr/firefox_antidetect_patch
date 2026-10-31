/* The charstring interpreter (notes/cff.md, group 4 "interp"): the operator loop and its
 * driver. The path procs, flex and the small helpers are in interp_procs.c.
 *
 *   Type1InterpretCharString    0x1800b981c  picks the path procs (erectToDev tables), runs
 *                                            the interpreter, retries without hinting on failure
 *   DoType1InterpretCharString  0x1800b9ab0  Type 2 / CFF2 charstrings: operand stack, subrs,
 *                                            path operators, flex, hints and hint masks, blend
 *
 * Both are ported from the assembly. The interpreter is a multi-pass machine: with hinting on,
 * a first pass (state + 0x460 == 0) only collects the stems and the lines that GCLine extends,
 * then GCProcess / GlobalColoringGC lock them, and the charstring is interpreted again with the
 * path procs (state + 0x460 == 2 or 3). A hint failure restarts in plain mode (2).
 *
 * The labels L_xxxxx are the addresses (RVA) of the original basic blocks. Calls to every
 * other function go through CFF_ORIG, so each port can be swapped alone; the path procs and
 * the transform procs stay indirect calls through the same pointers.
 *
 * The ATM state (first argument, CFFFontInst + 0x9a0), as the interpreter uses it:
 *   +0x000 u16 hinting mode (4, 5, 7), +0x002 u16, +0x004 flex parameter, +0x008 the matrix
 *   for SetGSMatrix, +0x020/+0x0e4 std width counts (copied to +0x420/+0x424, their tables
 *   +0x024/+0x0e8 to +0x410/+0x418), +0x2a8/+0x2ac default std widths, +0x2b4 GCProcess
 *   parameter, +0x2bc flags (bit 4, 5, 16/19/22 the erect pattern, 30/31 hint directions),
 *   +0x2c0 flags (bit 13 hint failure), +0x2c4 subr count, +0x2c8, +0x2ec/+0x2f0 std width
 *   values, +0x2f4 random seed, +0x2f8..+0x304 default sidebearing and width, +0x318 the
 *   transient array (+0x324 its size), +0x320/+0x328 subr/gsubr bias, +0x32c gsubr count,
 *   +0x330 u16, +0x336/+0x338 u16 from the CharIO, +0x33c, +0x358 the CharIO, +0x370 nominal
 *   width, +0x374/+0x378 the stem dilation widths (+0x37c/+0x380 inputs), +0x384 CFF2,
 *   +0x388 the blend record, +0x390 vsindex, +0x394/+0x398 the blend vector arguments,
 *   +0x3a0 operand stack size, +0x3a8 operand stack, +0x3b0 the "blended" flags per operand,
 *   +0x3b8 width seen, +0x3bc stems locked, +0x3c0, +0x3c4..+0x3d4 the seac/accent advance,
 *   +0x3dc operator budget (20,000,000), +0x3e0 BlueBoost, +0x3e8 hint failure, +0x3ec/+0x3f0,
 *   +0x3f8 the global-coloring buffer (+0x400 its data, +0x458 = data + 0x4b0), +0x410..+0x424,
 *   +0x428 the TfmLockPt proc BCSetMtx installed, +0x434, +0x438, +0x43c, +0x440, +0x44c,
 *   +0x450 the memory record, +0x460 the pass (0 hints, 2 plain, 3 hinted), +0x468/+0x470 the
 *   x and y edge lists (Map), +0x4a8/+0x4b0 the stem and counter lists, +0x540.. the
 *   SetGSMatrix copy, +0x580/+0x588/+0x598 transform procs, +0x5d4.
 *
 * The original reads operand stack slots a charstring may never have written: at the end of a
 * counter group of 2 + 2 stems (hintmask, or the first moveto, after a cntrmask) it calls
 * RXLock(stack + 4) where the two vstems are at stack + 0..3. The port reads the same memory, so
 * it agrees with the original, but the original itself then depends on the previous glyph
 * (interp_synth_check.py --self cntrmask_first, without its six dropped numbers).
 *
 * Negative control (cff_check.py --control interp, -DCFF_CONTROL_INTERP): rlineto adds one
 * unit to the x of every point; --control type1interp: Type1InterpretCharString always uses
 * the C (outline client) erect procs. Diagnostic builds: -DCFF_CONTROL_INTERPCOV counts the
 * operators and paths taken (cff_interp_cov), -DCFF_CONTROL_SYNTH substitutes the charstring
 * (reimpl/cff/interp_synth_check.py). */
#include <math.h>
#include "../truetype/fatal.h"
#include <stdlib.h>
#include "interp.h"

#ifdef IDWR_STANDALONE
#define cff_Type1InterpretCharString_impl cff_Type1InterpretCharString
#define cff_DoType1InterpretCharString_impl cff_DoType1InterpretCharString
#endif

/* RVAs of the callees */
enum {
    RVA_DoType1InterpretCharString = 0xb9ab0,
    RVA_CSNewLine = 0x1846d0,
    RVA_FixedFltn = 0xc71e0,
    RVA_erectToDev_CurveTo_FixedFltn = 0xc7210,
    RVA_erectToDev_CurveTo_client = 0xd70c0,
    RVA_erectToDev_ClosePath = 0xe3890,
    RVA_erectToDev_NewChar = 0xe3920,
    RVA_erectToDev_EndChar = 0xe9180,
    RVA_TfmLockPt3 = 0xe1320,
    RVA_TfmLockPt_noTfm = 0xb31f0,
    RVA_TfmLockPt1 = 0xe0a80,
    RVA_AdobeCFFResetBlend = 0xe7fc4,
    RVA_InitEdgeList = 0xe45a4,
    RVA_SetGSMatrix = 0xbe544,
    RVA_BlueBoost = 0xb0098,
    RVA_GCFixOnePath = 0xe9870,
    RVA_Map = 0xb326c,
    RVA_GCLine = 0xe5770,
    RVA_RBLock = 0xb0580,
    RVA_RVLock = 0x10c2b0,
    RVA_RXLock = 0xb027c,
    RVA_RMLock = 0x10c260,
    RVA_GCStartLock = 0xf3154,
    RVA_ClearHintLists = 0xe4c84,
    RVA_BCTfmP = 0xb2460,
    RVA_AdobeCFFIsUsedBlend = 0x10cb90,
    RVA_AdobeCFFCheckBlendVector = 0x10c428,
    RVA_AdobeCFFBuildBlendVector = 0x10a848,
    RVA_AdobeCFFDoCharStringBlend = 0xed7c0,
    RVA_GlobalColoringGC = 0xae794,
    RVA_GCFind = 0xafaa0,
    RVA_GCNew = 0xaec78,
    RVA_GetDTfmY = 0xf0d74,
    RVA_GetDTfmX = 0xf3864,
    RVA_UseStdWidth = 0xb1948,
    RVA_GCCalcLocs = 0xafba4,
    RVA_NewGlbCounter = 0xf1230,
    RVA_GCFixLocs = 0x10c3fc,
    RVA_FlexProc = 0xe8568,
    RVA_FlexProc2 = 0xe8b9c,
    RVA_CScanFill = 0xbf620,
    RVA_AdjustToStdWidths = 0x10c144,
    RVA_GCProcess = 0xae68c,
    RVA_CCBuild = 0x1847d4,
    RVA_fixmul = 0xb3340,
    RVA_fixdiv = 0xb316c,
    RVA_GCInitBuffs = 0xe4978,
};

/* the erect proc tables erectToDev_LineTo 0x1801baad0 and erectToDev_MoveTo 0x1801bab50:
 * entry = bit 16 * 1 + bit 22 * 2 + bit 19 * 4 of state + 0x2bc, + 8 for CScan (CSNewLine) */
static const uint32_t erect_lineto[16] = {
    0xdd740,  /* e_LineTo_Cppp */ 0x183850, /* e_LineTo_Cppn */ 0x183810, /* e_LineTo_Cpnp */
    0x1837d0, /* e_LineTo_Cpnn */ 0x183790, /* e_LineTo_Cnpp */ 0x183750, /* e_LineTo_Cnpn */
    0x183700, /* e_LineTo_Cnnp */ 0x1836b0, /* e_LineTo_Cnnn */ 0xbeeb0,  /* e_LineTo_Bppp */
    0x183640, /* e_LineTo_Bppn */ 0x1835d0, /* e_LineTo_Bpnp */ 0xf1050,  /* e_LineTo_Bpnn */
    0x183560, /* e_LineTo_Bnpp */ 0xef340,  /* e_LineTo_Bnpn */ 0xec250,  /* e_LineTo_Bnnp */
    0x1834f0, /* e_LineTo_Bnnn */
};
static const uint32_t erect_moveto[16] = {
    0xe12e0,  /* e_MoveTo_Cppp */ 0x183d80, /* e_MoveTo_Cppn */ 0x183d40, /* e_MoveTo_Cpnp */
    0x183d00, /* e_MoveTo_Cpnn */ 0x183cc0, /* e_MoveTo_Cnpp */ 0x183c80, /* e_MoveTo_Cnpn */
    0x183c30, /* e_MoveTo_Cnnp */ 0x183be0, /* e_MoveTo_Cnnn */ 0xbef20,  /* e_MoveTo_Bppp */
    0x183b60, /* e_MoveTo_Bppn */ 0x183ad0, /* e_MoveTo_Bpnp */ 0xbe4c0,  /* e_MoveTo_Bpnn */
    0x183a40, /* e_MoveTo_Bnpp */ 0x1839b0, /* e_MoveTo_Bnpn */ 0x183920, /* e_MoveTo_Bnnp */
    0x183890, /* e_MoveTo_Bnnn */
};

/* ------------------------------------------------------------------------------------------
 * types and helpers */

typedef struct {
    int32_t x, y;
} ipt; /* a packed point, as the original keeps them in 64-bit stack slots */

static inline uint64_t pk(ipt p) { return (uint32_t)p.x | (uint64_t)(uint32_t)p.y << 32; }
static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t neg32(int32_t v) { return (int32_t)(0u - (uint32_t)v); }
static inline int32_t abs32(int32_t v) { /* neg + cmovs */
    int32_t t = neg32(v);
    return t < 0 ? v : t;
}
static inline int32_t hi16(int32_t v) { return (int16_t)((uint32_t)v >> 16); } /* movsx word [x + 2] */
/* cvttsd2si with 32-bit or 64-bit destination: out of range and NaN give the "indefinite" value */
static inline int32_t cvtt32(double d) {
    if (!(d > -2147483649.0 && d < 2147483648.0))
        return (int32_t)0x80000000;
    return (int32_t)d;
}
static inline int64_t cvtt64(double d) {
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0))
        return INT64_MIN;
    return (int64_t)d;
}
static void rangecheck_failure(void) { IDWR_FATAL("interp.c:156"); } /* __report_rangecheckfailure */

#define S8(o) (*(uint8_t *)(st + (o)))
#define S16(o) (*(uint16_t *)(st + (o)))
#define S32(o) (*(int32_t *)(st + (o)))
#define U32(o) (*(uint32_t *)(st + (o)))
#define SP(o) (*(void **)(st + (o)))
#define F32(p, o) (*(int32_t *)((uint8_t *)(p) + (o)))
#define FU32(p, o) (*(uint32_t *)((uint8_t *)(p) + (o)))
#define FP(p, o) (*(void **)((uint8_t *)(p) + (o)))

typedef void (*tfmlock_fn)(ipt *in, ipt *out, uint8_t *st); /* TfmLockPt* */
typedef void (*tfm_fn)(uint64_t pt, ipt *out, uint8_t *st); /* state + 0x580/0x588/0x598 */
typedef int32_t (*proc_fn)(uint8_t *st, void *client);       /* NewChar, ClosePath, EndChar */
typedef int32_t (*pt_fn)(ipt *pt, uint8_t *st, void *client); /* MoveTo, LineTo */
typedef int32_t (*curve_fn)(ipt *cur, ipt *p1, ipt *p2, ipt *p3, uint8_t *st, void *client);
typedef void *(*alloc_fn)(void *obj, uint64_t size);
typedef void (*free_fn)(void *obj, void *p);
typedef const uint8_t *(*getcs_fn)(int32_t which, void *desc, void *a3, const uint8_t **end);

#define ORIG(t, rva) CFF_ORIG(t, rva)

/* -DCFF_CONTROL_INTERPCOV (and -DCFF_CONTROL_SYNTH): counters of the interpreter's operators
 * and paths (a diagnostic build, read by cff_interp_cov; not a negative control) */
#if defined(CFF_CONTROL_INTERPCOV) || defined(CFF_CONTROL_SYNTH)
static uint32_t g_cov[256];
#define COV(k) (g_cov[(k) & 255]++)
__declspec(dllexport) uint32_t *cff_interp_cov(void) { return g_cov; }
#else
#define COV(k) ((void)0)
#endif

/* ------------------------------------------------------------------------------------------
 * DoType1InterpretCharString
 *
 *   st     the ATM state
 *   mem    the memory record (st + 0x450): +0x10 the charstring callback for seac/accents,
 *          +0x20 / +0x28 allocator objects (vtable: [0] alloc, [2] free)
 *   desc   _t_T1CharDataDesc: +0x00/+0x08 charstring start/end, +0x10/+0x18 subrs data/offsets,
 *          +0x30/+0x38 global subrs, +0x40, +0x44/+0x46 u8 offSize, +0x48/+0x50 bounds
 *   io     _t_CharIO: +0x00 the grow buffers (pointers), +0x08 the metrics record, +0x18,
 *          +0x1c flags out, +0x20/+0x24 stem widths out, +0x28/+0x2a u16
 *   a5     passed to CScanFill
 *   procs  path procs: NewChar, MoveTo, LineTo, CurveTo, ClosePath, EndChar
 *   p7     +0x00 flags, +0x08 seac record, +0x10 hint memory, +0x18 client data
 */
CFF_API uint32_t cff_DoType1InterpretCharString_impl(uint8_t *st, void *mem, void **desc, void **io, void *a5,
                                                     void **procs, uint32_t *p7) {
    void *const client = FP(p7, 0x18);
    void *const hintmem = FP(p7, 0x10);
    int32_t *const base = (int32_t *)SP(0x3a8);
    const uint32_t f2bc = U32(0x2bc);
    int32_t vsindex = S32(0x390);
    const int noflex = (p7[0] & 1) ^ 1;           /* rbp + 0x70 */
    const void *const noTfm = ORIG(void *, RVA_TfmLockPt_noTfm);

    int32_t *sp = 0;                               /* rsp + 0x50 */
    const uint8_t *cs = 0, *cs_end = 0;            /* rbp - 0x48, rbp - 8 */
    ipt cur = {0, 0}, dev = {0, 0}, dpt = {0, 0};  /* rsp + 0x58, rbp - 0x50, rbp + 0x58 */
    ipt width = {0, 0};                            /* rbp + 0x48 */
    int32_t sbx = 0, sby = 0;                      /* rbp - 0x60, rbp - 0x5c */
    uint32_t accent = 0;                           /* rbp + 0x10 */
    void *gc_tail = 0;                             /* rbp + 0x60 */
    uint64_t lastpt = 0, startpt = 0;              /* rdi, rsi */
    int path_open = 0;                             /* rbp - 0x68 */
    tfmlock_fn tfm = 0;                            /* rbp - 0x10 */
    uint64_t hintok;                               /* rbp - 0x18 */
    int64_t depth = 0;                             /* rsp + 0x70 */
    struct {
        const uint8_t *end, *ptr;
    } sub[11];                                     /* rbp + 0xd0 */
    int32_t stems[96][2];                          /* rbp + 0x180 */
    uint8_t sflag[96];                             /* rbp + 0x480 */
    int64_t hcount64 = 0;                          /* rbp + 0x30 */
    int32_t hcount = 0, vcount = 0;                /* rbp - 0x24, rbp */
    int hm_pending = 0, cm_pending = 0;            /* rbp + 0x14, rbp + 0x18 */
    int32_t F[20];                                 /* rbp + 0x80: the flex record */
    uint32_t op;
    int32_t n;
    uint8_t f28, f27, f58;

    /* b9b31 */
    U32(0x2c0) &= ~0x2000u;
    SP(0x450) = mem;
    ORIG(void (*)(void *), RVA_AdobeCFFResetBlend)(SP(0x388));
    if (SP(0x318))
        memset(SP(0x318), 0, (uint64_t)U32(0x324) << 2);
    {
        void **buf = (void **)FP(io[0], 0x20);
        if (FU32(buf, 8) < 0x2030) {
            void *obj = FP(SP(0x450), 0x20);
            if (buf[0])
                (*(free_fn *)((uint8_t *)*(void **)obj + 0x10))(obj, buf[0]);
            obj = FP(SP(0x450), 0x20);
            buf[0] = (*(alloc_fn *)*(void **)obj)(obj, 0x2030);
            buf = (void **)FP(io[0], 0x20);
            if (!buf[0]) {
                FU32(buf, 8) = 0;
                return 0xfc140c43;
            }
            FU32(buf, 8) = 0x2030;
        }
        buf = (void **)FP(io[0], 0x20);
        SP(0x468) = buf[0];
        SP(0x470) = (uint8_t *)buf[0] + 0x1018;
        ORIG(void (*)(void *), RVA_InitEdgeList)(buf[0]);
        ORIG(void (*)(void *), RVA_InitEdgeList)(SP(0x470));
    }
    SP(0x428) = 0;
    S32(0x440) = 1;
    SP(0x410) = st + 0x24;
    SP(0x418) = st + 0xe8;
    S32(0x420) = S32(0x20);
    S32(0x424) = S32(0xe4);
    f28 = (f2bc & 0xc0000000) != 0 && (p7[0] & 1);
    f27 = (p7[0] & 1) && (f2bc >> 30 & 1);
    f58 = (p7[0] & 1) && (f2bc >> 31 & 1);
    ORIG(void (*)(void *, int32_t, uint8_t *), RVA_SetGSMatrix)(st + 8, 0, st);
    {
        int32_t w = S32(0x378);
        if (w < 0) {
            COV(113);
            int32_t v;
            ipt t;
            if (S32(0x37c) <= 0) {
                v = ORIG(int32_t (*)(int32_t, int32_t), RVA_fixmul)(S32(0x2a8), 0xccd);
            } else {
                t.x = S32(0x37c);
                t.y = 0;
                ((tfm_fn)SP(0x580))(pk(t), &t, st);
                if (t.y != 0) {
                    double y = (double)t.y * (1.0 / 65536.0), x = (double)t.x * (1.0 / 65536.0);
                    v = cvtt32(sqrt(y * y + x * x) * 65536.0);
                } else
                    v = t.x;
            }
            S32(0x374) = v;
            if (S32(0x380) <= 0) {
                w = ORIG(int32_t (*)(int32_t, int32_t), RVA_fixmul)(S32(0x2ac), 0xccd);
            } else {
                t.x = 0;
                t.y = S32(0x380);
                ((tfm_fn)SP(0x580))(pk(t), &t, st);
                if (t.x != 0) {
                    double y = (double)t.y * (1.0 / 65536.0), x = (double)t.x * (1.0 / 65536.0);
                    w = cvtt32(sqrt(y * y + x * x) * 65536.0);
                } else
                    w = t.y;
            }
            S32(0x378) = w;
        }
        /* b9dba */
        F32(io, 0x20) = w;
        F32(io, 0x24) = S32(0x374);
    }
    U32(0x438) = f2bc & 0x10;
    if (f2bc & 0x10) {
        COV(114);
        ipt t = {0, 0x10000};
        ((tfm_fn)SP(0x598))(pk(t), &t, st);
        F32(SP(0x470), 0x10) = t.y;
        t.x = 0x10000;
        t.y = 0;
        ((tfm_fn)SP(0x598))(pk(t), &t, st);
        F32(SP(0x468), 0x10) = t.x;
    }
    /* b9de0 */
    S32(0x44c) = 0x10000;
    S32(0x434) = S32(0x2c8) == 2;
    S32(0x3e8) = 0;
    if ((f2bc & 0x20) && f28)
        goto L_bd387;
    S32(0x460) = 2;

L_b9e4b:
    if (io[1]) {
        FU32(io[1], 0x20) = 0xdeadbeef;
        FU32(io[1], 0x24) = 0xdeadbeef;
    }
    if (!(p7[0] & 0x10))
        S32(0x3d0) = 0;
    if (S32(0x3e0))
        ORIG(void (*)(uint8_t *), RVA_BlueBoost)(st);
    FU32(io, 0x1c) |= U32(0x2c0);
    S32(0x3dc) = 0x1312d00;
    if (S32(0x5d4) < 0) {
        COV(112);
        /* bd3b4 */
        int32_t e = S32(0x5d4), c;
        int32_t (*fixmul)(int32_t, int32_t) = ORIG(int32_t (*)(int32_t, int32_t), RVA_fixmul);
        if (S16(0) == 5) {
            c = S32(0x374);
            if (c < 0x30000)
                S32(0x5d4) = 0xf;
            else {
                if (c < 0x60000)
                    e = add32(((int32_t)((uint32_t)fixmul(0x1aaaa, 0x60000 - c) + 0x8000) >> 16), 10);
                else if (c < 0xa0000) {
                    S32(0x5d4) = 10;
                    goto L_b9eb3;
                } else if (c < 0xe0000)
                    e = (int32_t)((uint32_t)fixmul(0x28000, 0xe0000 - c) + 0x8000) >> 16;
                else
                    goto L_bd436;
                S32(0x5d4) = e;
            L_bd436:
                if (e < 0)
                    S32(0x5d4) = 0;
                else if (e > 0xf)
                    S32(0x5d4) = 0xf;
            }
        } else if (S16(0) == 4) {
            c = S32(0x374);
            if (c < 0x20000)
                S32(0x5d4) = 8;
            else {
                if (c < 0x40000)
                    e = add32(((int32_t)((uint32_t)fixmul(0x18000, 0x40000 - c) + 0x8000) >> 16), 5);
                else if (c < 0x70000) {
                    S32(0x5d4) = 5;
                    goto L_b9eb3;
                } else if (c < 0x90000)
                    e = (int32_t)((uint32_t)fixmul(0x28000, 0x90000 - c) + 0x8000) >> 16;
                else
                    goto L_bd4d5;
                S32(0x5d4) = e;
            L_bd4d5:
                if (e < 0)
                    S32(0x5d4) = 0;
                else if (e > 8)
                    S32(0x5d4) = 8;
            }
        }
    }
L_b9eb3:
    hintok = f28;

    /* ---- a pass over the charstring ---- */
L_b9f1d:
    S32(0x3ec) = 0;
    S32(0x3f0) = 0;
    SP(0x4a8) = 0;
    SP(0x4b0) = 0;
    if (S32(0x33c)) {
        S32(0x3e8) = 1;
        S32(0x460) = 2;
    }
    width.x = width.y = 0;
    cur.x = cur.y = 0;
    S32(0x3b8) = 0;
    S32(0x3bc) = 0;
    sby = 0;
    sbx = 0;
    vcount = 0;
    hcount = 0;
    hcount64 = 0;
    hm_pending = 0;
    cm_pending = 0;
    S32(0x43c) = 0;
    S32(0x3c0) = 0;
    if (S32(0x384))
        S32(0x3b8) = 1;
    accent = 0;
    cs_end = 0;
    if (p7[0] & 0x18) {
        /* bd4fe: the base or accent glyph of a seac */
            COV(111);
        int32_t *r = (int32_t *)FP(p7, 8);
        int32_t v;
        if (p7[0] & 0x10)
            v = r[2];
        else {
            v = r[3];
            accent = (uint16_t)((uint32_t)(r[0] + 0x8000) >> 16) | (uint32_t)(uint16_t)((uint32_t)(r[1] + 0x8000) >> 16) << 16;
        }
        F32(desc, 0x40) = v;
        cs = ((getcs_fn)FP(mem, 0x10))(2, desc, FP(p7, 0x10), &cs_end);
    } else {
        cs = (const uint8_t *)desc[0];
        cs_end = (const uint8_t *)desc[1];
    }
    if (!cs)
        return 0xfe140d09;
    sub[0].end = cs_end;
    sub[0].ptr = cs;
    path_open = 1;
    depth = 0;
    if (!hintok)
        tfm = ORIG(tfmlock_fn, RVA_TfmLockPt3);
    else if (S32(0x540) == 1 && S32(0x548) <= 0x10000 && S32(0x554) <= 0x10000 && S32(0x460) != 0)
        tfm = (tfmlock_fn)noTfm;
    else
        tfm = ORIG(tfmlock_fn, RVA_TfmLockPt1);
    if (procs[0] && !((proc_fn)procs[0])(st, client))
        return 0xfc140d1e;
    sp = base;
    if (SP(0x3b0))
        memset(SP(0x3b0), 0, (uint64_t)U32(0x3a0) << 2);

    /* ---- the operator loop ---- */
L_ba043:
    if (S32(0x384)) {
        /* bb291: CFF2, an implicit return / endchar at the end of a (sub)charstring */
            COV(105);
        if (cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
            return 0xfe140d2d;
        if (cs > cs_end)
            op = depth > 0 ? 0xb : 0xe;
        else {
            op = *cs++;
            if (op == 0xb || op == 0xe)
                op = 0;
        }
    } else {
        if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
            return 0xfe140d39;
        op = *cs++;
    }
    /* ba080 */
    if (--S32(0x3dc) == 0) {
        U32(0x2c0) |= 0x2000;
        return 0xfe140d44;
    }
    if (sp < base)
        return 0xfe140d47;
    n = (int32_t)(sp - base);

    COV(op < 32 ? op : 250);
    switch (op) {
    case 0:
    case 2:
    case 13:
    case 17:
        goto L_bd979;
    case 1:
        goto L_bb76c;
    case 18:
        S32(0x440) = 0;
        goto L_bb76c;
    case 3:
        goto L_bbbf2;
    case 23:
        S32(0x440) = 0;
        goto L_bbbf2;
    case 19: /* hintmask */
        if (sp == base)
            goto L_bb457;
        hm_pending = 1;
        goto L_bbbf2;
    case 20: /* cntrmask */
        if (sp == base)
            goto L_bbccc;
        cm_pending = 1;
        goto L_bbbf2;
    case 4: /* vmoveto */
        if (n == 1) {
            base[7] = base[0];
            base[0] = -1;
            base[6] = 0;
        } else if (n < 1)
            return 0xfe140dad;
        else if (n == 2) {
            base[6] = 0;
            base[7] = base[1];
        } else
            return 0xfe140db6;
        goto L_ba342;
    case 21: /* rmoveto */
        if (n == 2) {
            int32_t a = base[0];
            base[0] = -1;
            base[6] = a;
            base[7] = base[1];
        } else if (n == 3) {
            base[6] = base[1];
            base[7] = base[2];
        } else
            return 0xfe1412a0;
        goto L_ba342;
    case 22: /* hmoveto */
        if (n == 1) {
            base[6] = base[0];
            base[0] = -1;
        } else if (n == 2)
            base[6] = base[1];
        else
            return 0xfe1412ad;
        base[7] = 0;
        goto L_ba342;
    case 5:
        goto L_badb0;
    case 6:
    case 7: { /* hlineto, vlineto */
        int horiz = op == 6;
        if (n < 1 || S32(0x3b8) == 0)
            return 0xfe140e65;
        for (int64_t i = 0; i < n; i++) {
            if (horiz)
                cur.x = add32(cur.x, base[i]);
            else
                cur.y = add32(cur.y, base[i]);
            horiz ^= 1;
            if (S32(0x460) == 0) {
                ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(lastpt, pk(cur), st);
                lastpt = pk(cur);
                dev = cur;
                continue;
            }
            if (SP(0x428) == noTfm) {
                COV(106);
                if (!horiz)
                    dev.x = ORIG(int32_t (*)(void *, int32_t), RVA_Map)(SP(0x468), cur.x);
                else
                    dev.y = ORIG(int32_t (*)(void *, int32_t), RVA_Map)(SP(0x470), cur.y);
            } else
                tfm(&cur, &dev, st);
            if (procs[2] && !((pt_fn)procs[2])(&dev, st, client))
                return 0xfc140e7d;
        }
        goto L_ba89f;
    }
    case 8:
    case 24:
        goto L_bab13;
    case 25: { /* rlinecurve */
        int32_t *p = base;
        if (n < 8 || (n & 1) || S32(0x3b8) == 0)
            return 0xfe1412b7;
        for (int32_t i = 0; i < n - 6; i += 2, p += 2) {
            cur.x = add32(cur.x, p[0]);
            cur.y = add32(cur.y, p[1]);
            if (S32(0x460) != 0) {
                tfm(&cur, &dev, st);
                if (procs[2] && !((pt_fn)procs[2])(&dev, st, client))
                    return 0xfc1412c4;
            } else {
                ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(lastpt, pk(cur), st);
                lastpt = pk(cur);
                dev = cur;
            }
        }
        for (int k = 0; k < 6; k++)
            base[k] = base[n - 6 + k];
        sp = base + 6;
        goto L_bab13;
    }
    case 26:
        goto L_bd047;
    case 27:
        goto L_bce38;
    case 28: /* shortint */
        if (sp >= base + U32(0x3a0))
            return 0xfe141356;
        if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
            return 0xfe141357;
        {
            uint32_t v = *cs++;
            if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
                return 0xfe141359;
            v = (v << 8 | *cs++) << 16;
            *sp++ = (int32_t)v;
        }
        goto L_ba043;
    case 30:
    case 31:
        goto L_bb886;
    case 9: /* closepath */
        if (S32(0x384))
            goto L_ba8a3;
        if (sp != base)
            return 0xfe140ed9;
        if (S32(0x460) == 0) {
            ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(pk(cur), startpt, st);
            goto L_ba8a3;
        }
        if (!path_open && procs[4] && !((proc_fn)procs[4])(st, client))
            return 0xfc140ee1;
        path_open = 1;
        goto L_ba8a3;
    case 10: { /* callsubr */
        int32_t idx;
        if (n < 1)
            return 0xfe140efc;
        if (SP(0x3b0) && ((int32_t *)SP(0x3b0))[n - 1] != 0)
            return 0xfe140eff;
        sp--;
        idx = add32(hi16(*sp), S32(0x320));
        if (idx >= S32(0x2c4) || idx < 0)
            return 0xfe140f02;
        if (1) {
            const uint8_t *offs = (const uint8_t *)desc[3], *data = (const uint8_t *)desc[2];
            uint8_t osz = *((uint8_t *)desc + 0x44);
            if (0) {
            case 29: /* callgsubr */
                if (n < 1)
                    return 0xfe140eee;
                if (SP(0x3b0) && ((int32_t *)SP(0x3b0))[n - 1] != 0)
                    return 0xfe140ef1;
                sp--;
                idx = add32(hi16(*sp), S32(0x328));
                if (idx >= S32(0x32c) || idx < 0)
                    return 0xfe140ef4;
                offs = (const uint8_t *)desc[7];
                data = (const uint8_t *)desc[6];
                osz = *((uint8_t *)desc + 0x46);
            }
            /* ba946 */
            uint32_t o0;
            uint64_t o1;
            if (depth == 10)
                return 0xfe140f09;
            if (!offs)
                return 0xfe140f11;
            if (osz == 4) {
                const uint8_t *q = offs + (int64_t)(int32_t)((uint32_t)idx * 4);
                o0 = (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3];
            } else if (osz == 3) {
                const uint8_t *q = offs + (int64_t)(int32_t)((uint32_t)idx * 3);
                o0 = (uint32_t)q[0] << 16 | (uint32_t)q[1] << 8 | q[2];
            } else if (osz == 2) {
                const uint8_t *q = offs + (int64_t)(int32_t)((uint32_t)idx * 2);
                o0 = (uint32_t)q[0] << 8 | q[1];
            } else if (osz == 1)
                o0 = offs[(uint32_t)idx];
            else
                return 0xfe140f11;
            if (o0 == 0)
                return 0xfe140f11;
            sub[depth].ptr = cs;
            depth++;
            cs = data + o0;
            {
                int32_t j = add32(idx, 1);
                if (osz == 4) {
                    const uint8_t *q = offs + (int64_t)(int32_t)((uint32_t)j * 4);
                    o1 = (uint64_t)q[0] << 24 | (uint64_t)q[1] << 16 | (uint64_t)q[2] << 8 | q[3];
                } else if (osz == 3) {
                    const uint8_t *q = offs + (int64_t)(int32_t)((uint32_t)j * 3);
                    o1 = (uint64_t)q[0] << 16 | (uint64_t)q[1] << 8 | q[2];
                } else if (osz == 2) {
                    const uint8_t *q = offs + (int64_t)(int32_t)((uint32_t)j * 2);
                    o1 = (uint64_t)q[0] << 8 | q[1];
                } else
                    o1 = offs[(uint64_t)(uint32_t)idx + 1];
            }
            cs_end = data - 1 + o1;
            sub[depth].end = cs_end;
            sub[depth].ptr = cs;
        }
        goto L_ba043;
    }
    case 11: /* return */
        if (depth <= 0)
            return 0xfe140f2a;
        depth--;
        cs = sub[depth].ptr;
        cs_end = sub[depth].end;
        goto L_ba043;
    case 12:
        goto L_ba22f;
    case 14:
        goto L_bb00d;
    case 15: /* vsindex */
        if (!S32(0x384) || n < 1)
            return 0xfe1413b1;
        if (ORIG(int32_t (*)(void *), RVA_AdobeCFFIsUsedBlend)(SP(0x388)))
            return 0xfe1413b5;
        sp--;
        vsindex = hi16(*sp);
        goto L_ba8a3;
    case 16: { /* blend */
        int64_t k;
        uint32_t r;
        if (!S32(0x384) || n < 1)
            return 0xfe1413c1;
        if (SP(0x3b0) && ((int32_t *)SP(0x3b0))[n - 1] != 0)
            return 0xfe1413c6;
        sp--;
        k = hi16(*sp);
        if (k > (int64_t)(sp - base))
            return 0xfe1413c9;
        if (ORIG(int32_t (*)(void *, int32_t, int32_t, void *), RVA_AdobeCFFCheckBlendVector)(SP(0x388), vsindex,
                                                                                            S32(0x394), SP(0x398)) &&
            (r = ORIG(uint32_t (*)(void *, int32_t, int32_t, void *), RVA_AdobeCFFBuildBlendVector)(
                 SP(0x388), vsindex, S32(0x394), SP(0x398))) != 0)
            return r;
        if (ORIG(int32_t (*)(void *, int32_t, int32_t **), RVA_AdobeCFFDoCharStringBlend)(SP(0x388), (int32_t)k, &sp))
            return 0xfe1413d2;
        goto L_ba043;
    }
    default:
        break;
    }

    /* ba10e: a number */
    if (sp >= base + U32(0x3a0))
        return 0xfe1413e9;
    *sp++ = (int32_t)((op - 0x8b) << 16);
    if (op <= 0xf6)
        goto L_ba043;
    sp--;
    if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
        return op <= 0xfa ? 0xfe1413f0 : op <= 0xfe ? 0xfe1413f4 : 0xfe1413f7;
    if (op <= 0xfa)
        *sp = (int32_t)((((op + 9) << 24) | (uint32_t)*cs << 16) + 0x6c0000);
    else if (op <= 0xfe)
        *sp = (int32_t)((0xffffff94u - (((op << 8) - 0xfb00) | *cs)) << 16);
    else {
        uint32_t v = *cs++;
        if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
            return 0xfe1413f8;
        v = v << 8 | *cs++;
        if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
            return 0xfe1413f9;
        v = v << 8 | *cs++;
        if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
            return 0xfe1413fa;
        v = v << 8 | *cs;
        *sp = (int32_t)v;
    }
    sp++;
    cs++;
    goto L_ba043;

    /* ---- escape operators ---- */
L_ba22f : {
    uint8_t e;
    if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
        return 0xfe140f31;
    e = *cs++;
    if (S32(0x384)) {
        if (e == 0x22 || e == 0x23 || e == 0x24 || e == 0x25)
            goto L_esc;
        goto L_ba8a3;
    }
L_esc:
    COV(32 + (e < 64 ? e : 63));
    switch (e) {
    case 0: /* dotsection */
        if (sp != base)
            return 0xfe140f40;
        goto L_ba8a3;
    case 3: /* and */
        if (n < 2)
            return 0xfe140ffb;
        sp--;
        sp[-1] = (*sp != 0 && sp[-1] != 0) ? 0x10000 : 0;
        goto L_ba043;
    case 4: /* or */
        if (n < 2)
            return 0xfe141002;
        sp--;
        sp[-1] = (*sp != 0 || sp[-1] != 0) ? 0x10000 : 0;
        goto L_ba043;
    case 5: /* not */
        if (n < 1)
            return 0xfe141010;
        sp[-1] = sp[-1] == 0 ? 0x10000 : 0;
        goto L_ba043;
    case 9: /* abs */
        if (n < 1)
            return 0xfe14101a;
        sp[-1] = abs32(sp[-1]);
        goto L_ba043;
    case 10: /* add */
        if (n < 2)
            return 0xfe140f9f;
        sp--;
        sp[-1] = add32(sp[-1], *sp);
        goto L_ba043;
    case 11: /* sub */
        if (n < 2)
            return 0xfe140fa9;
        sp--;
        sp[-1] = sub32(sp[-1], *sp);
        goto L_ba043;
    case 12: /* div */
        if (n < 2)
            return 0xfe141066;
        sp -= 2;
        *sp = ORIG(int32_t (*)(int32_t, int32_t), RVA_fixdiv)(sp[0], sp[1]);
        sp++;
        goto L_ba043;
    case 14: /* neg */
        if (n < 1)
            return 0xfe141015;
        sp[-1] = neg32(sp[-1]);
        goto L_ba043;
    case 15: /* eq */
        if (n < 2)
            return 0xfe141009;
        sp--;
        sp[-1] = *sp == sp[-1] ? 0x10000 : 0;
        goto L_ba043;
    case 18: /* drop */
        if (n < 1)
            return 0xfe14102b;
        sp--;
        goto L_ba043;
    case 20: { /* put */
        int32_t i, v;
        if (n < 2)
            return 0xfe140fb3;
        sp--;
        i = *sp >> 16;
        sp--;
        v = *sp;
        if (!SP(0x318)) {
            uint32_t c = U32(0x324);
            void *obj, *p;
            if (c - 1 > 0xfffe)
                return 0xff140fb9;
            obj = FP(mem, 0x28);
            if (!*(void **)*(void **)obj)
                return 0xfc140fb9;
            p = (*(alloc_fn *)*(void **)obj)(obj, c << 2);
            SP(0x318) = p;
            if (!p)
                return 0xfc140fb9;
            memset(p, 0, c << 2);
        }
        if ((uint32_t)i >= U32(0x324))
            return 0xfe140fbb;
        ((int32_t *)SP(0x318))[(uint32_t)i] = v;
        goto L_ba043;
    }
    case 21: { /* get */
        int32_t i;
        if (n < 1)
            return 0xfe140fc2;
        sp--;
        i = hi16(*sp);
        if (!SP(0x318)) {
            uint32_t c = U32(0x324);
            void *obj, *p;
            if (c - 1 > 0xfffe)
                return 0xff140fc7;
            obj = FP(mem, 0x28);
            if (!*(void **)*(void **)obj)
                return 0xfc140fc7;
            p = (*(alloc_fn *)*(void **)obj)(obj, c << 2);
            SP(0x318) = p;
            if (!p)
                return 0xfc140fc7;
            memset(p, 0, c << 2);
        }
        if ((uint32_t)i >= U32(0x324))
            return 0xfe140fc9;
        *sp++ = ((int32_t *)SP(0x318))[(uint32_t)i];
        goto L_ba043;
    }
    case 22: { /* ifelse */
        int32_t v2, v1, s2, s1;
        if (n < 4)
            return 0xfe140fd2;
        v2 = *--sp;
        v1 = *--sp;
        s2 = *--sp;
        s1 = *--sp;
        *sp++ = v1 > v2 ? s2 : s1;
        goto L_ba043;
    }
    case 23: { /* random */
        int32_t r;
        if (sp >= base + U32(0x3a0))
            return 0xfe140fdd;
        r = (int32_t)((uint32_t)S32(0x2f4) * 0x41c64e6du + 0x3039);
        S32(0x2f4) = r;
        *sp++ = (int32_t)((uint32_t)(r >> 16) & 0xffff) + 1;
        FU32(io, 0x1c) |= 0x100;
        goto L_ba043;
    }
    case 24: /* mul */
        if (n < 2)
            return 0xfe140ff1;
        sp -= 2;
        *sp = ORIG(int32_t (*)(int32_t, int32_t), RVA_fixmul)(sp[1], sp[0]);
        sp++;
        goto L_ba043;
    case 26: { /* sqrt */
        double d;
        if (n < 1)
            return 0xfe14101f;
        d = (double)sp[-1] * 0x1p-30;
        d = sqrt(d) * 0x1p30 + 0.5;
        sp[-1] = (int32_t)((uint32_t)cvtt64(d) + 0x40) >> 7 & 0x1ffffff;
        goto L_ba043;
    }
    case 27: /* dup */
        if (n < 1 || sp >= base + U32(0x3a0))
            return 0xfe141025;
        *sp = sp[-1];
        sp++;
        goto L_ba043;
    case 28: { /* exch */
        int32_t t;
        if (n < 2)
            return 0xfe141030;
        t = sp[-1];
        sp[-1] = sp[-2];
        sp[-2] = t;
        goto L_ba043;
    }
    case 29: { /* index */
        int32_t i;
        if (n < 2)
            return 0xfe14103b;
        i = hi16(sp[-1]);
        if (i < 0)
            i = 0;
        if (sp - 2 - (int64_t)i < base)
            return 0xfe141040;
        sp[-1] = *(sp - 2 - (int64_t)i);
        goto L_ba043;
    }
    case 30: { /* roll */
        int32_t j, m, r;
        int64_t cnt;
        if (n < 2)
            return 0xfe14104a;
        j = hi16(*--sp);
        cnt = hi16(*--sp);
        if (cnt < 0 || sp - cnt < base)
            return 0xfe14104e;
        if (cnt == 0)
            goto L_ba043;
        m = (int32_t)cnt;
        r = j % m;
        if (r < 0) {
            for (int32_t t = -r; t > 0; t--) {
                int32_t b = *(sp - cnt);
                for (int64_t k = 0; k < cnt - 1; k++)
                    *(sp - cnt + k) = *(sp - cnt + k + 1);
                sp[-1] = b;
            }
        } else {
            for (int32_t t = r; t > 0; t--) {
                int32_t top = sp[-1];
                for (int64_t k = 0; k < cnt - 1; k++)
                    sp[-1 - k] = sp[-2 - k];
                *(sp - cnt) = top;
            }
        }
        goto L_ba043;
    }
    case 0x22:
    case 0x23:
    case 0x24:
    case 0x25:
        goto L_flex;
    case 0x26: /* the start of hinting in the middle of a charstring */
        if (!hintok || S32(0x460) != 2 || S32(0x3e8) != 0)
            goto L_ba043;
        goto L_bd387;
    default:
        return 0xfe1410e5;
    }

    /* ---- flex, hflex, hflex1, flex1 ---- */
L_flex : {
    int32_t thr = 0x320000, f4x, f4y, f8x, f8y;
    if (e == 0x23) { /* flex */
        if (n != 13)
            return 0xfe141071;
        thr = base[12];
        F[4] = add32(base[0], cur.x);
        F[5] = add32(base[1], cur.y);
        F[6] = add32(base[2], F[4]);
        F[7] = add32(base[3], F[5]);
        F[8] = add32(base[4], F[6]);
        F[9] = add32(base[5], F[7]);
        F[12] = add32(base[6], F[8]);
        F[13] = add32(base[7], F[9]);
        F[14] = add32(base[8], F[12]);
        F[15] = add32(base[9], F[13]);
        F[16] = add32(base[10], F[14]);
        F[17] = add32(base[11], F[15]);
        f8x = F[16];
        f8y = F[17];
    } else if (e == 0x22) { /* hflex */
        int32_t y;
        if (n != 7)
            return 0xfe1410c4;
        F[17] = F[15] = F[5] = cur.y;
        y = add32(base[2], cur.y);
        F[13] = F[9] = F[7] = y;
        F[4] = add32(base[0], cur.x);
        F[6] = add32(base[1], F[4]);
        F[8] = add32(base[3], F[6]);
        F[12] = add32(base[4], F[8]);
        F[14] = add32(base[5], F[12]);
        f8x = add32(base[6], F[14]);
        f8y = cur.y;
        F[16] = f8x;
    } else if (e == 0x24) { /* hflex1 */
        int32_t y;
        if (n != 9)
            return 0xfe1410d0;
        F[17] = cur.y;
        F[5] = add32(base[1], cur.y);
        y = add32(base[3], F[5]);
        F[7] = F[9] = F[13] = y;
        F[15] = add32(base[7], y);
        F[4] = add32(base[0], cur.x);
        F[6] = add32(base[2], F[4]);
        F[8] = add32(base[4], F[6]);
        F[12] = add32(base[5], F[8]);
        F[14] = add32(base[6], F[12]);
        f8x = add32(base[8], F[14]);
        f8y = cur.y;
        F[16] = f8x;
    } else { /* flex1 */
        if (n != 11)
            return 0xfe1410b0;
        F[4] = add32(base[0], cur.x);
        F[6] = add32(base[2], F[4]);
        F[8] = add32(base[4], F[6]);
        F[12] = add32(base[6], F[8]);
        F[14] = add32(base[8], F[12]);
        F[5] = add32(base[1], cur.y);
        F[7] = add32(base[3], F[5]);
        F[9] = add32(base[5], F[7]);
        F[13] = add32(base[7], F[9]);
        F[15] = add32(base[9], F[13]);
        if (abs32(sub32(F[14], cur.x)) > abs32(sub32(F[15], cur.y))) {
            f8x = add32(F[14], base[10]);
            f8y = cur.y;
        } else {
            f8x = cur.x;
            f8y = add32(F[15], base[10]);
        }
        F[16] = f8x;
        F[17] = f8y;
    }
    /* ba613 */
    f4x = F[8];
    f4y = F[9];
    if (S32(0x3b8) == 0)
        return 0xfe141082;
    if (abs32(sub32(f8x, cur.x)) < abs32(sub32(f8y, cur.y))) {
        F[0] = cur.x;
        F[1] = f4y;
    } else {
        F[1] = cur.y;
        F[0] = f4x;
    }
    F[2] = F[0];
    F[3] = F[1];
    F[10] = F[8];
    F[11] = F[9];
    F[18] = F[16];
    F[19] = F[17];
    cur.x = F[16];
    cur.y = F[17];
    {
        static const int idx[7] = {2, 4, 6, 10, 12, 14, 16};
        for (int k = 0; k < 7; k++) {
            ipt *q = (ipt *)&F[idx[k]];
            tfm(q, q, st);
            ((tfm_fn)SP(0x588))(pk(*q), q, st);
        }
    }
    dpt.x = F[2];
    dpt.y = F[3];
    if (S32(0x460) == 0)
        goto L_ba89f;
    if (noflex == 0 && hintok != 0) {
        COV(109);
        if (!ORIG(int32_t (*)(int32_t *, int32_t, ipt *, void *, void *, void *, int32_t, int32_t, int32_t, uint8_t *),
                  RVA_FlexProc)(F, thr, &dev, procs[2], procs[3], client, S32(4), S32(0x44c), S32(0x434), st))
            return 0xfc1410a6;
    } else {
        COV(110);
        if (!ORIG(int32_t (*)(int32_t *, ipt *, void *, uint8_t *, void *), RVA_FlexProc2)(F, &dev, procs[3], st,
                                                                                           client))
            return 0xfc1410a0;
    }
    dpt.x = F[2];
    dpt.y = F[3];
    goto L_ba89f;
}
}

    /* ---- moveto ---- */
L_ba342:
    sp = base + 9;
L_ba350:
    if (S32(0x3c0) != 0) {
        /* bc556 */
        if (S32(0x460) == 0) {
            COV(104);
        if (!ORIG(int32_t (*)(void *, void *, int32_t, int32_t, uint8_t *, void *), RVA_GlobalColoringGC)(
                    SP(0x4a8), FP(io[0], 0x10), S32(0x3f0), S32(0x3ec), st, hintmem)) {
                S32(0x3e8) = 1;
                S32(0x460) = 2;
                FU32(io, 0x1c) |= 0x2000;
                goto L_b9f1d;
            }
            S32(0x460) = 3;
            SP(0x4b0) = 0;
            SP(0x400) = 0;
            if (S32(0x3e8) != 0) {
                S32(0x460) = 2;
                FU32(io, 0x1c) |= 0x2000;
                goto L_b9f1d;
            }
            if (procs[0] && !((proc_fn)procs[0])(st, client))
                return 0xfc140dcf;
        }
    }
    /* ba361 */
    if (S32(0x3b8) == 0) {
        /* bb065: no width yet */
        if (base[0] == -1) {
            if (FU32(io, 0x18) & 0x1000) {
                base[0] = 0;
                base[2] = 0;
                base[3] = S32(0x304);
                base[1] = S32(0x2fc);
            } else {
                base[1] = 0;
                base[3] = 0;
                base[2] = S32(0x300);
                base[0] = S32(0x2f8);
            }
        } else {
            int32_t w = add32(base[0], S32(0x370));
            base[0] = 0;
            base[1] = 0;
            base[2] = w;
            base[3] = 0;
        }
        goto L_bb32d;
    }
L_ba376:
    if (S32(0x3bc) == 0) {
        /* bb08d: lock the stems at the first moveto */
        S32(0x3bc) = 1;
        if (f28) {
            if (f58) {
                int64_t k = 0;
                int32_t i = 0;
                for (int64_t s = 0; s < hcount64; s++, i++) {
                    if (sflag[s]) {
                        base[k] = stems[s][0];
                        base[k + 1] = stems[s][1];
                        k += 2;
                    } else {
                        base[5] = stems[s][1];
                        base[4] = stems[s][0];
                        ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RBLock)(base + 4, sby, st, hintmem);
                    }
                    if (k == 6) {
                        ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RVLock)(base, sby, st, hintmem);
                        k = 0;
                    }
                    if (i == hcount - 1 && k) {
                        ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RBLock)(base, sby, st, hintmem);
                        if (k == 4)
                            ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RBLock)(base + 2, sby, st, hintmem);
                    }
                    if (S32(0x3e8) && S32(0x460) != 2)
                        goto L_b9f01;
                }
            }
            /* bc5da */
            if (f27) {
                int32_t total = hcount + vcount;
                int64_t k = 0;
                int64_t s = hcount64;
                for (int32_t i = hcount; i < total; i++, s++) {
                    if (sflag[s]) {
                        base[k] = stems[s][0];
                        base[k + 1] = stems[s][1];
                        k += 2;
                    } else {
                        base[5] = stems[s][1];
                        base[4] = stems[s][0];
                        ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RXLock)(base + 4, sbx, st, hintmem);
                    }
                    if (k == 6) {
                        ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RMLock)(base, sbx, st, hintmem);
                        k = 0;
                    }
                    if (i == total - 1 && k) {
                        ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RXLock)(base, sbx, st, hintmem);
                        if (k == 4)
                            ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RXLock)(base + 4, sbx, st, hintmem);
                    }
                    if (S32(0x3e8) && S32(0x460) != 2)
                        goto L_b9f01;
                }
            }
        }
    }
    /* ba383 */
    cur.x = add32(cur.x, base[6]);
    cur.y = add32(cur.y, base[7]);
    tfm(&cur, &dpt, st);
    dev = dpt;
    lastpt = pk(cur);
    startpt = lastpt;
    if (S32(0x460) == 0)
        goto L_ba8a3;
    if (!path_open && procs[4] && !((proc_fn)procs[4])(st, client))
        return 0xfc140e34;
    if (procs[1] && !((pt_fn)procs[1])(&dev, st, client))
        return 0xfc140e37;
    path_open = 1;
    goto L_ba8a3;

    /* ---- the width and sidebearing (first stem, moveto or endchar) ---- */
L_bb32d : {
    int64_t used;
    if (S32(0x3b8) == 1)
        return 0xfe140f48;
    S32(0x3b8) = 1;
    sbx = cur.x = base[0];
    sby = cur.y = base[1];
    width.x = base[2];
    width.y = base[3];
    if (!(p7[0] & 0x10)) {
        *(uint64_t *)io[1] = pk(cur);
        *(uint64_t *)((uint8_t *)io[1] + 8) = pk(width);
    }
    ORIG(void (*)(uint8_t *, uint64_t, void *), RVA_BCTfmP)(st, pk(cur), (uint8_t *)io[1] + 0x10);
    ORIG(void (*)(uint8_t *, uint64_t, void *), RVA_BCTfmP)(st, pk(width), (uint8_t *)io[1] + 0x18);
    if (FU32(io[1], 0x20) == 0xdeadbeef)
        ORIG(void (*)(uint8_t *, uint64_t, void *), RVA_BCTfmP)(st, pk(width), (uint8_t *)io[1] + 0x20);
    ((tfm_fn)SP(0x580))(pk(cur), &dev, st);
    if (procs[1] && !((pt_fn)procs[1])(&dev, st, client))
        return 0xfc140f7c;
    path_open = 1;
    used = sp - base;
    if (used == 9) {
        base[0] = base[6];
        base[1] = base[7];
        goto L_ba376;
    }
    if (used == 5)
        goto L_bc82b;
    if (used == 8) {
        for (int k = 0; k < 4; k++)
            base[k] = base[k + 4];
        goto L_bdc56;
    }
    if (hm_pending)
        goto L_bb457;
    goto L_bbcbe;
}

    /* ---- hstem, hstemhm ---- */
L_bb76c : {
    int32_t acc = 0;
    int odd = n & 1;
    if (n < 2)
        return 0xfe140d5d;
    for (int64_t i = odd; i < n; i += 2) {
        if (hcount64 >= 0x60 || (odd && S32(0x3b8) != 0))
            return 0xfe140d60;
        if (f58 && vcount != 0) {
            FU32(io, 0x1c) |= 0x2000;
            return 0xfe140d64;
        }
        acc = add32(acc, base[i]);
        stems[hcount64][0] = acc;
        acc = add32(acc, base[i + 1]);
        stems[hcount64][1] = base[i + 1];
        if (hcount64 >= 0x60)
            rangecheck_failure();
        hcount++;
        sflag[hcount64] = 0;
        hcount64++;
    }
    if (odd)
        goto L_bc921;
    if (S32(0x3b8) != 0)
        goto L_ba8a3;
    goto L_bb30e;
}

    /* ---- vstem, vstemhm, and the implicit vstems of hintmask and cntrmask ---- */
L_bbbf2 : {
    int32_t acc = 0, total;
    int odd = n & 1;
    int64_t s;
    if (n < 2)
        return 0xfe140d8e;
    s = hcount64 + vcount;
    total = hcount + vcount;
    for (int64_t i = odd; i < n; i += 2) {
        if (total >= 0x60 || (odd && S32(0x3b8) != 0))
            return 0xfe140d91;
        acc = add32(acc, base[i]);
        stems[s][0] = acc;
        stems[s][1] = base[i + 1];
        if (s >= 0x60)
            rangecheck_failure();
        vcount++;
        sflag[s] = 0;
        s++;
        acc = add32(acc, base[i + 1]);
        total++;
    }
    if (odd)
        goto L_bc921;
    if (S32(0x3b8) == 0)
        goto L_bb30e;
    if (hm_pending)
        goto L_bb457;
    goto L_bbcbe;
}
L_bc921:
    base[2] = add32(S32(0x370), base[0]);
    goto L_bb315;
L_bb30e:
    base[2] = S32(0x300);
L_bb315:
    base[3] = 0;
    base[0] = 0;
    base[1] = 0;
    sp = base + 4;
    goto L_bb32d;
L_bbcbe:
    if (cm_pending == 0)
        goto L_ba8a3;

    /* ---- cntrmask ---- */
L_bbccc : {
    COV(103);
    void *prev = 0;
    int32_t total;
    uint8_t mask = 0;
    cm_pending = 0;
    if (S32(0x3c0) == 0)
        gc_tail = 0;
    total = vcount + hcount;
    for (int32_t i = 0; i < total; i++) {
        int64_t s = i;
        if ((i & 7) == 0) {
            if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
                return 0xfe141204;
            mask = *cs++;
        }
        if (S32(0x460) != 0) {
            if ((int8_t)mask < 0 && S32(0x460) == 2 && S32(0x3e8) == 0)
                sflag[s] = 1;
            else {
                if (s >= 0x60)
                    rangecheck_failure();
                sflag[s] = 0;
            }
            mask <<= 1;
            continue;
        }
        /* bc26d */
        if (s >= 0x60)
            rangecheck_failure();
        sflag[s] = 0;
        if ((int8_t)mask < 0) {
            int32_t a = stems[s][0], e = add32(stems[s][1], a), ish = s < hcount64;
            uint8_t *stem = ORIG(uint8_t *(*)(int32_t, int32_t, int32_t, uint8_t *), RVA_GCFind)(a, e, ish, st);
            if (!stem) {
                stem = ORIG(uint8_t *(*)(void **, void **, uint8_t *, void *), RVA_GCNew)(&gc_tail, &prev, st, hintmem);
                if (!stem)
                    goto L_b9ef6;
                if (SP(0x4a8) == 0)
                    SP(0x4a8) = stem;
                else {
                    if (!gc_tail)
                        goto L_b9ef6;
                    *(void **)gc_tail = stem;
                }
                F32(stem, 0x10) = e;
                {
                    uint32_t fl = FU32(stem, 0x48);
                    F32(stem, 0xc) = a;
                    FP(stem, 0) = 0;
                    F32(stem, 0x1c) = 0;
                    F32(stem, 0x20) = 0x3e80000;
                    FU32(stem, 0x48) = (((fl & ~1u) | (uint32_t)ish) & ~8u) | 4;
                }
                if (s < hcount64) {
                    ORIG(void (*)(int32_t, int32_t *, uint8_t *), RVA_GetDTfmY)(stems[s][0], (int32_t *)(stem + 0x14), st);
                    ORIG(void (*)(int32_t, int32_t *, uint8_t *), RVA_GetDTfmY)(F32(stem, 0x10), (int32_t *)(stem + 0x18), st);
                    F32(stem, 0x2c) = F32(stem, 8) = sub32(F32(stem, 0x18), F32(stem, 0x14));
                    if (S32(0x424))
                        ORIG(void (*)(int32_t, int32_t *, void *, int32_t, int32_t, uint16_t, uint8_t *), RVA_UseStdWidth)(
                            stems[s][1], (int32_t *)(stem + 8), SP(0x418), S32(0x424), S32(0x2f0), S16(0), st);
                } else {
                    ORIG(void (*)(int32_t, int32_t *, uint8_t *), RVA_GetDTfmX)(stems[s][0], (int32_t *)(stem + 0x14), st);
                    ORIG(void (*)(int32_t, int32_t *, uint8_t *), RVA_GetDTfmX)(F32(stem, 0x10), (int32_t *)(stem + 0x18), st);
                    F32(stem, 0x2c) = F32(stem, 8) = sub32(F32(stem, 0x18), F32(stem, 0x14));
                    if (S32(0x420))
                        ORIG(void (*)(int32_t, int32_t *, void *, int32_t, int32_t, uint16_t, uint8_t *), RVA_UseStdWidth)(
                            stems[s][1], (int32_t *)(stem + 8), SP(0x410), S32(0x420), S32(0x2ec), S16(0), st);
                }
                {
                    uint32_t fl = FU32(stem, 0x48);
                    FP(stem, 0x40) = 0;
                    FP(stem, 0x38) = 0;
                    FU32(stem, 0x48) = (fl & 0xffff001d) | 0x10;
                }
                ORIG(void (*)(uint8_t *, uint8_t *), RVA_GCCalcLocs)(stem, st);
                gc_tail = stem;
            }
            /* bc3de */
            if (prev) {
                uint8_t *c = ORIG(uint8_t *(*)(uint8_t *, void *), RVA_NewGlbCounter)(st, hintmem);
                if (!c)
                    goto L_b9ef6;
                F32(c, 8) = sub32(F32(stem, 0x14), F32(prev, 0x18));
                FP(c, 0x10) = stem;
                FP(c, 0x18) = prev;
                FP(c, 0) = SP(0x4b0);
                SP(0x4b0) = c;
                FP(stem, 0x40) = c;
                FP(prev, 0x38) = c;
            }
            prev = stem;
        }
        /* bc451 */
        if ((i == hcount - 1 || i == total - 1) && prev) {
            ORIG(void (*)(void *, uint8_t *), RVA_GCFixLocs)(SP(0x4a8), st);
            for (uint8_t *q = (uint8_t *)SP(0x4a8); q; q = (uint8_t *)FP(q, 0))
                F32(q, 8) = sub32(F32(q, 0x28), F32(q, 0x24));
            if (SP(0x400) == 0) {
                void **buf = (void **)SP(0x3f8);
                if (FU32(buf, 8) < 0x1450) {
                    void *obj = FP(SP(0x450), 0x20);
                    if (buf[0])
                        (*(free_fn *)((uint8_t *)*(void **)obj + 0x10))(obj, buf[0]);
                    obj = FP(SP(0x450), 0x20);
                    buf = (void **)SP(0x3f8);
                    buf[0] = (*(alloc_fn *)*(void **)obj)(obj, 0x1450);
                    buf = (void **)SP(0x3f8);
                    if (!buf[0]) {
                        FU32(buf, 8) = 0;
                        S32(0x460) = 2;
                        FU32(io, 0x1c) |= 0x2000;
                        goto L_b9f1d;
                    }
                    FU32(buf, 8) = 0x1450;
                    buf = (void **)SP(0x3f8);
                }
                SP(0x400) = buf[0];
                S32(0x3c0) = 1;
                SP(0x458) = (uint8_t *)buf[0] + 0x4b0;
            }
            if (!ORIG(int32_t (*)(void *, void *, int32_t, uint16_t, int32_t), RVA_GCFixOnePath)(
                    prev, SP(0x458), S32(0x2b4), (FU32(prev, 0x48) & 1) ? S16(0x330) : S16(2), S32(0x3f0)))
                goto L_b9ef6;
            prev = 0;
        }
        mask <<= 1;
    }
    goto L_ba8a3;
}

    /* ---- hintmask ---- */
L_bb457:
    COV(115);
    hm_pending = 0;
    if (S32(0x3c0) != 0 && S32(0x460) == 0) {
        /* bc1d2 */
        COV(104);
        if (!ORIG(int32_t (*)(void *, void *, int32_t, int32_t, uint8_t *, void *), RVA_GlobalColoringGC)(
                SP(0x4a8), FP(io[0], 0x10), S32(0x3f0), S32(0x3ec), st, hintmem)) {
            S32(0x3e8) = 1;
            goto L_bcd16;
        }
        S32(0x460) = 3;
        SP(0x4b0) = 0;
        SP(0x400) = 0;
        if (S32(0x3e8) != 0)
            goto L_bcd16;
        if (procs[0] && !((proc_fn)procs[0])(st, client))
            return 0xfc141195;
    }
    /* bb468 */
    if (S32(0x3bc) != 0) {
        if (S32(0x460) == 0)
            ORIG(void (*)(uint8_t *), RVA_GCStartLock)(st);
        ORIG(void (*)(uint8_t *), RVA_ClearHintLists)(st);
        S32(0x440) = 0;
        if (S32(0x43c) != 0)
            goto L_b9f1d;
    }
    {
        uint8_t mask = 0;
        int64_t k = 0;
        int32_t i = 0, total;
        for (int64_t s = 0; s < hcount64; s++, i++) {
            if ((i & 7) == 0) {
                if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
                    return 0xfe1411a9;
                mask = *cs++;
            }
            if (!f58)
                continue;
            if ((int8_t)mask < 0) {
                if (sflag[s]) {
                    base[k] = stems[s][0];
                    base[k + 1] = stems[s][1];
                    k += 2;
                } else {
                    base[5] = stems[s][1];
                    base[4] = stems[s][0];
                    ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RBLock)(base + 4, sby, st, hintmem);
                }
            }
            if (k == 6) {
                ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RVLock)(base, sby, st, hintmem);
                k = 0;
            }
            if (i == hcount - 1 && k) {
                ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RBLock)(base, sby, st, hintmem);
                if (k == 4)
                    ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RBLock)(base + 2, sby, st, hintmem);
            }
            if (S32(0x3e8) && S32(0x460) != 2)
                goto L_b9f01;
            mask <<= 1;
        }
        /* bb5a4 */
        total = hcount + vcount;
        k = 0;
        {
            int64_t s = hcount64;
            for (int32_t j = hcount; j < total; j++, s++) {
                if ((j & 7) == 0) {
                    if (cs > cs_end || cs < (const uint8_t *)desc[9] || cs > (const uint8_t *)desc[10])
                        return 0xfe1411cc;
                    mask = *cs++;
                }
                if (!f27)
                    continue;
                if ((int8_t)mask < 0) {
                    if (sflag[s]) {
                        base[k] = stems[s][0];
                        base[k + 1] = stems[s][1];
                        k += 2;
                    } else {
                        base[5] = stems[s][1];
                        base[4] = stems[s][0];
                        ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RXLock)(base + 4, sbx, st, hintmem);
                    }
                }
                if (k == 6) {
                    ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RMLock)(base, sbx, st, hintmem);
                    k = 0;
                }
                if (j == total - 1 && k) {
                    ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RXLock)(base, sbx, st, hintmem);
                    if (k == 4)
                        ORIG(void (*)(int32_t *, int32_t, uint8_t *, void *), RVA_RXLock)(base + 4, sbx, st, hintmem);
                }
                if (S32(0x3e8) && S32(0x460) != 2)
                    goto L_b9f01;
                mask <<= 1;
            }
        }
        S32(0x3bc) = 1;
        goto L_ba8a3;
    }

    /* ---- endchar ---- */
L_bb00d:
    if (S32(0x384))
        goto L_bc82b_flag;
    if (p7[0] & 0x80)
        FU32(io, 0x1c) |= 0x200;
    if (n > 1) {
        /* bbfad: seac */
        int32_t a, b, c, d;
        p7[0] |= 0x80;
        if ((p7[0] & 0x18) || path_open == 0)
            return 0xfe1410f4;
        if (n == 5) {
            a = base[1];
            b = base[2];
            c = base[3];
            d = base[4];
        } else if (n == 4) {
            a = base[0];
            base[0] = -1;
            b = base[1];
            c = base[2];
            d = base[3];
        } else
            return 0xfe141101;
        base[7] = d;
        base[6] = c;
        base[5] = b;
        base[4] = a;
        sp = base + 8;
        if (S32(0x3b8) == 0)
            goto L_ba350;
        goto L_bdc56;
    }
    if (S32(0x3b8) != 0)
        goto L_bc82b_flag;
    if (sp == base)
        base[0] = -1;
    sp = base + 5;
    goto L_ba350;

L_bc82b_flag:
    /* bdb17: endchar with the path flag as it is */
    if (S32(0x460) != 0) {
        if (path_open == 0) {
            if (procs[4] && !((proc_fn)procs[4])(st, client))
                return 0xfc14112f;
            path_open = 1;
        }
        goto L_bdb58;
    }
    goto L_bccd8;
L_bc82b:
    /* edx = 1 (the path flag was just set) */
    if (S32(0x460) != 0)
        goto L_bdb58;
L_bccd8:
    COV(116);
    S32(0x460) = 3;
    ORIG(void (*)(uint8_t *), RVA_AdjustToStdWidths)(st);
    ORIG(void (*)(uint8_t *, void *, int32_t, void *), RVA_GCProcess)(st, FP(io[0], 0x10), S32(0x2b4), hintmem);
    if (S32(0x3e8) == 0) {
        SP(0x4b0) = 0;
        goto L_b9f1d;
    }
L_bcd16:
    S32(0x460) = 2;
    FU32(io, 0x1c) |= 0x2000;
    goto L_b9f1d;
L_bdb58:
    if (p7[0] & 0x20) {
        uint32_t r = ORIG(uint32_t (*)(uint8_t *, void *, void *, uint32_t, void *, void *, void *), RVA_CScanFill)(
            st, a5, io, accent, mem, *(void **)client, hintmem);
        if (r)
            return r;
        if (procs[0] && !((proc_fn)procs[0])(st, client))
            return 0xfc14113c;
    } else {
        if (procs[5] && !((proc_fn)procs[5])(st, client))
            return 0xfc141142;
    }
    /* bc840 */
    if (S32(0x460) == 0)
        goto L_bccd8;
    if (path_open == 0 && procs[4] && !((proc_fn)procs[4])(st, client))
        return 0xfc14115b;
    {
        uint32_t f = p7[0];
        if ((f & 0x30) != 0x30) {
            if ((f & 0x28) == 0x28 && S32(0x3c8) > S32(0x3c4))
                F32(io[1], 0x20) = S32(0x3cc);
            return 0;
        }
        p7[0] = (f & ~0x10u) | 8;
        if (S32(0x460) == 0)
            ORIG(void (*)(uint8_t *), RVA_GCStartLock)(st);
        ORIG(void (*)(uint8_t *), RVA_ClearHintLists)(st);
        S32(0x3c8) = add32(S32(0x3d0), S32(0x3c4));
        S32(0x440) = 1;
        S32(0x3cc) = add32(S32(0x3d4), F32(io[1], 0x20));
        S32(0x3d0) = 0;
        U32(0x3c4) = 0x80000000;
        goto L_b9f1d;
    }

    /* ---- seac: interpret the base glyph, then the accent ---- */
L_bdc56:
    COV(100);
    base[3] = 0;
    if ((base[6] & 0xff00ffff) || (base[7] & 0xff00ffff))
        return 0xfe141108;
    ORIG(void (*)(int32_t *, void *, int32_t, int32_t, uint8_t *), RVA_CCBuild)(base + 3, FP(p7, 8), sbx, sby, st);
    if (p7[0] & 0x200)
        return 1;
    p7[0] |= 0x10;
    if (S32(0x460) == 0)
        ORIG(void (*)(uint8_t *), RVA_GCStartLock)(st);
    ORIG(void (*)(uint8_t *), RVA_ClearHintLists)(st);
    S32(0x440) = 1;
    goto L_b9f1d;

    /* ---- rlineto ---- */
L_badb0:
    n = (int32_t)(sp - base);
    if (n < 2 || (n & 1) || S32(0x3b8) == 0)
        return 0xfe140e48;
    for (int64_t i = 0; i < n; i += 2) {
#ifdef CFF_CONTROL_INTERP
        cur.x = add32(cur.x, add32(base[i], 1));
#else
        cur.x = add32(cur.x, base[i]);
#endif
        cur.y = add32(cur.y, base[i + 1]);
        if (S32(0x460) == 0) {
            ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(lastpt, pk(cur), st);
            lastpt = pk(cur);
            dev = cur;
            continue;
        }
        tfm(&cur, &dev, st);
        if (procs[2] && !((pt_fn)procs[2])(&dev, st, client))
            return 0xfc140e54;
    }
    goto L_ba89f;

    /* ---- rrcurveto, rcurveline ---- */
L_bab13 : {
    int32_t cnt = (int32_t)(sp - base), lim;
    int32_t *p;
    int32_t i;
    int64_t k;
    if (cnt < 6 || S32(0x3b8) == 0)
        return 0xfe140e94;
    lim = cnt - 6;
    p = base + 1;
    i = 0;
    k = 0;
    while (i <= lim) {
        ipt end;
        if (cnt < 6)
            return 0xfe140e97;
        if (SP(0x428) == noTfm) {
            COV(106);
            ipt p1, p2, p3;
            int32_t v = add32(cur.x, p[-1]);
            int32_t (*map)(void *, int32_t) = ORIG(int32_t (*)(void *, int32_t), RVA_Map);
            p1.x = map(SP(0x468), v);
            v = add32(v, p[1]);
            p2.x = map(SP(0x468), v);
            v = add32(v, p[3]);
            p3.x = map(SP(0x468), v);
            cur.x = v;
            v = add32(cur.y, p[0]);
            p1.y = map(SP(0x470), v);
            v = add32(v, p[2]);
            p2.y = map(SP(0x470), v);
            v = add32(v, p[4]);
            p3.y = map(SP(0x470), v);
            cur.y = v;
            if (procs[3] && !((curve_fn)procs[3])(&dev, &p1, &p2, &p3, st, client))
                return 0xfc140ea8;
            end = p3;
        } else {
            ipt P1, P2, P3;
            int32_t c = add32(p[-1], cur.x);
            int32_t r8 = add32(p[1], c);
            int32_t d = add32(p[3], r8);
            p[-1] = c;
            p[0] = add32(p[0], cur.y);
            c = add32(p[2], p[0]);
            p[4] = add32(p[4], c);
            p[1] = r8;
            p[2] = c;
            p[3] = d;
            cur.x = d;
            cur.y = p[4];
            P1.x = p[-1];
            P1.y = p[0];
            P2.x = p[1];
            P2.y = p[2];
            P3.x = p[3];
            P3.y = p[4];
            if (S32(0x460) == 0) {
                ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(lastpt, pk(P1), st);
                ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(pk(P2), pk(P3), st);
                lastpt = pk(cur);
            } else {
                tfm(&P1, &P1, st);
                tfm(&P2, &P2, st);
                tfm(&P3, &P3, st);
                if (procs[3] && !((curve_fn)procs[3])(&dev, &P1, &P2, &P3, st, client))
                    return 0xfc140ec4;
            }
            end = P3;
        }
        dev = end;
        if ((int64_t)(sp - base) == (int64_t)(i + 8)) {
            base[0] = base[k + 6];
            base[1] = base[k + 7];
            sp = base + 2;
            goto L_badb0;
        }
        i += 6;
        cnt -= 6;
        k += 6;
        p += 6;
    }
    goto L_ba89f;
}

    /* ---- hhcurveto ---- */
L_bce38 : {
    int32_t cnt = n;
    int64_t k = 0, total = n;
    if (cnt == 0 || S32(0x3b8) == 0)
        return 0xfe14131e;
    while (k < total) {
        ipt P1, P2, P3;
        int32_t y, x;
        if (cnt & 1) {
            if (k != 0 || total < 5)
                return 0xfe141322;
            y = add32(cur.y, base[0]);
            k = 1;
            cnt--;
        } else {
            if (cnt < 4)
                return 0xfe141327;
            y = cur.y;
        }
        x = add32(base[k], cur.x);
        P1.x = x;
        P1.y = y;
        x = add32(base[k + 1], x);
        y = add32(y, base[k + 2]);
        P2.x = x;
        P2.y = y;
        P3.x = add32(x, base[k + 3]);
        P3.y = y;
        cur = P3;
        if (SP(0x428) == noTfm) {
            COV(106);
            int32_t (*map)(void *, int32_t) = ORIG(int32_t (*)(void *, int32_t), RVA_Map);
            P1.x = map(SP(0x468), P1.x);
            P1.y = map(SP(0x470), P1.y);
            P2.x = map(SP(0x468), P2.x);
            P2.y = map(SP(0x470), P2.y);
            P3.x = map(SP(0x468), P3.x);
            P3.y = map(SP(0x470), P3.y);
            if (procs[3] && !((curve_fn)procs[3])(&dev, &P1, &P2, &P3, st, client))
                return 0xfc14134a;
        } else if (S32(0x460) == 0) {
            ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(lastpt, pk(P1), st);
            ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(pk(P2), pk(P3), st);
            lastpt = pk(cur);
        } else {
            tfm(&P1, &P1, st);
            tfm(&P2, &P2, st);
            tfm(&P3, &P3, st);
            if (procs[3] && !((curve_fn)procs[3])(&dev, &P1, &P2, &P3, st, client))
                return 0xfc14133d;
        }
        dev = P3;
        cnt -= 4;
        k += 4;
    }
    goto L_ba89f;
}

    /* ---- vvcurveto ---- */
L_bd047 : {
    int32_t cnt = n;
    int64_t k = 0, total = n;
    if (cnt == 0 || S32(0x3b8) == 0)
        return 0xfe1412d7;
    while (k < total) {
        ipt P1, P2, P3;
        int odd = cnt & 1;
        if (odd) {
            if (cnt < 5)
                return 0xfe1412db;
            P1.y = add32(base[k + 1], cur.y);
            P1.x = add32(base[k], cur.x);
            P2.y = add32(base[k + 3], P1.y);
            P2.x = add32(base[k + 2], P1.x);
            P3.y = add32(P2.y, base[k + 4]);
        } else {
            if (cnt < 4)
                return 0xfe1412e3;
            P1.y = add32(base[k], cur.y);
            P2.y = add32(base[k + 2], P1.y);
            P1.x = cur.x;
            P2.x = add32(base[k + 1], cur.x);
            P3.y = add32(P2.y, base[k + 3]);
        }
        P3.x = P2.x;
        cur = P3;
        if (SP(0x428) == noTfm) {
            COV(106);
            int32_t (*map)(void *, int32_t) = ORIG(int32_t (*)(void *, int32_t), RVA_Map);
            P1.x = map(SP(0x468), P1.x);
            P1.y = map(SP(0x470), P1.y);
            P2.x = map(SP(0x468), P2.x);
            P2.y = map(SP(0x470), P2.y);
            P3.x = map(SP(0x468), P3.x);
            P3.y = map(SP(0x470), P3.y);
            if (procs[3] && !((curve_fn)procs[3])(&dev, &P1, &P2, &P3, st, client))
                return 0xfc141306;
        } else if (S32(0x460) == 0) {
            ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(lastpt, pk(P1), st);
            ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(pk(P2), pk(P3), st);
            lastpt = pk(cur);
        } else {
            tfm(&P1, &P1, st);
            tfm(&P2, &P2, st);
            tfm(&P3, &P3, st);
            if (procs[3] && !((curve_fn)procs[3])(&dev, &P1, &P2, &P3, st, client))
                return 0xfc1412f9;
        }
        dev = P3;
        if (odd) {
            cnt -= 5;
            k += 5;
        } else {
            cnt -= 4;
            k += 4;
        }
    }
    goto L_ba89f;
}

    /* ---- vhcurveto, hvcurveto ---- */
L_bb886 : {
    int horiz = op == 31;
    int32_t cnt = n, lim, r13 = 5;
    int32_t *b = base + 4;
    if (cnt < 4 || S32(0x3b8) == 0)
        return 0xfe141373;
    lim = cnt - 4;
    while (r13 - 5 <= lim) {
        ipt P1, P2, P3;
        if (cnt < 4)
            return 0xfe141376;
        if (horiz) {
            P1.x = add32(b[-4], cur.x);
            P1.y = cur.y;
            P2.x = add32(b[-3], P1.x);
            P2.y = add32(b[-2], cur.y);
            P3.x = P2.x;
            P3.y = add32(P2.y, b[-1]);
            if (r13 == n)
                P3.x = add32(b[0], P2.x);
            horiz = 0;
        } else {
            P1.x = cur.x;
            P1.y = add32(b[-4], cur.y);
            P2.x = add32(b[-3], cur.x);
            P2.y = add32(b[-2], P1.y);
            P3.x = add32(P2.x, b[-1]);
            P3.y = P2.y;
            if (r13 == n)
                P3.y = add32(b[0], P2.y);
            horiz = 1;
        }
        cur = P3;
        if (SP(0x428) == noTfm) {
            COV(106);
            int32_t (*map)(void *, int32_t) = ORIG(int32_t (*)(void *, int32_t), RVA_Map);
            P1.x = map(SP(0x468), P1.x);
            P1.y = map(SP(0x470), P1.y);
            P2.x = map(SP(0x468), P2.x);
            P2.y = map(SP(0x470), P2.y);
            P3.x = map(SP(0x468), P3.x);
            P3.y = map(SP(0x470), P3.y);
            if (procs[3] && !((curve_fn)procs[3])(&dev, &P1, &P2, &P3, st, client))
                return 0xfc1413a8;
        } else if (S32(0x460) == 0) {
            ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(lastpt, pk(P1), st);
            ORIG(void (*)(uint64_t, uint64_t, uint8_t *), RVA_GCLine)(pk(P2), pk(P3), st);
            lastpt = pk(cur);
        } else {
            tfm(&P1, &P1, st);
            tfm(&P2, &P2, st);
            tfm(&P3, &P3, st);
            if (procs[3] && !((curve_fn)procs[3])(&dev, &P1, &P2, &P3, st, client))
                return 0xfc14139a;
        }
        cnt -= 4;
        r13 += 4;
        b += 4;
        dev = P3;
    }
    goto L_ba89f;
}

    /* ---- reserved operators: ignored in CFF2 ---- */
L_bd979:
    if (S32(0x384) == 0)
        return 0xfe1413e3;
    goto L_ba8a3;

    /* ---- after a path operator: clear the stack ---- */
L_ba89f:
    path_open = 0;
L_ba8a3:
    sp = base;
    if (SP(0x3b0))
        memset(SP(0x3b0), 0, (uint64_t)U32(0x3a0) << 2);
    goto L_ba043;

    /* ---- hint failure: start again without hints ---- */
L_b9ef6:
    S32(0x3e8) = 1;
L_b9f01:
    COV(102);
    S32(0x460) = 2;
    FU32(io, 0x1c) |= 0x2000;
    goto L_b9f1d;

    /* ---- the hint pass ---- */
L_bd387:
    COV(101);
    S32(0x460) = 0;
    ORIG(void (*)(void *, void *, void *, uint8_t *), RVA_GCInitBuffs)(((void **)io[0])[0], ((void **)io[0])[1],
                                                                       ((void **)io[0])[2], st);
    goto L_b9e4b;
}
CFF_EXPORT_N(cff_DoType1InterpretCharString, cff_DoType1InterpretCharString_impl, 3);

/* ------------------------------------------------------------------------------------------
 * Type1InterpretCharString */

CFF_API uint32_t cff_Type1InterpretCharString_impl(uint8_t *st, void *mem, void **desc, uint8_t *io, void *a5,
                                                   void **procs, uint32_t *p7) {
    uint32_t local[8]; /* the caller's record, copied */
    void *erect[4];    /* client data, state, erect proc table, caller's proc table */
    void *table[6];    /* NewChar, MoveTo, LineTo, CurveTo, ClosePath, EndChar */
    void **pr = procs;
    uint32_t *flags = (uint32_t *)(st + 0x2bc);
    uint32_t r, saved = 0;
    int retried = 0;
    typedef uint32_t (*do_fn)(uint8_t *, void *, void **, void *, void *, void **, uint32_t *);
    do_fn interp = ORIG(do_fn, RVA_DoType1InterpretCharString);
    void *csnewline = ORIG(void *, RVA_CSNewLine);

    memset(table, 0, sizeof table);
    memset(erect, 0, sizeof erect);
    memcpy(local, p7, sizeof local);
    *(uint32_t *)(io + 0x1c) = 0;
    *(void **)(st + 0x358) = io;
    *(uint32_t *)(st + 0x3c4) = 0x80000000;
    if (procs[2] != csnewline || (*flags & 0x490000) != 0x400000) {
        uint32_t f = *flags;
        uint32_t i = (f >> 16 & 1) + (f >> 22 & 1) * 2 + (f >> 19 & 1) * 4 + (procs[2] == csnewline ? 8 : 0);
#ifdef CFF_CONTROL_TYPE1INTERP
        i &= 7;
#endif
        erect[3] = procs;
        erect[1] = st;
        table[1] = ORIG(void *, erect_moveto[i]);
        table[2] = ORIG(void *, erect_lineto[i]);
        table[0] = ORIG(void *, RVA_erectToDev_NewChar);
        table[3] = procs[3] == ORIG(void *, RVA_FixedFltn) ? ORIG(void *, RVA_erectToDev_CurveTo_FixedFltn)
                                                          : ORIG(void *, RVA_erectToDev_CurveTo_client);
        local[0] |= 0x40;
        table[4] = ORIG(void *, RVA_erectToDev_ClosePath);
        table[5] = ORIG(void *, RVA_erectToDev_EndChar);
        erect[0] = *(void **)(p7 + 6);
        erect[2] = table;
        *(void **)(local + 6) = erect;
        pr = table;
        COV(108);
    }
    *(uint16_t *)(st + 0x336) = *(uint16_t *)(io + 0x28);
    *(uint16_t *)(st + 0x338) = *(uint16_t *)(io + 0x2a);
    r = interp(st, mem, desc, io, a5, pr, local);
    *p7 |= local[0] & 0x180;
    while (r != 0) {
        uint32_t f;
        if ((((r & 0xff000000) + 0x5000000) & 0xfeffffff) == 0 || (*(uint32_t *)(st + 0x2c0) & 0x3000))
            break;
        f = *flags;
        if (!(f & 0xc0000000))
            break;
        saved = f;
        *flags = f & 0x3fffffff;
        *(uint32_t *)(st + 0x2c0) |= 0x2000;
        *(uint32_t *)(io + 0x1c) |= 0x2000;
        *(uint16_t *)(st + 0x336) = *(uint16_t *)(io + 0x28);
        *(uint16_t *)(st + 0x338) = *(uint16_t *)(io + 0x2a);
        retried = 1;
        COV(107);
        r = interp(st, mem, desc, io, a5, pr, local);
        *p7 |= local[0] & 0x180;
    }
    if (retried)
        *flags = saved;
    return r;
}
CFF_EXPORT_N(cff_Type1InterpretCharString, cff_Type1InterpretCharString_impl, 3);

/* ------------------------------------------------------------------------------------------
 * -DCFF_CONTROL_SYNTH (reimpl/cff/interp_synth_check.py, oracle mode only): a hook for
 * DoType1InterpretCharString that interprets a charstring of the test's choosing instead of the
 * glyph's, with the original (mode 0) or the port (mode 1), so the operators no corpus font
 * uses are compared too. A diagnostic build, not a negative control. */
#if defined(CFF_CONTROL_SYNTH) && !defined(IDWR_STANDALONE)
int oracle_hook_suspend(uint32_t rva, int suspend); /* reimpl/oracle/fs_oracle.c */
static uint8_t g_synth[8192];
static uint32_t g_synth_len, g_synth_mode;
static uint32_t g_synth_stats[3]; /* calls, calls that returned an error, the last error */

__declspec(dllexport) uint32_t *cff_synth_stats(void) { return g_synth_stats; }

__declspec(dllexport) int cff_synth_set(const uint8_t *cs, uint32_t len, uint32_t mode) {
    if (len > sizeof g_synth)
        return 0;
    memcpy(g_synth, cs, len);
    g_synth_len = len;
    g_synth_mode = mode;
    return 1;
}

CFF_API uint32_t cff_synth_DoType1_impl(uint8_t *st, void *mem, void **desc, void **io, void *a5, void **procs,
                                        uint32_t *p7) {
    void *d[11];
    uint32_t r;
    memcpy(d, desc, sizeof d);
    if (g_synth_len) {
        d[0] = g_synth;
        d[1] = g_synth + g_synth_len - 1;
        if ((uint8_t *)d[9] > g_synth)
            d[9] = g_synth;
        if ((uint8_t *)d[10] < g_synth + g_synth_len)
            d[10] = g_synth + g_synth_len;
    }
    if (g_synth_mode)
        r = cff_DoType1InterpretCharString_impl(st, mem, d, io, a5, procs, p7);
    else {
        typedef uint32_t (*do_fn)(uint8_t *, void *, void **, void *, void *, void **, uint32_t *);
        oracle_hook_suspend(RVA_DoType1InterpretCharString, 1);
        r = ORIG(do_fn, RVA_DoType1InterpretCharString)(st, mem, d, io, a5, procs, p7);
        oracle_hook_suspend(RVA_DoType1InterpretCharString, 0);
    }
    memcpy((uint8_t *)desc + 0x40, (uint8_t *)d + 0x40, 8); /* the seac callback's field */
    g_synth_stats[0]++;
    if (r) {
        g_synth_stats[1]++;
        g_synth_stats[2] = r;
    }
    return r;
}
CFF_EXPORT_N(cff_synth_DoType1, cff_synth_DoType1_impl, 3);
#endif
