/*
 * fs_* scaler API per glyph in DWrite.dll 10.0.26100.9711's TrueType scaler, and the device
 * metrics it computes, ported to C. Ported from REA assembly (authoritative) and the Ghidra
 * decompilation. All cross-function calls go through ORIG(rva): the oracle's function swap
 * overwrites each original's entry with a jump to the port, so ORIG(rva) runs the port when
 * that function is also swapped in and the original otherwise. This keeps single-function and
 * all-together swaps both correct.
 *
 * Group: fs__Contour, fs_NewGlyph, fs_SetUpKey, fs_FindBlocForPpem,
 * fsg_UpdatePrivateSpaceAddresses, fsg_UpdateWorkSpaceElement, sbit_CalcDevHorMetrics,
 * sbit_GetDevAdvanceWidth, sbit_GetDevAdvanceHeight, SubstituteHorMetrics,
 * SubstituteVertMetrics, sfac_GetSbitMetrics, UScaleX, SScaleX, UScaleY, UEmScaleX,
 * SEmScaleY, UEmScaleY.
 *
 * Behaviour worth knowing:
 * - Stack arguments of ORIG calls must have the original's width: itrp_Execute's 6th argument
 *   (trace function) is a full qword 0; passing a 4-byte int leaves stale upper bits that the
 *   interpreter calls as a trace function.
 * - fs__Contour's work-space setup (fsg_UpdatePrivateSpaceAddresses, then a 72-byte copy of
 *   the 9 memory bases to client+0x128) and fsg_UpdateWorkSpaceElement (fs_FindBitMapSize,
 *   fs_ContourScan) only run when memory base 3/4 moved since client+0x140/0x148 was set.
 * - fs_FindBitMapSize recomputes the device advances fs__Contour writes to info+0x48/0x4c/
 *   0xcc/0xd0 for outline glyphs.
 * - sbit_CalcDevHorMetrics (only caller fs__Contour) needs fs_GlyphInputType+0x88, which
 *   DWrite's rasterizer clears and never sets, and GDI-compatible overscale with strikes on,
 *   which NewTransform never builds. SubstituteHorMetrics needs a strike without horizontal
 *   metrics (none among the system fonts).
 * - UEmScaleX/UEmScaleY take the scale from +0x1e/+0x20 under a transform (+0x1a/+0x1c
 *   otherwise), unlike UScaleX/UScaleY; SubstituteHorMetrics scales the horizontal side bearing
 *   with SEmScaleY, as the original does.
 * - SafeInt checks call the original SafeIntOnOverflow / SafeIntOnDivZero (which throw); no
 *   valid font reaches them.
 */
#include <string.h>
#include "tt_state.h"
#include "orig.h"

/* SafeInt traps: the original throws (noreturn). Reachable only on arithmetic overflow that
 * valid fonts never produce. We call the original so behaviour matches if ever hit. */
#define RVA_SafeIntOverflow 0x1090b0
#define RVA_SafeIntDivZero  0x14cd24
static void of(void) { ORIG(void (*)(void), RVA_SafeIntOverflow)(); }
static void dz(void) { ORIG(void (*)(void), RVA_SafeIntDivZero)(); }

/* ---- out-of-group callees (RVA), all reached through ORIG so hooks apply ---- */
#define RVA_UScaleX            0x24f8
#define RVA_SScaleX            0x25b0
#define RVA_UScaleY            0x23a4
#define RVA_UEmScaleX          0x3ed8
#define RVA_SEmScaleY          0x4570
#define RVA_UEmScaleY          0x4d68
#define RVA_sfac_GetSbitMetrics 0x2190
#define RVA_SubstituteHorMetrics 0x10b6cc
#define RVA_SubstituteVertMetrics 0x4ccc
#define RVA_sbit_CalcDevHorMetrics 0x1404
#define RVA_sbit_GetDevAdvanceWidth 0x1d28
#define RVA_sbit_GetDevAdvanceHeight 0x1e5c
#define RVA_fsg_UpdatePrivateSpaceAddresses 0xca2b4
#define RVA_sfac_ReadGlyphHorMetrics 0xf5b0
#define RVA_sfac_ReadGlyphVertMetrics 0xf840
#define RVA_sfac_SearchForStrike 0x7dc4
#define RVA_sfac_GetGlyphIndex 0x10930c
#define RVA_sbit_SearchForBitmap 0x6620
#define RVA_SmartPtrDtor       0xe5100
/* fs__Contour callees */
#define RVA_sfac_CopyCVT       0xc0f0
#define RVA_scl_Scale          0xc270
#define RVA_itrp_Execute       0xb7dc
#define RVA_fsg_RunPreProgram  0xbe70
#define RVA_fsg_ExecuteGlyph   0xdbdc
#define RVA_fsg_Embold         0x64acc
#define RVA_scl_PostTransformGlyph 0xc37c
#define RVA_fsg_GridFit        0xc3e4
#define RVA_DWRITE_FixDiv      0x69800
#define RVA_scl_ScaleDownFromSubPixelOverscale 0xccac8
#define RVA_scl_AdjustCompatibleMetrics 0xd7f5c

typedef int (*fn2_t)(void *, void *);

/* ============================ scaling (16.16 fixed, SafeInt arithmetic) ============================ */

/* UScaleX 0x24f8: scale an unsigned value in x, with optional +1 rounding. state fields:
 * +0x18 transform flag (==3), +0x1a numX, +0x1e denX, +0x50 round flag. */
TT_EXPORT uint16_t *tt_UScaleX(uint16_t *out, uint8_t *st, uint32_t val) {
    uint32_t p3 = val & 0xffff;
    if (AT(st, 0x18, int16_t) == 3) {
        uint32_t num = (uint32_t)AT(st, 0x1a, uint16_t) * 2;
        if ((uint64_t)p3 * num >= 0x100000000ULL) of();
        p3 = p3 * num;
        uint32_t add = AT(st, 0x1e, uint16_t);
        uint32_t r = add + p3;
        if (!(p3 <= r)) of();
        p3 = r;
        uint32_t den = (uint32_t)AT(st, 0x1e, uint16_t) * 2;
        if (p3 != 0) { if (den == 0) dz(); p3 = p3 / den; } else p3 = 0;
        if (p3 >= 0x10000) of();
        p3 = (uint16_t)p3;
    }
    if (AT(st, 0x50, int16_t) != 0 && (int16_t)p3 != 0) {
        p3 = (p3 & 0xffff) + 1;
        if (p3 > 0xffff) of();
    }
    *out = (uint16_t)p3;
    return out;
}

/* UScaleY 0x23a4: y version, numY +0x1c, denY +0x20, no rounding. */
TT_EXPORT uint16_t *tt_UScaleY(uint16_t *out, uint8_t *st, uint32_t val) {
    uint32_t p3 = val & 0xffff;
    if (AT(st, 0x18, int16_t) == 3) {
        uint32_t num = (uint32_t)AT(st, 0x1c, uint16_t) * 2;
        if ((uint64_t)p3 * num >= 0x100000000ULL) of();
        p3 = p3 * num;
        uint32_t add = AT(st, 0x20, uint16_t);
        uint32_t r = add + p3;
        if (!(p3 <= r)) of();
        p3 = r;
        uint32_t den = (uint32_t)AT(st, 0x20, uint16_t) * 2;
        if (p3 != 0) { if (den == 0) dz(); p3 = p3 / den; } else p3 = 0;
        if (p3 >= 0x10000) of();
        p3 = (uint16_t)p3;
    }
    *out = (uint16_t)p3;
    return out;
}

/* SScaleX 0x25b0: signed x scale. Non-transform stores the value as-is. */
TT_EXPORT uint16_t *tt_SScaleX(uint16_t *out, uint8_t *st, int16_t val) {
    if (AT(st, 0x18, int16_t) == 3) {
        if (val < 0) {
            if (val == (int16_t)0x8000) of();            /* SafeInt<short> negate */
            uint16_t pos = (uint16_t)(-(int)val);
            uint16_t sc;
            ORIG(uint16_t *(*)(uint16_t *, uint8_t *, uint32_t), RVA_UScaleX)(&sc, st, pos);
            int neg = -(int)(uint32_t)sc;                /* SafeInt<int> negate (sc != INT_MIN) */
            if ((uint32_t)(neg + 0x8000) >= 0x10000) of();
            *out = (uint16_t)(int16_t)neg;
        } else {
            uint16_t sc;
            ORIG(uint16_t *(*)(uint16_t *, uint8_t *, uint32_t), RVA_UScaleX)(&sc, st, (uint16_t)val);
            if (sc >= 0x8000) of();
            *out = sc;
        }
    } else {
        *out = (uint16_t)val;
    }
    return out;
}

/* UEmScaleX 0x3ed8: em-scale in x. transform picks denX(+0x1e) else numX(+0x1a); divisor
 * +0x4e doubled; optional +1 rounding via +0x50. */
TT_EXPORT uint16_t *tt_UEmScaleX(uint16_t *out, uint8_t *st, uint32_t val) {
    uint16_t f = (AT(st, 0x18, int16_t) == 3) ? AT(st, 0x1e, uint16_t) : AT(st, 0x1a, uint16_t);
    uint64_t u = (uint64_t)f * 2 * (val & 0xffff);
    if (u >= 0x100000000ULL) of();
    uint16_t em = AT(st, 0x4e, uint16_t);
    uint32_t u1 = (uint32_t)u;
    uint32_t u3 = em + u1;
    if (!(u1 <= u3)) of();
    uint64_t res = 0;
    if (u3 != 0) {
        uint32_t den = (uint32_t)em + em;
        if (den == 0) dz();
        res = (uint64_t)u3 / den;
        if (res > 0xffff) of();
    }
    if (AT(st, 0x50, int16_t) == 0 || (int16_t)res == 0) { *out = (uint16_t)res; return out; }
    res = (res & 0xffff) + 1;
    if (res >= 0x10000) of();
    *out = (uint16_t)res;
    return out;
}

/* UEmScaleY 0x4d68: em-scale in y. transform picks denY(+0x20) else numY(+0x1c); no rounding. */
TT_EXPORT uint16_t *tt_UEmScaleY(uint16_t *out, uint8_t *st, uint16_t val) {
    uint16_t f = (AT(st, 0x18, int16_t) == 3) ? AT(st, 0x20, uint16_t) : AT(st, 0x1c, uint16_t);
    uint64_t u = (uint64_t)f * 2 * (uint16_t)val;
    if (u >= 0x100000000ULL) of();
    uint16_t em = AT(st, 0x4e, uint16_t);
    uint32_t u1 = (uint32_t)u;
    uint32_t u5 = em + u1;
    if (!(u1 <= u5)) of();
    uint16_t res = 0;
    if (u5 != 0) {
        uint32_t den = (uint32_t)em + em;
        if (den == 0) dz();
        uint64_t q = (uint64_t)u5 / den;
        res = (uint16_t)q;
        if (q > 0xffff) of();
    }
    *out = res;
    return out;
}

/* SEmScaleY 0x4570: signed em-scale in y (uses UEmScaleY internally, which does the transform
 * check). */
TT_EXPORT uint16_t *tt_SEmScaleY(uint16_t *out, uint8_t *st, int16_t val) {
    if (val < 0) {
        if (val == (int16_t)0x8000) of();
        uint16_t nv = (uint16_t)(-(int)val);
        uint16_t sc;
        ORIG(uint16_t *(*)(uint16_t *, uint8_t *, uint16_t), RVA_UEmScaleY)(&sc, st, nv);
        int neg = -(int)(uint32_t)sc;                    /* CastThrow<short,int> */
        if ((uint32_t)(neg + 0x8000) >= 0x10000) of();
        *out = (uint16_t)(int16_t)neg;
    } else {
        uint16_t sc;
        ORIG(uint16_t *(*)(uint16_t *, uint8_t *, uint16_t), RVA_UEmScaleY)(&sc, st, (uint16_t)val);
        if (sc >= 0x8000) of();
        *out = sc;
    }
    return out;
}

/* ============================ sbit metrics ============================ */

/* sfac_GetSbitMetrics 0x2190: read embedded-bitmap metrics for the current glyph through the
 * client GetSfntFragment callback (client+8), release via client+0x10. param_2 selects the
 * metric layout (1 hor, 3 both, else vert); param_3==2 picks the horizontal EBLC table. */
TT_EXPORT int tt_sfac_GetSbitMetrics(uint8_t *cl, uint16_t p2, uint16_t p3, uint32_t p4,
                                     uint16_t *p5, uint16_t *p6, int16_t *p7, int16_t *p8,
                                     int16_t *p9, int16_t *p10, uint16_t *p11, uint16_t *p12,
                                     uint8_t *p13, uint8_t *p14) {
    typedef uint8_t *(*getfrag_t)(void *, int, int, void **);
    typedef void (*relfrag_t)(void *);
    *p13 = 0;
    *p14 = 0;
    uint32_t len = (p2 == 3) ? 8 : 5;
    long present_off = (p3 != 2) ? 0xac : 0xa4;
    if (AT(cl, present_off, int32_t) == 0) return 0x1409;
    long base_off = (p3 != 2) ? 0xa8 : 0xa0;
    uint32_t off = AT(cl, base_off, uint32_t) + p4;
    if (off < p4 || off > 0x7fffffff) of();
    void *ctx = NULL;
    getfrag_t get = (getfrag_t)AT(cl, 8, void *);
    uint8_t *ptr = get(AT(cl, 0, void *), (int)off, (int)len, &ctx);
    int err = (ptr == NULL) ? 0x1408 : 0;
    if (err != 0) {
        if (ctx) ((relfrag_t)AT(cl, 0x10, void *))(ctx);
        return err;
    }
    *p5 = (uint16_t)ptr[0];
    *p6 = (uint16_t)ptr[1];
    uint8_t b = ptr[2];
    if (p2 == 3) {
        *p7 = (int16_t)(int8_t)b;
        *p8 = (int16_t)(int8_t)ptr[3];
        *p11 = (uint16_t)ptr[4];
        *p9 = (int16_t)(int8_t)ptr[5];
        *p10 = (int16_t)(int8_t)ptr[6];
        b = ptr[7];
        *p13 = 1;
        *p14 = 1;
        *p12 = (uint16_t)b;
    } else if (p2 == 1) {
        *p7 = (int16_t)(int8_t)b;
        *p8 = (int16_t)(int8_t)ptr[3];
        b = ptr[4];
        *p13 = 1;
        *p11 = (uint16_t)b;     /* param_12 = param_11 in the original */
    } else {
        *p9 = (int16_t)(int8_t)b;
        *p10 = (int16_t)(int8_t)ptr[3];
        b = ptr[4];
        *p14 = 1;
        *p12 = (uint16_t)b;
    }
    if (ctx) ((relfrag_t)AT(cl, 0x10, void *))(ctx);
    return 0;
}

#ifdef _WIN32
__declspec(dllimport) void __stdcall OutputDebugStringA(const char *);
#endif

/* SubstituteHorMetrics 0x10b6cc: horizontal advance / left side bearing from hmtx when the
 * strike lacked big metrics. */
TT_EXPORT int tt_SubstituteHorMetrics(uint8_t *st, uint8_t *cl) {
    uint16_t aw = 0;
    int16_t lsb = 0;
    int err = ORIG(int (*)(uint8_t *, uint16_t, uint16_t *, int16_t *), RVA_sfac_ReadGlyphHorMetrics)(
        cl, AT(cl, 0xe8, uint16_t), &aw, &lsb);
    if (err == 0) {
        uint16_t t;
        ORIG(uint16_t *(*)(uint16_t *, uint8_t *, uint32_t), RVA_UEmScaleX)(&t, st, aw);
        AT(st, 0x2e, uint16_t) = t;
        AT(st, 0x44, uint16_t) = AT(st, 0x48, uint16_t);
        ORIG(uint16_t *(*)(uint16_t *, uint8_t *, int16_t), RVA_SEmScaleY)(&t, st, lsb);
        AT(st, 0x46, uint16_t) = t;
    }
    return err;
}

/* SubstituteVertMetrics 0x4ccc: vertical advance / top side bearing from vmtx. */
TT_EXPORT int tt_SubstituteVertMetrics(uint8_t *st, uint8_t *cl) {
    uint16_t ah = 0;
    int16_t tsb = 0;
    int err = ORIG(int (*)(uint8_t *, uint16_t, uint16_t *, int16_t *), RVA_sfac_ReadGlyphVertMetrics)(
        cl, AT(cl, 0xe8, uint16_t), &ah, &tsb);
    if (err == 0) {
        uint16_t t;
        ORIG(uint16_t *(*)(uint16_t *, uint8_t *, uint16_t), RVA_UEmScaleY)(&t, st, ah);
        AT(st, 0x30, uint16_t) = t;
        AT(st, 0x48, uint16_t) = AT(st, 0x44, uint16_t);
        uint16_t sv;
        ORIG(uint16_t *(*)(uint16_t *, uint8_t *, int16_t), RVA_SEmScaleY)(&sv, st, tsb);
        if ((int16_t)sv == (int16_t)0x8000) of();
        AT(st, 0x4a, int16_t) = (int16_t)(-(int)(int16_t)sv);
        err = 0;
    }
    return err;
}

/* sbit_CalcDevHorMetrics 0x1404: device advance, left and right side bearing from the strike
 * metrics, into *p3, *p4, *p5 in 26.6 (<<6); negated for orientation 2. */
TT_EXPORT int tt_sbit_CalcDevHorMetrics(uint8_t *st, uint8_t *cl, int32_t *p3, int32_t *p4, int32_t *p5) {
    typedef uint16_t *(*usx_t)(uint16_t *, uint8_t *, uint32_t);
    typedef uint16_t *(*ssx_t)(uint16_t *, uint8_t *, int16_t);
    uint8_t hor = 0, vert = 0;
    if ((AT(st, 0x22, uint16_t) & 0xfffd) != 0)
        OutputDebugStringA("sbit_CalcDevHorMetrics called under rotation\n");
    int err = ORIG(int (*)(uint8_t *, uint16_t, uint16_t, uint32_t, uint16_t *, uint16_t *,
                           int16_t *, int16_t *, int16_t *, int16_t *, uint16_t *, uint16_t *,
                           uint8_t *, uint8_t *), RVA_sfac_GetSbitMetrics)(
        cl, AT(st, 0x24, uint16_t), AT(st, 0x26, uint16_t), AT(st, 4, uint32_t),
        (uint16_t *)(st + 0x2a), (uint16_t *)(st + 0x2c), (int16_t *)(st + 0x44),
        (int16_t *)(st + 0x46), (int16_t *)(st + 0x48), (int16_t *)(st + 0x4a),
        (uint16_t *)(st + 0x2e), (uint16_t *)(st + 0x30), &hor, &vert);
    if (err != 0) return err;
    if (hor == 0) {
        err = ORIG(int (*)(uint8_t *, uint8_t *), RVA_SubstituteHorMetrics)(st, cl);
        if (err != 0) return err;
    }
    uint16_t t;
    if (AT(st, 0x22, int16_t) == 0) {
        ORIG(usx_t, RVA_UScaleX)(&t, st, AT(st, 0x2e, uint16_t)); uint16_t a = t;
        int16_t b = AT(st, 0x44, int16_t);
        *p3 = (uint32_t)a << 6;
        ORIG(ssx_t, RVA_SScaleX)(&t, st, b); int16_t sb = (int16_t)t;
        uint16_t c = AT(st, 0x2c, uint16_t);
        *p4 = (int)sb << 6;
        ORIG(usx_t, RVA_UScaleX)(&t, st, c); uint16_t cc = t;
        *p5 = (*p3 + (uint32_t)cc * (uint32_t)-0x40) - *p4;
    } else if (AT(st, 0x22, int16_t) == 2) {
        ORIG(usx_t, RVA_UScaleX)(&t, st, AT(st, 0x2e, uint16_t)); uint16_t a = t;
        int16_t b = AT(st, 0x44, int16_t);
        *p3 = (uint32_t)a * (uint32_t)-0x40;
        ORIG(ssx_t, RVA_SScaleX)(&t, st, b); int16_t sb = (int16_t)t;
        uint16_t c = AT(st, 0x2c, uint16_t);
        *p4 = (int)sb * -0x40;
        ORIG(usx_t, RVA_UScaleX)(&t, st, c); uint16_t cc = t;
        *p5 = ((uint32_t)cc * 0x40 - *p4) + *p3;
    } else {
        return 0x1802;
    }
    return 0;
}

/* sbit_GetDevAdvanceWidth 0x1d28: device advance width as (x,y) in 26.6. */
TT_EXPORT int tt_sbit_GetDevAdvanceWidth(uint8_t *st, uint8_t *cl, int32_t *pt) {
    uint8_t f1 = 0, f2 = 0;
    int err = ORIG(int (*)(uint8_t *, uint16_t, uint16_t, uint32_t, uint16_t *, uint16_t *,
                           int16_t *, int16_t *, int16_t *, int16_t *, uint16_t *, uint16_t *,
                           uint8_t *, uint8_t *), RVA_sfac_GetSbitMetrics)(
        cl, AT(st, 0x24, uint16_t), AT(st, 0x26, uint16_t), AT(st, 4, uint32_t),
        (uint16_t *)(st + 0x2a), (uint16_t *)(st + 0x2c), (int16_t *)(st + 0x44),
        (int16_t *)(st + 0x46), (int16_t *)(st + 0x48), (int16_t *)(st + 0x4a),
        (uint16_t *)(st + 0x2e), (uint16_t *)(st + 0x30), &f1, &f2);
    if (err != 0) return err;
    if (f1 == 0) {
        err = ORIG(int (*)(uint8_t *, uint8_t *), RVA_SubstituteHorMetrics)(st, cl);
        if (err != 0) return err;
    }
    uint16_t t;
    ORIG(uint16_t *(*)(uint16_t *, uint8_t *, uint32_t), RVA_UScaleX)(&t, st, AT(st, 0x2e, uint16_t));
    int16_t s = AT(st, 0x22, int16_t);
    uint32_t v = t;
    int32_t x = 0, y = (int32_t)(v * 0x40);
    if (s == 0) { x = (int32_t)(v * 0x40); y = 0; }
    else if (s != 1) {
        if (s == 2) { y = 0; x = (int32_t)(v * (uint32_t)-0x40); }
        else { if (s != 3) return 0x1802; y = (int32_t)(v * (uint32_t)-0x40); }
    }
    pt[0] = x;
    pt[1] = y;
    return 0;
}

/* sbit_GetDevAdvanceHeight 0x1e5c: device advance height as (x,y) in 26.6. */
TT_EXPORT int tt_sbit_GetDevAdvanceHeight(uint8_t *st, uint8_t *cl, int32_t *pt) {
    uint8_t f1 = 0, f2 = 0;
    int err = ORIG(int (*)(uint8_t *, uint16_t, uint16_t, uint32_t, uint16_t *, uint16_t *,
                           int16_t *, int16_t *, int16_t *, int16_t *, uint16_t *, uint16_t *,
                           uint8_t *, uint8_t *), RVA_sfac_GetSbitMetrics)(
        cl, AT(st, 0x24, uint16_t), AT(st, 0x26, uint16_t), AT(st, 4, uint32_t),
        (uint16_t *)(st + 0x2a), (uint16_t *)(st + 0x2c), (int16_t *)(st + 0x44),
        (int16_t *)(st + 0x46), (int16_t *)(st + 0x48), (int16_t *)(st + 0x4a),
        (uint16_t *)(st + 0x2e), (uint16_t *)(st + 0x30), &f2, &f1);
    if (err != 0) return err;
    if (f1 == 0) {
        err = ORIG(int (*)(uint8_t *, uint8_t *), RVA_SubstituteVertMetrics)(st, cl);
        if (err != 0) return err;
    }
    uint16_t t;
    ORIG(uint16_t *(*)(uint16_t *, uint8_t *, uint32_t), RVA_UScaleY)(&t, st, AT(st, 0x30, uint16_t));
    int16_t s = AT(st, 0x22, int16_t);
    uint32_t v = t;
    int32_t x = (int32_t)(v * 0x40), y = (int32_t)(v * 0x40);
    if (s != 0) {
        if (s == 1) { y = 0; x = (int32_t)(v * (uint32_t)-0x40); goto store; }
        if (s != 2) { if (s != 3) return 0x1802; y = 0; goto store; }
        y = (int32_t)(v * (uint32_t)-0x40);
    }
    x = 0;
store:
    pt[0] = x;
    pt[1] = y;
    return 0;
}

/* ============================ fs_* key / glyph setup ============================ */

/* fs_SetUpKey 0x8034: bind the client record to the input/info and its callbacks, returning
 * the client on success (errout=0) or NULL with the error code. flags is the required bit
 * mask over client+0x1d0. */
TT_EXPORT uint8_t *tt_fs_SetUpKey(uint8_t *input, void *info, uint32_t flags, int32_t *errout) {
    uint8_t *cl = AT(input, 8, uint8_t *);
    if (cl == NULL) { *errout = 0x1001; return NULL; }
    AT(cl, 0x118, void *) = input + 8;
    AT(cl, 0x120, void *) = info;
    if (info == NULL) { *errout = 0x1003; return NULL; }
    void *p = AT(input, 0x50, void *);
    AT(cl, 8, void *) = p;
    if (p == NULL) { *errout = 0x1008; return NULL; }
    p = AT(input, 0x58, void *);
    AT(cl, 0x10, void *) = p;
    if (p == NULL) AT(cl, 0x10, void *) = (uint8_t *)idwr_orig(RVA_SmartPtrDtor);
    p = AT(input, 0x60, void *);
    AT(cl, 0xf8, void *) = p;
    if (p == NULL) { *errout = 0x100d; return NULL; }
    p = AT(input, 0x68, void *);
    AT(cl, 0x100, void *) = p;
    if (p == NULL) { *errout = 0x100e; return NULL; }
    p = AT(input, 0x70, void *);
    AT(cl, 0x108, void *) = p;
    if (p == NULL) { *errout = 0x100f; return NULL; }
    if ((AT(cl, 0x1d0, uint32_t) & flags) == flags) {
        AT(cl, 0, void *) = AT(input, 0x78, void *);
        *errout = 0;
        return cl;
    }
    *errout = 0x1005;
    return NULL;
}

/* fs_FindBlocForPpem 0x8178: does the face have a bitmap strike at (ppemX, ppemY)? *found. */
TT_EXPORT int tt_fs_FindBlocForPpem(uint8_t *input, void *info, uint16_t ppemX, uint16_t ppemY, uint8_t *found) {
    int32_t key_err = 0;
    *found = 0;
    uint8_t *cl = ORIG(uint8_t *(*)(uint8_t *, void *, uint32_t, int32_t *), 0x8034)(input, info, 2, &key_err);
    if (cl == NULL) return key_err;
    uint16_t kind = 0, a = 0, b = 0;
    uint32_t c = 0;
    int err = ORIG(int (*)(uint8_t *, uint16_t, uint16_t, uint16_t *, uint16_t *, uint16_t *, uint32_t *),
                   RVA_sfac_SearchForStrike)(cl, ppemX, ppemY, &kind, &a, &b, &c);
    if (err == 0) {
        *found = (kind == 2);
        return 0;
    }
    return err;
}

/* fs_NewGlyph 0x6c388: resolve the glyph id, bind the client, search for an embedded bitmap. */
TT_EXPORT int tt_fs_NewGlyph(uint8_t *input, uint8_t *info) {
    uint8_t *cl = AT(input, 8, uint8_t *);
    if (cl == NULL || AT(input, 0x20, void *) == NULL || AT(input, 0x28, void *) == NULL) return 0x1003;
    AT(cl, 0x118, void *) = input + 8;
    AT(cl, 0x120, void *) = info;
    if (info == NULL) return 0x1003;
    void *p = AT(input, 0x50, void *);
    AT(cl, 8, void *) = p;
    if (p == NULL) return 0x1008;
    p = AT(input, 0x58, void *);
    AT(cl, 0x10, void *) = p;
    if (p == NULL) AT(cl, 0x10, void *) = (uint8_t *)idwr_orig(RVA_SmartPtrDtor);
    p = AT(input, 0x60, void *);
    AT(cl, 0xf8, void *) = p;
    if (p == NULL) return 0x100d;
    p = AT(input, 0x68, void *);
    AT(cl, 0x100, void *) = p;
    if (p == NULL) return 0x100e;
    p = AT(input, 0x70, void *);
    AT(cl, 0x108, void *) = p;
    if (p == NULL) return 0x100f;
    if ((AT(cl, 0x1d0, uint32_t) & 6) != 6) return 0x1005;
    AT(cl, 0, void *) = AT(input, 0x78, void *);
    uint16_t id;
    if (AT(input, 0x90, uint16_t) == 0xffff) {
        AT(cl, 0xe8, uint16_t) = AT(input, 0x92, uint16_t);
        id = AT(input, 0x92, uint16_t);
    } else {
        int e = ORIG(int (*)(uint8_t *, uint16_t), RVA_sfac_GetGlyphIndex)(cl, AT(input, 0x90, uint16_t));
        if (e != 0) return e;
        id = AT(cl, 0xe8, uint16_t);
    }
    AT(info, 0x24, uint16_t) = id;
    uint64_t ng = (uint64_t)AT(cl, 0x1b4, uint16_t) - 1;
    if (ng >= 0x10000) of();
    if (AT(cl, 0xe8, uint16_t) > (uint16_t)ng) return 0x100a;
    AT(cl, 0x351, uint8_t) = (AT(input, 0x95, uint8_t) == 0);
    AT(cl, 0x350, uint8_t) = AT(input, 0x94, uint8_t);
    int16_t sVar6 = 0;
    if (AT(input, 0x95, uint8_t) == 0 && AT(cl, 0x488, int16_t) == 0) {
        int e = ORIG(int (*)(uint8_t *, uint8_t *, uint16_t, uint16_t *), RVA_sbit_SearchForBitmap)(
            cl + 0x3c0, cl, AT(cl, 0xe8, uint16_t), (uint16_t *)(info + 0xa8));
        if (e != 0) return e;
    } else {
        AT(info, 0xa8, uint16_t) = 0;
    }
    int16_t s1 = AT(cl, 0x488, int16_t);
    if (s1 != 0) sVar6 = (int16_t)(s1 * s1 + 1);
    AT(info, 0xdc, int16_t) = sVar6;
    int16_t aa = AT(info, 0xa8, int16_t);
    AT(cl, 0x1d0, uint32_t) = 0xe;
    AT(cl, 0x351, uint8_t) = (aa != 0);
    return 0;
}

/* ============================ fsg_* workspace address updates ============================ */

/* fsg_UpdateWorkSpaceElement 0x1cc07c: rebase the scan-conversion element pointers in the
 * work-space addr record (p2[0x10] array) onto the fresh base p2[8]. */
TT_EXPORT void tt_fsg_UpdateWorkSpaceElement(uint8_t *off, uint8_t *addr) {
    uint8_t *base = AT(addr, 8, uint8_t *);
    uint8_t *dst = AT(addr, 0x10, uint8_t *);
    for (long i = 0; i < 6; i++)
        AT(dst, i * 8, uint8_t *) = base + AT(off, 0x14 + i * 4, uint32_t);
    AT(dst, 0x38, uint8_t *) = base + AT(off, 0x30, uint32_t);
    AT(dst, 0x40, uint8_t *) = base + AT(off, 0x34, uint32_t);
    AT(dst, 0x30, uint8_t *) = base + AT(off, 0x2c, uint32_t);
    AT(dst, 0x48, uint8_t *) = base + AT(off, 0x38, uint32_t);
    AT(dst, 0x58, uint8_t *) = base + AT(off, 0x3c, uint32_t);
    AT(dst, 0x68, uint8_t *) = base + AT(off, 0x40, uint32_t);
}

#define NOWRAP(b, x) do { if ((uintptr_t)((b) + (x)) < (uintptr_t)(b)) of(); } while (0)

static void priv_consts(uint8_t *wp, uint8_t *mp, uint16_t uVar9) {
    AT(wp, 8, uint16_t) = AT(mp, 0x18, uint16_t);
    uint16_t u5 = AT(mp, 0xa, uint16_t);
    if (AT(mp, 0xa, uint16_t) <= AT(mp, 6, uint16_t)) u5 = AT(mp, 6, uint16_t);
    AT(wp, 0x1b0, uint32_t) = (uint32_t)u5 + 4;
    AT(wp, 0x178, uint8_t *) = mp;
    AT(wp, 0x180, int16_t) = (int16_t)uVar9;
    wp[0x171] = 0;
    AT(wp, 0x195, uint16_t) = 1;
    AT(wp, 0x198, uint32_t) = 0x2710;
    AT(wp, 0x19c, uint32_t) = 0x64;
    AT(wp, 0x1a0, uint32_t) = 0x989680;
}

/* fsg_UpdatePrivateSpaceAddresses 0xca2b4: build the two interpreter work-space elements
 * (main + composite) from the offset table, rebased onto base, and report the instruction and
 * single-width arrays through param_7..param_10. param_4 is unused by the original. */
TT_EXPORT void tt_fsg_UpdatePrivateSpaceAddresses(uint8_t *cl, uint8_t *mp, uint8_t *base, uint32_t p4,
                                                  uint8_t *off, void *p6, void **p7, uint32_t *p8,
                                                  void **p9, uint32_t *p10) {
    (void)p4;
    uint32_t uVar10 = AT(off, 0xc, uint32_t);
    NOWRAP(base, uVar10);
    uint8_t *pcVar14 = base + AT(off, 0, uint32_t); NOWRAP(base, 0u); if ((uintptr_t)pcVar14 < (uintptr_t)base) of();
    uint32_t uVar1 = AT(off, 4, uint32_t); NOWRAP(base, uVar1);
    uint32_t uVar2 = AT(off, 8, uint32_t); NOWRAP(base, uVar2);
    uint8_t *pcVar6 = base + AT(off, 0x70, uint32_t); if ((uintptr_t)pcVar6 < (uintptr_t)base) of();
    uint8_t *pcVar7 = base + AT(off, 0x10, uint32_t); if ((uintptr_t)pcVar7 < (uintptr_t)base) of();
    uint8_t *pcVar8 = base + AT(off, 0x14, uint32_t); if ((uintptr_t)pcVar8 < (uintptr_t)base) of();
    uint32_t uVar3 = AT(cl, 0x6c, uint32_t);
    uint8_t *pcVar11 = base + AT(off, 0x18, uint32_t);
    *p7 = pcVar8;
    *p8 = uVar3;
    if ((uintptr_t)pcVar11 >= (uintptr_t)base) {
        uint32_t uVar9 = AT(cl, 0x44, uint32_t);
        uint32_t uVar4 = AT(cl, 0x4c, uint32_t);
        *p9 = pcVar11;
        *p10 = uVar4;
        if (uVar9 != 0) { uVar9 >>= 1; if (uVar9 > 0xffff) of(); }
        AT(pcVar7, 0x1a8, void *) = AT(cl, 0, void *);
        AT(pcVar7, 0x18, uint8_t *) = base + uVar10;
        AT(pcVar7, 0xb8, uint8_t *) = base + uVar1;
        AT(pcVar7, 0, void *) = p6;
        AT(pcVar7, 0x1e0, uint8_t *) = pcVar6;
        AT(pcVar7, 0x10, uint8_t *) = pcVar14;
        AT(pcVar7, 0xc0, uint8_t *) = base + uVar2;
        priv_consts(pcVar7, mp, (uint16_t)uVar9);
        AT(pcVar7, 0x108, uint32_t) = uVar3;
        AT(pcVar7, 0x100, void *) = (uVar3 == 0) ? NULL : pcVar8;
        AT(pcVar7, 0xf8, uint32_t) = uVar4;
        AT(pcVar7, 0xf0, void *) = (uVar4 == 0) ? NULL : pcVar11;
        memset(pcVar14, 0, (size_t)AT(mp, 0x12, uint16_t) << 2);
        uVar10 = AT(off, 0x60, uint32_t);
        NOWRAP(base, uVar10);
        uint8_t *pcVar14b = base + AT(off, 0x54, uint32_t); if ((uintptr_t)pcVar14b < (uintptr_t)base) of();
        uVar1 = AT(off, 0x58, uint32_t); NOWRAP(base, uVar1);
        uVar2 = AT(off, 0x5c, uint32_t); NOWRAP(base, uVar2);
        uint8_t *pcVar7b = base + AT(off, 0x64, uint32_t);
        if ((uintptr_t)pcVar7b >= (uintptr_t)base) {
            AT(pcVar7b, 0x1a8, void *) = AT(cl, 0, void *);
            AT(pcVar7b, 0x18, uint8_t *) = base + uVar10;
            void *pvVar13 = *p9;
            uint32_t uVar10b = *p10;
            AT(pcVar7b, 0x10, uint8_t *) = pcVar14b;
            AT(pcVar7b, 0xb8, uint8_t *) = base + uVar1;
            AT(pcVar7b, 0xc0, uint8_t *) = base + uVar2;
            AT(pcVar7b, 0, void *) = p6;
            AT(pcVar7b, 0x1e0, uint8_t *) = pcVar6;
            priv_consts(pcVar7b, mp, (uint16_t)uVar9);
            void *pvVar12 = (*p8 != 0) ? *p7 : NULL;
            AT(pcVar7b, 0x108, uint32_t) = (*p8 != 0) ? *p8 : 0;
            AT(pcVar7b, 0x100, void *) = pvVar12;
            if (uVar10b == 0) { pvVar13 = NULL; uVar10b = 0; }
            AT(pcVar7b, 0xf8, uint32_t) = uVar10b;
            AT(pcVar7b, 0xf0, void *) = pvVar13;
            memset(pcVar14b, 0, (size_t)AT(mp, 0x12, uint16_t) << 2);
            return;
        }
    }
    of();
}

/* ============================ fs__Contour ============================ */

/* SafeIntOnOverflow inside fs__Contour: the original throws; unreachable with valid fonts. */
static int of_ret(void) { of(); return 0; }

/* 32.32 round-and-clamp to int32 used by the compatible-metrics scaling loop. */
static inline int32_t clamp_fixmul(int32_t v, int32_t scale) {
    int64_t p = (int64_t)v * (int64_t)scale;
    p = (p >> 63) + 0x8000 + p;
    int64_t s = p >> 0x10;
    if (s < 0x80000000LL) return (s < -0x80000000LL) ? (int32_t)0x80000000 : (int32_t)(p >> 0x10);
    return 0x7fffffff;
}

/* fs__Contour 0xc8f0: the hinting entry. Sets up the scaler work space, runs the prep/cvt
 * program once per transform, grid-fits the glyph (fsg_ExecuteGlyph / fsg_GridFit) or reads an
 * embedded bitmap advance, and writes the device advance/side-bearing metrics into the info
 * record the oracle reads. */
TT_EXPORT int tt_fs__Contour(uint8_t *input, uint8_t *info, char flag) {
    typedef int (*fn2i)(uint8_t *, void *);
    uint8_t *cl = AT(input, 8, uint8_t *);
    uint64_t local_78 = 0, local_68 = 0;
    uint8_t *local_60 = NULL, *local_58 = NULL;
    uint32_t local_res20 = 0;
    void *local_70 = NULL;

    if (cl == NULL || AT(input, 0x20, void *) == NULL || AT(input, 0x28, void *) == NULL) return 0x1003;
    AT(cl, 0x118, void *) = input + 8;
    AT(cl, 0x120, void *) = info;
    if (info == NULL) return 0x1003;
    void *p = AT(input, 0x50, void *); AT(cl, 8, void *) = p; if (p == NULL) return 0x1008;
    p = AT(input, 0x58, void *); AT(cl, 0x10, void *) = p;
    if (p == NULL) AT(cl, 0x10, void *) = (uint8_t *)idwr_orig(RVA_SmartPtrDtor);
    p = AT(input, 0x60, void *); AT(cl, 0xf8, void *) = p; if (p == NULL) return 0x100d;
    p = AT(input, 0x68, void *); AT(cl, 0x100, void *) = p; if (p == NULL) return 0x100e;
    p = AT(input, 0x70, void *); AT(cl, 0x108, void *) = p; if (p == NULL) return 0x100f;
    if ((AT(cl, 0x1d0, uint32_t) & 0xe) != 0xe) return 0x1005;
    AT(cl, 0, void *) = AT(input, 0x78, void *);

    uint8_t *gbase = AT(input, 0x20, uint8_t *);
    if (AT(cl, 0x140, void *) != (void *)gbase || AT(cl, 0x148, void *) != AT(input, 0x28, void *)) {
        AT(cl, 0x1d8, uint8_t *) = gbase + AT(cl, 0x208, uint32_t);
        AT(cl, 0x1e0, uint8_t *) = gbase + AT(cl, 0x20c, uint32_t);
        AT(cl, 0x1e8, uint8_t *) = gbase + AT(cl, 0x210, uint32_t);
        AT(cl, 0x1f0, uint8_t *) = gbase + AT(cl, 0x214, uint32_t);
        AT(cl, 0x1f8, uint8_t *) = gbase + AT(cl, 0x218, uint32_t);
        AT(cl, 0x200, uint8_t *) = gbase + AT(cl, 0x24c, uint32_t);
        void *o50 = NULL, *o70 = NULL; uint32_t o84 = 0, oR8 = 0;
        ORIG(void (*)(uint8_t *, uint8_t *, uint8_t *, uint32_t, uint8_t *, void *, void **, uint32_t *, void **, uint32_t *),
             RVA_fsg_UpdatePrivateSpaceAddresses)(
            cl, cl + 0x1b0, AT(AT(cl, 0x118, uint8_t *), 0x20, uint8_t *), 0, cl + 0x258,
            gbase + AT(cl, 0x208, uint32_t), &o50, &o84, &o70, &oR8);
        if ((uint64_t)8u * 9u >= 0x100000000ULL) of();
        memcpy(cl + 0x128, AT(cl, 0x118, void *), 8u * 9u);
    }

    uint8_t uVar29 = 0;
    uint8_t *lVar21 = AT(cl, 0x1e0, uint8_t *);
    int64_t *el = AT(cl, 0x1e8, void *);
    el[0] = (int64_t)(lVar21 + AT(cl, 0x21c, uint32_t));
    el[1] = (int64_t)(lVar21 + AT(cl, 0x220, uint32_t));
    el[2] = (int64_t)(lVar21 + AT(cl, 0x224, uint32_t));
    el[3] = (int64_t)(lVar21 + AT(cl, 0x228, uint32_t));
    el[4] = (int64_t)(lVar21 + AT(cl, 0x22c, uint32_t));
    el[5] = (int64_t)(lVar21 + AT(cl, 0x230, uint32_t));
    el[7] = (int64_t)(lVar21 + AT(cl, 0x238, uint32_t));
    el[8] = (int64_t)(lVar21 + AT(cl, 0x23c, uint32_t));
    el[6] = (int64_t)(lVar21 + AT(cl, 0x234, uint32_t));
    el[9] = (int64_t)(lVar21 + AT(cl, 0x240, uint32_t));
    el[0xb] = (int64_t)(lVar21 + AT(cl, 0x244, uint32_t));
    el[0xd] = (int64_t)(lVar21 + AT(cl, 0x248, uint32_t));

    uint8_t *pfVar5 = AT(AT(cl, 0x118, uint8_t *), 0x20, uint8_t *);
    uint8_t *pfVar35 = pfVar5 + AT(cl, 0x268, uint32_t);
    if ((uintptr_t)pfVar35 < (uintptr_t)pfVar5) return of_ret();
    if (AT(cl, 0x41a, uint8_t) & 2) {
        local_60 = pfVar5 + AT(cl, 0x2bc, uint32_t);
        if ((uintptr_t)local_60 < (uintptr_t)pfVar5) return of_ret();
        uint8_t *pfVar20 = pfVar5 + AT(cl, 0x2c4, uint32_t);
        if ((uintptr_t)pfVar20 < (uintptr_t)pfVar5) return of_ret();
        local_58 = pfVar5 + AT(cl, 0x2c0, uint32_t);
        if ((uintptr_t)local_58 < (uintptr_t)pfVar5) return of_ret();
        AT(local_58, 0x00, uint8_t *) = pfVar20 + AT(cl, 0x27c, uint32_t);
        AT(local_58, 0x08, uint8_t *) = pfVar20 + AT(cl, 0x280, uint32_t);
        AT(local_58, 0x10, uint8_t *) = pfVar20 + AT(cl, 0x284, uint32_t);
        AT(local_58, 0x18, uint8_t *) = pfVar20 + AT(cl, 0x288, uint32_t);
        AT(local_58, 0x20, uint8_t *) = pfVar20 + AT(cl, 0x28c, uint32_t);
        AT(local_58, 0x28, uint8_t *) = pfVar20 + AT(cl, 0x290, uint32_t);
        AT(local_58, 0x38, uint8_t *) = pfVar20 + AT(cl, 0x298, uint32_t);
        AT(local_58, 0x40, uint8_t *) = pfVar20 + AT(cl, 0x29c, uint32_t);
        AT(local_58, 0x30, uint8_t *) = pfVar20 + AT(cl, 0x294, uint32_t);
        AT(local_58, 0x48, uint8_t *) = pfVar20 + AT(cl, 0x2a0, uint32_t);
        AT(local_58, 0x68, uint8_t *) = pfVar20 + AT(cl, 0x2a8, uint32_t);
    }
    if ((AT(cl, 0x41a, uint16_t) & 1) && !(AT(cl, 0x41a, uint16_t) & 2)) {
        uVar29 = 1;
        local_res20 = (local_res20 & 0xffffff00) | 1;
    }

    uint8_t *plVar17 = pfVar5 + AT(cl, 0x278, uint32_t);
    if ((uintptr_t)plVar17 < (uintptr_t)pfVar5) return of_ret();
    int64_t *ws = (int64_t *)(pfVar5 + AT(cl, 0x274, uint32_t));
    if ((uintptr_t)ws < (uintptr_t)pfVar5) return of_ret();
    ws[0] = (int64_t)(plVar17 + AT(cl, 0x27c, uint32_t));
    ws[1] = (int64_t)(plVar17 + AT(cl, 0x280, uint32_t));
    ws[2] = (int64_t)(plVar17 + AT(cl, 0x284, uint32_t));
    ws[3] = (int64_t)(plVar17 + AT(cl, 0x288, uint32_t));
    ws[4] = (int64_t)(plVar17 + AT(cl, 0x28c, uint32_t));
    ws[5] = (int64_t)(plVar17 + AT(cl, 0x290, uint32_t));
    ws[7] = (int64_t)(plVar17 + AT(cl, 0x298, uint32_t));
    ws[8] = (int64_t)(plVar17 + AT(cl, 0x29c, uint32_t));
    ws[6] = (int64_t)(plVar17 + AT(cl, 0x294, uint32_t));
    ws[9] = (int64_t)(plVar17 + AT(cl, 0x2a0, uint32_t));
    ws[0xb] = (int64_t)(plVar17 + AT(cl, 0x2a4, uint32_t));
    ws[0xd] = (int64_t)(plVar17 + AT(cl, 0x2a8, uint32_t));

    if (AT(cl, 0x414, uint8_t) == 0) { AT(cl, 0x1d4, uint8_t) = 0; flag = 0; }
    uint8_t res8_lo = uVar29;

    if (AT(cl, 0x1d4, uint8_t) != 0) {
        AT(cl, 0x1d4, uint8_t) = 0;
        AT(pfVar35, 0x1a0, uint32_t) = 0x989680;
        int32_t iv = AT(cl, 0x1a0, int32_t);
        AT(pfVar35, 0x50, void *) = (uint8_t *)idwr_orig(0xad020); /* itrp_RoundToGrid */
        AT(pfVar35, 0x58, uint16_t) = 3;
        AT(pfVar35, 0x4c, uint32_t) = 0x40;
        AT(pfVar35, 0x38, uint64_t) = 0x44;
        AT(pfVar35, 0x6c, uint32_t) = 0x80;
        AT(pfVar35, 0x70, uint8_t) = 1;
        AT(pfVar35, 0x68, uint32_t) = 0x30009;
        iv = (0x16c0a - iv) >> 10;
        AT(pfVar35, 0x44, uint32_t) = (uint32_t)(-(int)(AT(pfVar35, 0x1bf, uint8_t) != 0)) & 0x120;
        AT(pfVar35, 0x48, uint32_t) = 0;
        AT(pfVar35, 0x28, int32_t) = iv;
        AT(pfVar35, 0x2c, int32_t) = -iv;
        AT(pfVar35, 0x30, uint32_t) = 0;
        AT(pfVar35, 0x24, uint32_t) = 0;
        int32_t *cvt = AT(pfVar35, 0x18, int32_t *);
        uint32_t e = ORIG(fn2i, RVA_sfac_CopyCVT)(cl, cvt);
        if (e != 0) { AT(cl, 0x414, uint8_t) = 0; return (int)e; }
        if (AT(pfVar35, 0x180, int16_t) != 0)
            ORIG(void (*)(uint8_t *, void *, int32_t *, void *, uint32_t), RVA_scl_Scale)(
                pfVar35 + 0x150, AT(pfVar35, 0xe8, void *), cvt, AT(pfVar35, 0x18, void *),
                (uint32_t)AT(pfVar35, 0x180, uint16_t));
        uint16_t np = AT(cl, 0x1c0, uint16_t);
        size_t sz = (size_t)np * 4;
        memset((void *)ws[0], 0, sz); memset((void *)ws[2], 0, sz); memset((void *)ws[4], 0, sz);
        memset((void *)ws[1], 0, sz); memset((void *)ws[3], 0, sz); memset((void *)ws[5], 0, sz);
        memset((void *)ws[6], 0, np); memset((void *)ws[9], 0, np);
        *(uint16_t *)ws[7] = 0; *(uint16_t *)ws[8] = 0;
        np = AT(cl, 0x1c0, uint16_t);
        if (np > 0x7fff) return of_ret();
        *(uint16_t *)ws[7] = 0; *(uint16_t *)ws[8] = (uint16_t)(np - 1);
        AT((uint8_t *)ws, 0x50, uint16_t) = 1;
        void *elptr = AT(cl, 0x1e8, void *);
        AT(pfVar35, 0x16a, uint16_t) = 1;
        AT(pfVar35, 0x78, uint64_t) = AT(pfVar35, 0x38, uint64_t);
        AT(pfVar35, 0x80, uint64_t) = AT(pfVar35, 0x40, uint64_t);
        AT(pfVar35, 0x88, uint64_t) = AT(pfVar35, 0x48, uint64_t);
        AT(pfVar35, 0x90, uint64_t) = AT(pfVar35, 0x50, uint64_t);
        AT(pfVar35, 0x98, uint32_t) = AT(pfVar35, 0x58, uint32_t);
        AT(pfVar35, 0x9c, uint32_t) = AT(pfVar35, 0x5c, uint32_t);
        AT(pfVar35, 0xa0, uint32_t) = AT(pfVar35, 0x60, uint32_t);
        AT(pfVar35, 0xa4, uint32_t) = AT(pfVar35, 0x64, uint32_t);
        AT(pfVar35, 0xa8, uint32_t) = AT(pfVar35, 0x68, uint32_t);
        AT(pfVar35, 0xac, uint32_t) = AT(pfVar35, 0x6c, uint32_t);
        AT(pfVar35, 0xb0, uint32_t) = AT(pfVar35, 0x70, uint32_t);
        AT(pfVar35, 0xb4, uint32_t) = AT(pfVar35, 0x74, uint32_t);
        uint8_t *prog = AT(pfVar35, 0xf0, uint8_t *);
        uint32_t ex = 0;
        if (prog != NULL)
            /* the 6th argument is the trace function: a full qword 0 (MOV [RSP+0x28], R15) */
            ex = ORIG(int (*)(void *, void *, uint8_t *, uint8_t *, uint8_t *, void *), RVA_itrp_Execute)(
                ws, elptr, prog, prog + AT(pfVar35, 0xf8, uint32_t), pfVar35, NULL);
        if (!(AT(pfVar35, 0x88, uint8_t) & 2)) {
            AT(pfVar35, 0x38, uint64_t) = AT(pfVar35, 0x78, uint64_t);
            AT(pfVar35, 0x40, uint64_t) = AT(pfVar35, 0x80, uint64_t);
            AT(pfVar35, 0x48, uint64_t) = AT(pfVar35, 0x88, uint64_t);
            AT(pfVar35, 0x50, uint64_t) = AT(pfVar35, 0x90, uint64_t);
            AT(pfVar35, 0x58, uint64_t) = AT(pfVar35, 0x98, uint64_t);
            AT(pfVar35, 0x60, uint64_t) = AT(pfVar35, 0xa0, uint64_t);
            AT(pfVar35, 0x68, uint64_t) = AT(pfVar35, 0xa8, uint64_t);
            AT(pfVar35, 0x70, uint64_t) = AT(pfVar35, 0xb0, uint64_t);
        }
        if ((int)ex != 0) { AT(cl, 0x414, uint8_t) = 0; return (int)ex; }
        uVar29 = (uint8_t)local_res20;
        res8_lo = (uint8_t)local_res20;
        if (AT(cl, 0x41a, uint8_t) & 2) {
            uint32_t pe = ORIG(int (*)(uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, void *),
                                RVA_fsg_RunPreProgram)(cl, cl + 0x1b0, cl + 0x41c, local_60, cl + 0x1d8, local_58, NULL);
            res8_lo = uVar29;
            if (pe != 0) { AT(cl, 0x414, uint8_t) = 0; return (int)pe; }
        }
    }

    int32_t iVar27, iVar30, iVar33, iVar31;
    uint8_t svar11;

    if (AT(input, 0x90, uint8_t) == 0 || AT(cl, 0x351, uint8_t) == 0 ||
        (AT(input, 0x88, uint8_t) != 0 && (AT(cl, 0x41a, uint8_t) & 1))) {
        /* ---- grid-fit / scan path ---- */
        AT(cl, 0x3b9, uint8_t) = 0;
        uint16_t np = AT(cl, 0x1c0, uint16_t);
        local_68 = AT(input, 0x80, uint64_t);
        uint32_t local_84 = (uint32_t)AT(cl, 0x49c, uint16_t);
        uint16_t local_80 = AT(cl, 0x49a, uint16_t);
        if (np > 0x7fff) return of_ret();
        local_70 = (void *)((uint64_t)(uintptr_t)local_70 & 0xffffffff00000000ULL);
        local_78 = local_78 & 0xffffffff00000000ULL;
        local_res20 &= 0xffffff00;
        *(uint16_t *)ws[7] = 0; *(uint16_t *)ws[8] = (uint16_t)(np - 1);
        AT((uint8_t *)ws, 0x50, uint16_t) = 1;
        AT(cl, 0x3b8, uint8_t) = 0;
        uint16_t cut = AT(cl, 0x1ce, uint16_t);
        uint32_t uVar12 = (cut == 0) ? 1 : (cut <= 0x14 ? cut : 0x14);
        uint16_t sc = AT(cl, 0x1cc, uint16_t);
        if (sc < 3) sc = 3;
        uint32_t nslots = (uint32_t)sc + uVar12 + 1;
        if (nslots < uVar12 + 1) return of_ret();
        if (nslots != 0) memset(AT(cl, 0x1f0, void *), 1, nslots);
        uint8_t *slots = AT(cl, 0x1f0, uint8_t *);
        uint32_t idx = 0;
        while (slots[idx] == 0 && idx < nslots) idx++;
        if (idx == nslots) return 0x140c;
        slots[idx] = 0;
        uint16_t gid = AT(cl, 0xe8, uint16_t);
        uint8_t *rec = (uint8_t *)((uint64_t)idx * 0xf8 + (uintptr_t)AT(cl, 0x1f8, uint8_t *));
        AT(rec, 0x60, uint32_t) = 0x10000; AT(rec, 0x64, uint32_t) = 0; AT(rec, 0x68, uint32_t) = 0; AT(rec, 0x6c, uint32_t) = 0;
        AT(rec, 0x00, uint64_t) = 0;
        AT(rec, 0x70, uint32_t) = 0x10000; AT(rec, 0x74, uint32_t) = 0; AT(rec, 0x78, uint32_t) = 0; AT(rec, 0x7c, uint32_t) = 0;
        AT(rec, 0x80, uint32_t) = 0x10000;
        AT(rec, 0x08, uint64_t) = 0; AT(rec, 0x10, uint64_t) = 0;
        AT(rec, 0x38, uint32_t) = 3;
        AT(rec, 0x20, uint64_t) = 0; AT(rec, 0x30, uint64_t) = 0; AT(rec, 0x28, uint64_t) = 0;
        AT(rec, 0x18, uint32_t) = 0;
        AT(rec, 0x4e, uint16_t) = 0; AT(rec, 0x85, uint16_t) = 0; AT(rec, 0x84, uint8_t) = 0;
        AT(rec, 0x3e, uint32_t) = 0x7fff7fff; AT(rec, 0x42, uint32_t) = 0x80008000;
        AT(rec, 0x98, uint32_t) = 0xffff; AT(rec, 0xa0, uint64_t) = 0;
        AT(rec, 0x46, uint16_t) = 0; AT(rec, 0x4a, uint16_t) = 0;
        AT(rec, 0x50, uint32_t) = 2; AT(rec, 0x54, uint8_t) = 0;
        AT(rec, 0x3c, uint16_t) = gid;
        AT(rec, 0x56, uint64_t) = 0; AT(rec, 0x88, uint64_t) = 0; AT(rec, 0x90, uint64_t) = 0;
        uint8_t *zone = AT(cl, 0x1e8, uint8_t *);
        AT(rec, 0xa8, uint8_t *) = zone; AT(zone, 0x50, uint16_t) = 0;
        AT(rec, 0xb0, uint32_t) = 0x10000; AT(rec, 0xb4, uint32_t) = 0; AT(rec, 0xb8, uint32_t) = 0; AT(rec, 0xbc, uint32_t) = 0;
        AT(rec, 0xd4, uint8_t) = 1;
        AT(rec, 0xc0, uint32_t) = 0x10000; AT(rec, 0xc4, uint32_t) = 0; AT(rec, 0xc8, uint32_t) = 0; AT(rec, 0xcc, uint32_t) = 0;
        AT(rec, 0xd0, uint32_t) = 0x10000;
        AT(rec, 0xd8, uint64_t) = 0; AT(rec, 0xe0, uint64_t) = 0; AT(rec, 0xe8, uint64_t) = 0; AT(rec, 0xf0, uint64_t) = 0;
        AT(pfVar35, 0x1a0, uint32_t) = 0x989680;

        typedef int (*execglyph_t)(uint8_t *, uint8_t *, uint8_t *, uint32_t, uint8_t *, uint8_t *,
                                   uint8_t *, void *, uint64_t, uint8_t, uint32_t *, void **, uint64_t *);
        while (rec != NULL) {
            int ge = ORIG(execglyph_t, RVA_fsg_ExecuteGlyph)(
                cl, cl + 0x1b0, cl + 0x174, nslots, pfVar35, rec, cl + 0x1d8, ws, local_68, (uint8_t)flag,
                &local_res20, &local_70, &local_78);
            if (ge != 0) return ge;
            AT(cl, 0x3b8, uint8_t) |= (uint8_t)local_res20;
            AT(cl, 0x170, uint16_t) = AT(rec, 0x98, uint16_t);
            uint8_t *next = AT(rec, 0x08, uint8_t *);
            if (next == NULL) {
                next = AT(rec, 0x00, uint8_t *);
                size_t ri = (size_t)((rec - AT(cl, 0x1f8, uint8_t *)) / 0xf8);
                AT(cl, 0x1f0, uint8_t *)[ri] = 1;
            } else {
                AT(rec, 0x08, uint64_t) = 0;
            }
            AT(cl, 0x1ac, uint16_t) = AT(rec, 0x46, uint16_t);
            rec = next;
        }
        svar11 = res8_lo;
        if (AT(cl, 0x1a8, uint8_t) != 0) {
            ORIG(void (*)(uint8_t *, uint8_t *, uint8_t, int, uint8_t), RVA_fsg_Embold)(
                cl + 0x1d8, pfVar35, (uint8_t)flag, AT(input, 0x8c, int32_t) != 0, svar11);
            if (AT(cl, 0x1ac, int16_t) != 0) {
                int32_t x = (uint32_t)AT(cl, 0x174, uint16_t) * 2 - 1;
                int16_t add = (int16_t)(((int16_t)(x / 100) + (int16_t)(x >> 0x1f)) -
                                        (int16_t)((int64_t)x * 0x51eb851f >> 0x3f));
                AT(cl, 0x1ac, int16_t) = (int16_t)(AT(cl, 0x1ac, int16_t) + add);
            }
        }
        if ((AT(cl, 0x1a4, uint32_t) & 0x2000) != 0 || AT(pfVar35, 0x1d4, uint8_t) != 0)
            ORIG(void (*)(uint8_t *, void *, uint8_t *), RVA_scl_PostTransformGlyph)(
                pfVar35, AT(cl, 0x1e8, void *), cl + 0x178);

        int64_t *e2 = AT(cl, 0x1e8, void *);
        iVar27 = (AT(cl, 0x180, int32_t) + 0x200) >> 10;
        iVar30 = (AT(cl, 0x18c, int32_t) + 0x200) >> 10;
        if (svar11 != 0) { iVar27 = iVar27 * (int32_t)local_80; iVar30 = iVar30 * (int32_t)(local_84 & 0xffff); }
        int16_t nc2 = *(int16_t *)((uint8_t *)e2[8] - 2 + (int16_t)e2[0xa] * 2);
        int64_t off1 = (uint16_t)(nc2 + 1) * 4;
        int32_t dx = iVar27 - *(int32_t *)((uint8_t *)e2[0] + off1);
        iVar30 = iVar30 - *(int32_t *)((uint8_t *)e2[1] + off1);
        if (flag != 0 && AT(pfVar35, 0x1d4, uint8_t) == 0)
            dx = (svar11 == 0) ? ((dx + 0x20) & (int32_t)0xffffffc0) : ((dx + 2) & (int32_t)0xfffffffc);
        if ((dx != 0 || iVar30 != 0) && nc2 != -9) {
            int32_t i = 0; int64_t o = 0;
            do {
                *(int32_t *)((uint8_t *)e2[0] + o) += dx;
                *(int32_t *)((uint8_t *)e2[1] + o) += iVar30;
                i++; o += 4;
            } while (i < (int32_t)(uint16_t)(*(int16_t *)((uint8_t *)e2[8] - 2 + (int16_t)e2[0xa] * 2) + 9));
        }

        if (AT(cl, 0x41a, uint8_t) & 1) {
            local_84 = (uint32_t)AT(cl, 0x49c, uint16_t) << 0x10;
            int32_t xs = (int32_t)((uint32_t)AT(cl, 0x49a, uint16_t) << 0x10);
            local_70 = (void *)((uint64_t)(uintptr_t)local_70 & 0xffffffff00000000ULL);
            local_res20 = 0;
            int32_t cdhm = 0;
            if (AT(cl, 0x41a, uint8_t) & 2) {
                uint32_t advance = 0;
                int found_min = 0;
                if (AT(cl, 0x351, uint8_t) == 0) {
                    int64_t *e3 = AT(cl, 0x1e8, void *);
                    int32_t minx = 0x7fffffff;
                    int16_t nc3 = *(int16_t *)((uint8_t *)e3[8] - 2 + (int16_t)e3[0xa] * 2);
                    int32_t *xa = (int32_t *)e3[0];
                    uint16_t cnt = (uint16_t)(nc3 + 1);
                    advance = (uint32_t)(xa[(uint16_t)(nc3 + 2)] - xa[cnt]);
                    cdhm = (int32_t)advance;
                    if (cnt != 0) {
                        uint16_t k = cnt; int32_t *xp = xa;
                        do { if (*xp < minx) minx = *xp; xp++; k--; } while (k != 0);
                        if (minx != 0x7fffffff) found_min = 1;
                    }
                    if (!found_min)
                        OutputDebugStringA("scl_CalcDevHorMetrics called on an empty glyph\n");
                } else {
                    int32_t d3 = 0, d4 = 0, d5 = 0;
                    int ce = ORIG(int (*)(uint8_t *, uint8_t *, int32_t *, int32_t *, int32_t *),
                                  RVA_sbit_CalcDevHorMetrics)(cl + 0x3c0, cl, &d3, &d4, &d5);
                    cdhm = d3;
                    advance = (uint32_t)d3;
                    if (ce != 0) return ce;
                }
                int32_t fd;
                if (AT(cl, 0x1ac, int16_t) == 0 || advance == 0) {
                    fd = 0x10000;
                } else {
                    typedef int (*enginefn)(uint8_t *, uint32_t);
                    int32_t t = ((enginefn)AT(pfVar35, 0xd8, void *))(pfVar35 + 0x130, (uint32_t)AT(cl, 0x1ac, uint16_t));
                    int32_t q = ORIG(int (*)(int32_t, int32_t), RVA_DWRITE_FixDiv)((int32_t)advance, t);
                    fd = (-q < 0) ? q : -q;
                }
                AT(local_60, 0x1d0, int32_t) = fd;
#ifndef IDWR_STANDALONE
                typedef int (*gridfit_t)(uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *,
                                         void *, uint8_t, uint16_t *, uint8_t *, uint16_t *, int, uint8_t,
                                         uint16_t, uint16_t);
#else
                /* the port's own types: Apple arm64 packs stack arguments by their declared size */
                typedef int (*gridfit_t)(uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *,
                                         void *, uint8_t, uint16_t *, uint8_t *, uint16_t *, uint8_t, uint8_t,
                                         uint16_t, uint16_t);
#endif
                int gfe = ORIG(gridfit_t, RVA_fsg_GridFit)(
                    cl, cl + 0x1b0, cl + 0x41c, local_60, cl + 0x1d8, local_58, AT(input, 0x80, void *),
                    (uint8_t)flag, (uint16_t *)(cl + 0x170), cl + 0x3b8, (uint16_t *)(cl + 0x1ac),
                    AT(input, 0x8c, int32_t) != 0, (uint8_t)(AT(cl, 0x41a, uint8_t) & 1),
                    AT(cl, 0x49a, uint16_t), AT(cl, 0x49c, uint16_t));
                if (gfe != 0) return gfe;
                /* the original reloads the x scale from its stack slot, still overscale_x << 16 */
            }
            int64_t *e4 = AT(cl, 0x1e8, void *);
            uint16_t cnt = (uint16_t)(*(int16_t *)((uint8_t *)e4[8] - 2 + (int16_t)e4[0xa] * 2) + 9);
            uint16_t loopn = cnt;
            if (cnt != 0) {
                int32_t i = 0; int64_t o = 0;
                do {
                    int32_t *px = (int32_t *)((uint8_t *)e4[0] + o);
                    *px = clamp_fixmul(*px, xs);
                    int32_t *py = (int32_t *)((uint8_t *)e4[1] + o);
                    *py = clamp_fixmul(*py, (int32_t)local_84);
                    i++; o += 4;
                    loopn = (uint16_t)(*(int16_t *)((uint8_t *)e4[8] - 2 + (int16_t)e4[0xa] * 2) + 9);
                } while (i < (int32_t)loopn);
            }
            if (flag != 0 && (AT(cl, 0x41a, uint8_t) & 2))
                ORIG(void (*)(void *, uint32_t, int32_t), RVA_scl_AdjustCompatibleMetrics)(
                    AT(cl, 0x1e8, void *), loopn, (int32_t)((uint32_t)AT(cl, 0x49a, uint16_t) * (uint32_t)cdhm));
        }

        uint8_t *pfVar7 = AT(cl, 0x1e8, uint8_t *);
        uint8_t uVar1b = AT(cl, 0x41a, uint8_t);
        uint16_t os_y = AT(cl, 0x49c, uint16_t), os_x = AT(cl, 0x49a, uint16_t);
        AT(info, 0x78, uint64_t) = AT(pfVar7, 0x00, uint64_t);
        AT(info, 0x80, uint64_t) = AT(pfVar7, 0x08, uint64_t);
        AT(info, 0x88, uint64_t) = AT(pfVar7, 0x38, uint64_t);
        AT(info, 0x90, uint64_t) = AT(pfVar7, 0x40, uint64_t);
        AT(info, 0x98, uint64_t) = AT(pfVar7, 0x30, uint64_t);
        AT(info, 0xe0, uint64_t) = AT(pfVar7, 0x58, uint64_t);
        AT(info, 0x72, uint16_t) = AT(pfVar7, 0x50, uint16_t);
        if ((uVar1b & 0x41) == 1) {
            ORIG(void (*)(uint8_t *, uint16_t, uint16_t), RVA_scl_ScaleDownFromSubPixelOverscale)(pfVar7, os_x, os_y);
            AT(info, 0x78, uint64_t) = AT(pfVar7, 0x10, uint64_t);
            AT(info, 0x80, uint64_t) = AT(pfVar7, 0x18, uint64_t);
        }
        int64_t *e5 = AT(cl, 0x1e8, void *);
        uint8_t *bx = (uint8_t *)e5[0];
        uint8_t *by = (uint8_t *)e5[1];
        int16_t nc5 = *(int16_t *)((uint8_t *)e5[8] - 2 + (int16_t)e5[0xa] * 2);
        int64_t o2 = (uint16_t)(nc5 + 2) * 4;
        int64_t o4 = (uint16_t)(nc5 + 4) * 4;
        iVar27 = *(int32_t *)(bx + o2) - *(int32_t *)(bx + (uint16_t)(nc5 + 1) * 4);
        iVar30 = *(int32_t *)(by + o2) - *(int32_t *)(by + (uint16_t)(nc5 + 1) * 4);
        iVar33 = *(int32_t *)(bx + (uint16_t)(nc5 + 3) * 4) - *(int32_t *)(bx + o4);
        iVar31 = *(int32_t *)(by + (uint16_t)(nc5 + 3) * 4) - *(int32_t *)(by + o4);
    } else {
        /* ---- embedded-bitmap advance path ---- */
        AT(cl, 0x3b9, uint8_t) = 1;
        int32_t w[2] = {0, 0}, h[2] = {0, 0};
        int we = ORIG(int (*)(uint8_t *, uint8_t *, int32_t *), RVA_sbit_GetDevAdvanceWidth)(cl + 0x3c0, cl, w);
        if (we != 0) return we;
        we = ORIG(int (*)(uint8_t *, uint8_t *, int32_t *), RVA_sbit_GetDevAdvanceHeight)(cl + 0x3c0, cl, h);
        if (we != 0) return we;
        local_78 = (uint64_t)(uint32_t)w[0] | ((uint64_t)(uint32_t)w[1] << 32);
        local_68 = (uint64_t)(uint32_t)h[0] | ((uint64_t)(uint32_t)h[1] << 32);
        iVar27 = w[0]; iVar30 = w[1]; iVar33 = h[0]; iVar31 = h[1];
    }

    AT(info, 0x48, int32_t) = iVar27 * 0x400;
    AT(info, 0x4c, int32_t) = iVar30 * 0x400;
    AT(info, 0xcc, int32_t) = iVar33 * 0x400;
    AT(info, 0xd0, int32_t) = iVar31 * 0x400;
    if ((AT(cl, 0x41a, uint8_t) & 0x41) == 1) {
        AT(info, 0x48, int32_t) = (int32_t)((uint32_t)(AT(cl, 0x49a, uint16_t) >> 1) + iVar27 * 0x400) / (int32_t)(uint32_t)AT(cl, 0x49a, uint16_t);
        AT(info, 0x4c, int32_t) = (int32_t)((uint32_t)(AT(cl, 0x49c, uint16_t) >> 1) + iVar30 * 0x400) / (int32_t)(uint32_t)AT(cl, 0x49c, uint16_t);
        AT(info, 0xcc, int32_t) = (int32_t)((uint32_t)(AT(cl, 0x49a, uint16_t) >> 1) + iVar33 * 0x400) / (int32_t)(uint32_t)AT(cl, 0x49a, uint16_t);
        AT(info, 0xd0, int32_t) = (int32_t)((uint32_t)(AT(cl, 0x49c, uint16_t) >> 1) + iVar31 * 0x400) / (int32_t)(uint32_t)AT(cl, 0x49c, uint16_t);
    }
    AT(info, 0x70, uint8_t) = AT(cl, 0x3b8, uint8_t);
    uint8_t *obase = AT(AT(cl, 0x118, uint8_t *), 0x20, uint8_t *);
    uint64_t end = (uint64_t)AT(cl, 0x264, uint32_t) + (uint64_t)(uintptr_t)obase;
    if ((uint64_t)(uintptr_t)obase <= end) {
        AT(info, 0xa0, uint64_t) = end;
        AT(cl, 0x1d0, uint32_t) = 0x1e;
        return 0;
    }
    return of_ret();
}
