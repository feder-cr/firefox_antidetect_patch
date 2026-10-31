/*
 * TrueType scaler, glyph execution layer (fsg_*), DWrite.dll 10.0.26100.9711: loads a glyph's
 * outline (simple, or composite with its components loaded recursively), scales it, runs its
 * instructions and merges each component into its parent. Read from the assembly
 * (notes/truetype.md).
 *
 * fsg_GridFit walks a tree of glyph records (fsg_GlyphData, 0xf8 bytes, a slot pool in the
 * work space) depth first. fsg_ExecuteGlyph is a state machine on each record:
 *   3  new record: locate the glyph, read its header, metrics and vertical origin
 *   0  simple glyph: read the outline, grid fit it, merge it into the parent
 *   1  composite, first visit: allocate and link one child record per component
 *   2  composite, after its children: run the composite's instructions, merge
 *
 * Record layout (offsets into fsg_GlyphData):
 *   0x00 next (sibling, or the parent after the last child)  0x08 first child  0x10 parent
 *   0x18 sfac_GHandle {u32 size, 0x20 data, 0x28 fragment context, 0x30 outline pointer}
 *   0x38 state  0x3c glyph  0x3e bbox (4 x i16)  0x46 advance width  0x48 advance height
 *   0x4a lsb  0x4c tsb  0x4e depth  0x50 component type (1 offsets, 0 anchor points)
 *   0x54 args are xy  0x56/0x58 offset  0x5a/0x5c anchor points  0x60 component matrix (3x3)
 *   0x84 has phantom override  0x85 use my metrics  0x86 scaled offset  0x88 4 x i32 phantoms
 *   0x98 scan control  0x9a instruction length  0xa0 instructions  0xa8 element (zone)
 *   0xb0 accumulated matrix (3x3)  0xd4 same transform  0xd6 vertical origin
 *   0xd8 phantom deltas from the variation callback (4 x {x, y})
 * Work space (fsg_WorkSpaceAddr): 0x10 elements (0x70 per depth), 0x18 free map, 0x20 records.
 *
 * Calls between the functions of this file go through the original entry points, so a
 * function swapped alone (reimpl/oracle.py --swap) runs with the originals of the others,
 * and all of them swapped run only the port. Define TT_FSG_DIRECT to call the ports directly.
 */
#include <string.h>
#ifdef _WIN32
#include <windows.h> /* OutputDebugStringA; elsewhere tt_state.h */
#endif
#include "tt_state.h"
#include "orig.h"

/* RVAs of this group */
#define RVA_fsg_GridFit                 0x0c3e4
#define RVA_fsg_ExecuteGlyph            0x0dbdc
#define RVA_fsg_SimpleInnerGridFit      0x0fc0c
#define RVA_fsg_CompositeInnerGridFit   0x120ac
#define RVA_fsg_MergeGlyphData          0x68f7c
#define RVA_fsg_TransformChild          0x6925c
#define RVA_fsg_InitializeGlyphData     0xd12f8
#define RVA_fsg_DoScanControl           0xd5e04
#define RVA_fsg_LinkChild               0xdeb88
#define RVA_mth_MxConcat2x2             0xe743c
#define RVA_itrp_SetSameTransformFlag   0x12724
#define RVA_itrp_ExecuteGlyphPgm        0x1274c
#define RVA_AutoReleaseGlyph_Release    0xd14f0

typedef struct { uint8_t *client; uint8_t *handle; } AutoReleaseGlyph;

typedef int32_t (*ExecuteGlyph_fn)(uint8_t *, uint8_t *, uint8_t *, uint32_t, uint8_t *, uint8_t *, uint8_t *,
                                   uint8_t *, void *, uint8_t, uint8_t *, uint32_t *, uint32_t *);
typedef int32_t (*SimpleInnerGridFit_fn)(uint8_t *, uint16_t, int32_t, uint8_t *, uint8_t *, uint8_t *, uint8_t,
                                         void *, uint16_t, uint16_t *, uint16_t *, int16_t, int16_t, int16_t,
                                         uint8_t, uint8_t *, int16_t *, uint16_t, const uint8_t *, uint16_t *,
                                         uint16_t *, uint8_t *);
typedef int32_t (*CompositeInnerGridFit_fn)(uint8_t *, uint8_t *, uint8_t *, uint8_t, void *, uint16_t, uint16_t *,
                                            uint16_t *, int16_t, int16_t, int16_t, uint8_t, uint8_t *, int16_t *,
                                            uint16_t, const uint8_t *, uint16_t *, uint16_t *, uint8_t *, int32_t *);
typedef int32_t (*MergeGlyphData_fn)(uint8_t *, uint8_t *, uint16_t, uint8_t *);
typedef void (*TransformChild_fn)(uint8_t *);
typedef void (*InitializeGlyphData_fn)(uint8_t *, uint8_t *, uint16_t, uint16_t);
typedef uint8_t (*DoScanControl_fn)(uint16_t, uint32_t);
typedef void (*LinkChild_fn)(uint8_t *, uint8_t *);
typedef void (*MxConcat2x2_fn)(int32_t *, int32_t *);
typedef void (*SetSameTransformFlag_fn)(uint8_t *, uint8_t);
typedef int32_t (*ExecuteGlyphPgm_fn)(uint8_t *, uint8_t *, const uint8_t *, const uint8_t *, uint8_t *, void *,
                                      uint16_t *, uint16_t *, uint8_t *);
typedef void (*Release_fn)(AutoReleaseGlyph *);

/* the exports (the leaf ones are register-preserving thunks, see below) */
TT_EXPORT int32_t tt_fsg_ExecuteGlyph(uint8_t *, uint8_t *, uint8_t *, uint32_t, uint8_t *, uint8_t *, uint8_t *,
                                      uint8_t *, void *, uint8_t, uint8_t *, uint32_t *, uint32_t *);
TT_EXPORT int32_t tt_fsg_SimpleInnerGridFit(uint8_t *, uint16_t, int32_t, uint8_t *, uint8_t *, uint8_t *, uint8_t,
                                            void *, uint16_t, uint16_t *, uint16_t *, int16_t, int16_t, int16_t,
                                            uint8_t, uint8_t *, int16_t *, uint16_t, const uint8_t *, uint16_t *,
                                            uint16_t *, uint8_t *);
TT_EXPORT int32_t tt_fsg_CompositeInnerGridFit(uint8_t *, uint8_t *, uint8_t *, uint8_t, void *, uint16_t,
                                               uint16_t *, uint16_t *, int16_t, int16_t, int16_t, uint8_t,
                                               uint8_t *, int16_t *, uint16_t, const uint8_t *, uint16_t *,
                                               uint16_t *, uint8_t *, int32_t *);
TT_EXPORT int32_t tt_fsg_MergeGlyphData(uint8_t *, uint8_t *, uint16_t, uint8_t *);
TT_EXPORT void tt_fsg_TransformChild(uint8_t *);
TT_EXPORT int32_t tt_itrp_ExecuteGlyphPgm(uint8_t *, uint8_t *, const uint8_t *, const uint8_t *, uint8_t *, void *,
                                          uint16_t *, uint16_t *, uint8_t *);
TT_EXPORT void tt_AutoReleaseGlyph_Release(AutoReleaseGlyph *);
void tt_fsg_InitializeGlyphData(uint8_t *, uint8_t *, uint16_t, uint16_t);
uint8_t tt_fsg_DoScanControl(uint16_t, uint32_t);
void tt_fsg_LinkChild(uint8_t *, uint8_t *);
void tt_mth_MxConcat2x2(int32_t *, int32_t *);
void tt_itrp_SetSameTransformFlag(uint8_t *, uint8_t);

#ifdef TT_FSG_DIRECT
#define FSG(name) ((name##_fn)tt_##name)
#else
#define FSG(name) ORIG(name##_fn, RVA_##name)
#endif
#define fsg_ExecuteGlyph_fn ExecuteGlyph_fn
#define fsg_SimpleInnerGridFit_fn SimpleInnerGridFit_fn
#define fsg_CompositeInnerGridFit_fn CompositeInnerGridFit_fn
#define fsg_MergeGlyphData_fn MergeGlyphData_fn
#define fsg_TransformChild_fn TransformChild_fn
#define fsg_InitializeGlyphData_fn InitializeGlyphData_fn
#define fsg_DoScanControl_fn DoScanControl_fn
#define fsg_LinkChild_fn LinkChild_fn
#define mth_MxConcat2x2_fn MxConcat2x2_fn
#define itrp_SetSameTransformFlag_fn SetSameTransformFlag_fn
#define itrp_ExecuteGlyphPgm_fn ExecuteGlyphPgm_fn
#define AutoReleaseGlyph_Release_fn Release_fn

/* the ported interpreter (interp.c) */
TT_EXPORT int32_t tt_itrp_Execute(const uint8_t *twilight, const uint8_t *glyph, const uint8_t *p,
                                  const uint8_t *end, uint8_t *G, void *trace);

/* callees in other groups: the originals */
#define SafeIntOnOverflow()                  ORIG(void (*)(void), 0x1090b0)()
#define fsg_Embold                           ORIG(void (*)(uint8_t *, uint8_t *, uint8_t, uint8_t, uint8_t), 0x64acc)
#define fsg_CheckOutlineOrientation          ORIG(void (*)(uint8_t *), 0x10280)
#define scl_PostTransformGlyph               ORIG(void (*)(uint8_t *, uint8_t *, uint8_t *), 0x0c37c)
#define sfac_GetGlyphLocation                ORIG(int32_t (*)(uint8_t *, uint16_t, uint32_t *, uint32_t *, int32_t *), 0x0f078)
#define sfac_ReadGlyphHorMetrics             ORIG(int32_t (*)(uint8_t *, uint16_t, uint8_t *, uint8_t *), 0x0f5b0)
#define sfac_ReadGlyphBbox                   ORIG(int32_t (*)(uint8_t *, uint16_t, int16_t *), 0x0fa40)
#ifndef IDWR_STANDALONE
#define sfac_ReadOutlineData                 ORIG(int32_t (*)(int32_t *, uint8_t *, int32_t *, int32_t *, uint8_t *, uint8_t *, uint8_t, uint8_t, int16_t, int16_t *, int16_t *, uint8_t *, uint8_t *, uint32_t *, uint32_t *), 0x0eac0)
#else /* the port's integer widths (it narrows them): Apple arm64 packs stack arguments by size */
#define sfac_ReadOutlineData                 ORIG(int32_t (*)(int32_t *, uint8_t *, int32_t *, int32_t *, uint8_t *, uint8_t *, uint32_t, uint32_t, uint32_t, int16_t *, int16_t *, uint8_t *, uint8_t *, uint32_t *, uint32_t *), 0x0eac0)
#endif
#define sfac_ReadComponentData               ORIG(int32_t (*)(uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *), 0xda400)
#define sfac_ReadCompositeInstructions       ORIG(int32_t (*)(uint8_t *, uint8_t *, uint8_t *), 0x02fd8)
#define scl_IncrementChildElement            ORIG(void (*)(uint8_t *, uint8_t *), 0xdceb4)
#define scl_ScaleOldCharPoints               ORIG(void (*)(uint8_t *, uint8_t *), 0xc9ccc)
#define scl_ScaleOldPhantomPoints            ORIG(void (*)(uint8_t *, uint8_t *), 0x127c0)
#define scl_CopyCurrentCharPoints            ORIG(void (*)(uint8_t *), 0xd66ac)
#define scl_CopyCurrentPhantomPoints         ORIG(void (*)(uint8_t *), 0x12a24)
#define scl_RoundCurrentSideBearingPnt       ORIG(void (*)(uint8_t *, uint8_t *, uint16_t), 0x11a20)
#define scl_InitializeChildScaling           ORIG(void (*)(uint8_t *, uint8_t *, uint16_t), 0x1510b4)
#define scl_ScaleBackCurrentCharPoints       ORIG(void (*)(uint8_t *, uint8_t *), 0x151158)
#define scl_ScaleBackCurrentPhantomPoints    ORIG(void (*)(uint8_t *, uint8_t *), 0xf0880)
#define scl_OriginalCharPointsToCurrentFixedFUnits    ORIG(void (*)(uint8_t *), 0xf1d80)
#define scl_OriginalPhantomPointsToCurrentFixedFUnits ORIG(void (*)(uint8_t *), 0x12a80)
#define scl_CalcOrigPhantomPoints            ORIG(void (*)(uint8_t *, int16_t *, int16_t, int16_t, uint16_t, uint16_t, int16_t), 0x124b8)
#define scl_AdjustOldSideBearingPoints       ORIG(void (*)(uint8_t *, uint8_t *), 0x126b8)
#define scl_ScaleFixedCurrentCharPoints      ORIG(void (*)(uint8_t *, uint8_t *), 0xe7d78)
#define scl_ScaleFixedCurrentPhantomPoints   ORIG(void (*)(uint8_t *, uint8_t *), 0x10a27c)
#define scl_CalcComponentOffset              ORIG(void (*)(uint8_t *, int16_t, int16_t, uint8_t, uint8_t, uint8_t, uint8_t *, int32_t, int32_t *, int32_t *), 0x69388)
#define scl_ShiftCurrentCharPoints           ORIG(void (*)(uint8_t *, int32_t, int32_t), 0x692cc)
#define scl_UpdateParentElement              ORIG(int32_t (*)(uint8_t *, uint8_t *, uint8_t *), 0x69550)
#define mth_IntelMul                         ORIG(void (*)(int32_t, int32_t *, int32_t *, uint8_t *, int32_t, int32_t), 0x69624)
#define DWRITE_FixMul                        ORIG(int32_t (*)(int32_t, int32_t), 0x67b70)

/* sfac_ClientRec: client id, GetSfntFragment, ReleaseSfntFrag, table directory {offset, length}
 * per table at 0x20, outline variation callback at 0xf8 */
typedef const uint8_t *(*get_fragment_fn)(void *id, int32_t offset, int32_t length, void **ctx);
typedef void (*release_fragment_fn)(void *ctx);
typedef void (*outline_variation_fn)(void *id, uint16_t glyph, int16_t *ep, uint64_t contours, int32_t *x,
                                     int32_t *y, uint32_t n);
#define CL_ID(c)        AT(c, 0x00, void *)
#define CL_GET(c)       AT(c, 0x08, get_fragment_fn)
#define CL_RELEASE(c)   AT(c, 0x10, release_fragment_fn)
#define CL_VARIATION(c) AT(c, 0xf8, outline_variation_fn)

#define E_CFLAGS(e) AT(e, 0x58, uint8_t *)  /* per contour flags (bit 0: orientation) */

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* last point of the outline: ep[nc - 1], nc sign extended as the original reads it */
static inline uint16_t last_point(uint8_t *e) { return (uint16_t)E_EP(e)[(int64_t)E_NC(e) - 1]; }

/* ---------------------------------------------------------------------------------------- */
/* Register-preserving export thunks. MSVC's whole-program optimisation knows which volatile
 * registers these leaf functions leave alone, and the callers rely on it: fsg_ExecuteGlyph
 * keeps the child record in RDX across fsg_LinkChild, fsg_CompositeInnerGridFit keeps G in
 * RCX across itrp_SetSameTransformFlag. A swapped-in port therefore saves RCX, RDX, R8-R11
 * and XMM0-XMM5 around the C body.
 *
 * Standalone mode (-DIDWR_STANDALONE): every caller is a C port, so there is no thunk; the
 * defines rename each body to its export, the plain C function on any ABI. */
#ifdef IDWR_STANDALONE
#define tt_fsg_InitializeGlyphData_body   tt_fsg_InitializeGlyphData
#define tt_fsg_DoScanControl_body         tt_fsg_DoScanControl
#define tt_fsg_LinkChild_body             tt_fsg_LinkChild
#define tt_mth_MxConcat2x2_body           tt_mth_MxConcat2x2
#define tt_itrp_SetSameTransformFlag_body tt_itrp_SetSameTransformFlag
#define PRESERVING_THUNK(name) struct idwr_no_thunk_##name
#else
#define PRESERVING_THUNK(name)                                                                  \
    __asm__(".text\n.globl " #name "\n.def " #name "; .scl 2; .type 32; .endef\n"              \
            ".seh_proc " #name "\n" #name ":\n"                                                \
            "pushq %rcx\n.seh_pushreg %rcx\npushq %rdx\n.seh_pushreg %rdx\n"                   \
            "pushq %r8\n.seh_pushreg %r8\npushq %r9\n.seh_pushreg %r9\n"                       \
            "pushq %r10\n.seh_pushreg %r10\npushq %r11\n.seh_pushreg %r11\n"                   \
            "subq $0x88, %rsp\n.seh_stackalloc 0x88\n.seh_endprologue\n"                       \
            "movdqu %xmm0, 0x20(%rsp)\nmovdqu %xmm1, 0x30(%rsp)\nmovdqu %xmm2, 0x40(%rsp)\n"   \
            "movdqu %xmm3, 0x50(%rsp)\nmovdqu %xmm4, 0x60(%rsp)\nmovdqu %xmm5, 0x70(%rsp)\n"   \
            "call " #name "_body\n"                                                            \
            "movdqu 0x20(%rsp), %xmm0\nmovdqu 0x30(%rsp), %xmm1\nmovdqu 0x40(%rsp), %xmm2\n"   \
            "movdqu 0x50(%rsp), %xmm3\nmovdqu 0x60(%rsp), %xmm4\nmovdqu 0x70(%rsp), %xmm5\n"   \
            "addq $0x88, %rsp\npopq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n" \
            "ret\n.seh_endproc\n"                                                              \
            ".section .drectve,\"yn\"\n.ascii \" -export:" #name "\"\n.text\n")
#endif

PRESERVING_THUNK(tt_fsg_InitializeGlyphData);
PRESERVING_THUNK(tt_fsg_DoScanControl);
PRESERVING_THUNK(tt_fsg_LinkChild);
PRESERVING_THUNK(tt_mth_MxConcat2x2);
PRESERVING_THUNK(tt_itrp_SetSameTransformFlag);

/* ---------------------------------------------------------------------------------------- */
/* fsg_InitializeGlyphData 0x1800d12f8: a fresh record for glyph `glyph` at depth `depth`,
 * using the work space element of that depth */
static const int32_t identity3x3[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x10000};

__attribute__((used)) void tt_fsg_InitializeGlyphData_body(uint8_t *gd, uint8_t *ws, uint16_t glyph, uint16_t depth) {
    AT(gd, 0x98, uint16_t) = 0xffff;
    AT(gd, 0x3c, uint16_t) = glyph;
    memcpy(gd + 0x60, identity3x3, 0x24);
    AT(gd, 0x00, void *) = NULL;
    AT(gd, 0x08, void *) = NULL;
    AT(gd, 0x10, void *) = NULL;
    AT(gd, 0x38, int32_t) = 3;
    AT(gd, 0x20, void *) = NULL;
    AT(gd, 0x30, void *) = NULL;
    AT(gd, 0x28, void *) = NULL;
    AT(gd, 0x18, uint32_t) = 0;
    AT(gd, 0x4e, uint16_t) = depth;
    AT(gd, 0x85, uint16_t) = 0;
    AT(gd, 0x84, uint8_t) = 0;
    AT(gd, 0x3e, uint32_t) = 0x7fff7fff;
    AT(gd, 0x42, uint32_t) = 0x80008000;
    AT(gd, 0x9a, uint16_t) = 0;
    AT(gd, 0xa0, void *) = NULL;
    AT(gd, 0x46, uint16_t) = 0;
    AT(gd, 0x4a, uint16_t) = 0;
    AT(gd, 0x50, int32_t) = 2;
    AT(gd, 0x54, uint8_t) = 0;
    AT(gd, 0x56, uint64_t) = 0;
    AT(gd, 0x88, uint64_t) = 0;
    AT(gd, 0x90, uint64_t) = 0;
    uint8_t *e = AT(ws, 0x10, uint8_t *) + (uint64_t)depth * 0x70;
    AT(gd, 0xa8, uint8_t *) = e;
    E_NC(e) = 0;
    memcpy(gd + 0xb0, identity3x3, 0x24);
    AT(gd, 0xd4, uint8_t) = 1;
    AT(gd, 0xd8, uint64_t) = 0;
    AT(gd, 0xe0, uint64_t) = 0;
    AT(gd, 0xe8, uint64_t) = 0;
    AT(gd, 0xf0, uint64_t) = 0;
}

/* fsg_DoScanControl 0x1800d5e04: does the glyph's SCANCTRL word turn dropout control on?
 * Bit 8: on at or below ppem (low byte, 0xff = always); bits 9 and 10: on when the transform
 * flags (bits 10, 12: rotated, stretched) say so. */
static inline uint8_t scan_control(uint16_t sc, uint32_t flags) {
    if ((sc & 0x100) && (uint8_t)flags <= (uint8_t)sc) return 1;
    if ((sc & 0x100) && (uint8_t)sc == 0xff) return 1;
    if ((sc & 0x200) && (flags & 0x400)) return 1;
    if ((sc & 0x400) && (flags & 0x1000)) return 1;
    return 0;
}
__attribute__((used)) uint8_t tt_fsg_DoScanControl_body(uint16_t sc, uint32_t flags) { return scan_control(sc, flags); }

/* fsg_LinkChild 0x1800deb88: append `child` to the children of `parent` (the last child's
 * next pointer leads back to the parent) and give it the parent's transform */
__attribute__((used)) void tt_fsg_LinkChild_body(uint8_t *parent, uint8_t *child) {
    uint8_t *c = AT(parent, 0x08, uint8_t *);
    if (!c) {
        AT(parent, 0x08, uint8_t *) = child;
    } else {
        for (uint8_t *q = AT(c, 0x00, uint8_t *); q != parent; q = AT(q, 0x00, uint8_t *)) c = q;
        AT(c, 0x00, uint8_t *) = child;
    }
    AT(child, 0x00, uint8_t *) = parent;
    AT(child, 0x10, uint8_t *) = parent;
    memcpy(child + 0xb0, parent + 0xb0, 0x24);
    AT(child, 0xd4, uint8_t) = AT(parent, 0xd4, uint8_t);
}

/* mth_MxConcat2x2 0x1800e743c: the 2x2 part of b = a * b, 16.16 (DWRITE_FixMul) */
__attribute__((used)) void tt_mth_MxConcat2x2_body(int32_t *a, int32_t *b) {
    int32_t a00 = a[0], a01 = a[1], a10 = a[3], a11 = a[4];
    int32_t b10 = b[3], b11 = b[4], b12 = b[5];
    b[5] = (int32_t)((uint32_t)DWRITE_FixMul(a11, b12) + (uint32_t)DWRITE_FixMul(a10, b[2]));
    b[4] = (int32_t)((uint32_t)DWRITE_FixMul(a11, b11) + (uint32_t)DWRITE_FixMul(a10, b[1]));
    b[3] = (int32_t)((uint32_t)DWRITE_FixMul(a11, b10) + (uint32_t)DWRITE_FixMul(a10, b[0]));
    b[2] = (int32_t)((uint32_t)DWRITE_FixMul(a01, b12) + (uint32_t)DWRITE_FixMul(a00, b[2]));
    b[1] = (int32_t)((uint32_t)DWRITE_FixMul(a01, b11) + (uint32_t)DWRITE_FixMul(a00, b[1]));
    b[0] = (int32_t)((uint32_t)DWRITE_FixMul(a01, b10) + (uint32_t)DWRITE_FixMul(a00, b[0]));
}

/* itrp_SetSameTransformFlag 0x180012724 */
__attribute__((used)) void tt_itrp_SetSameTransformFlag_body(uint8_t *G, uint8_t same) {
    AT(G, 0x195, uint8_t) = same;
    AT(G, 0x196, uint8_t) = (AT(G, 0x171, uint8_t) == 0 && same) ? 0 : 1;
}

/* AutoReleaseGlyph::Release 0x1800d14f0: give the glyph's sfnt fragment back */
TT_EXPORT void tt_AutoReleaseGlyph_Release(AutoReleaseGlyph *ar) {
    uint8_t *h = ar->handle;
    if (!h) return;
    if (AT(h, 0x18, void *)) {
        CL_RELEASE(ar->client)(AT(h, 0x10, void *));
        AT(h, 0x10, void *) = NULL;
        AT(h, 0x18, void *) = NULL;
        AT(h, 0x08, void *) = NULL;
        AT(h, 0x00, uint32_t) = 0;
    }
    ar->handle = NULL;
    ar->client = NULL;
}

/* fsg_TransformChild 0x18006925c: apply the component matrix to the child's points (all of
 * them, phantoms included) unless it is the identity */
TT_EXPORT void tt_fsg_TransformChild(uint8_t *gd) {
    int32_t *m = (int32_t *)(gd + 0x60);
    if (m[0] == 0x10000 && m[0] == m[4] && m[1] == 0 && m[3] == 0) return;
    uint8_t *e = AT(gd, 0xa8, uint8_t *);
    mth_IntelMul((uint16_t)(last_point(e) + 9), E_X(e), E_Y(e), gd + 0x60, 0x10000, 0x10000);
}

/* itrp_ExecuteGlyphPgm 0x18001274c */
TT_EXPORT int32_t tt_itrp_ExecuteGlyphPgm(uint8_t *twilight, uint8_t *elem, const uint8_t *p, const uint8_t *end,
                                          uint8_t *G, void *trace, uint16_t *scan_ctl, uint16_t *scan_in,
                                          uint8_t *changed) {
    int32_t err = 0;
    AT(G, 0x16a, uint16_t) = 0x200;
    if (!(AT(G, 0x88, uint8_t) & 1)) err = tt_itrp_Execute(twilight, elem, p, end, G, trace);
    *changed = AT(G, 0x84, int32_t) != AT(G, 0x44, int32_t);
    *scan_in = AT(G, 0x84, uint16_t);
    *scan_ctl = AT(G, 0x86, uint16_t);
    return err;
}

/* fsg_MergeGlyphData 0x180068f7c: place a finished component in its parent: transform it,
 * offset it (by x/y, or by matching anchor points), copy its points into the parent's zone */
TT_EXPORT int32_t tt_fsg_MergeGlyphData(uint8_t *G, uint8_t *gd, uint16_t ppem, uint8_t *maxp) {
    uint8_t *parent = AT(gd, 0x10, uint8_t *);
    uint8_t *e = AT(gd, 0xa8, uint8_t *);
    int32_t xoff = 0, yoff = 0;
    uint8_t *pe = AT(parent, 0xa8, uint8_t *);
    FSG(fsg_TransformChild)(gd);
    if (AT(gd, 0xd4, uint8_t) == 0 && AT(parent, 0xd4, uint8_t) != 0) {
        AT(G, 0x195, uint8_t) = 1;
        AT(G, 0x196, uint8_t) = AT(G, 0x171, uint8_t) != 0;
        scl_ScaleFixedCurrentCharPoints(e, G);
        scl_ScaleFixedCurrentPhantomPoints(e, G);
        AT(gd, 0xd4, uint8_t) = 1;
    }
    int32_t parity;  /* RotationParity of the parent's accumulated transform */
    if (AT(parent, 0xb4, int32_t) == 0 && AT(parent, 0xbc, int32_t) == 0) parity = 0;
    else if (AT(parent, 0xb0, int32_t) == 0 && AT(parent, 0xc0, int32_t) == 0) parity = 1;
    else parity = 2;
    int32_t dx, dy;
    if (AT(gd, 0x50, int32_t) == 1) {
        if (AT(parent, 0xd4, uint8_t) == 0) scl_InitializeChildScaling(G, parent + 0xb0, ppem);
        scl_CalcComponentOffset(G, AT(gd, 0x56, int16_t), AT(gd, 0x58, int16_t), AT(gd, 0x54, uint8_t),
                                AT(gd, 0xd4, uint8_t), AT(gd, 0x86, uint8_t), gd + 0x60, parity, &xoff, &yoff);
        dx = xoff;
        dy = yoff;
    } else {
        if (AT(gd, 0x50, int32_t) != 0) OutputDebugStringA("Bad Multiplexing Indicator");
        if (E_NC(pe) == 0) return 0x1401;
        uint32_t p1 = AT(gd, 0x5a, uint16_t), p2 = AT(gd, 0x5c, uint16_t);
        if ((int32_t)p1 > (int32_t)E_EP(pe)[(int64_t)E_NC(pe) - 1] + 8) return 0x1401;
        if ((int32_t)p2 > (int32_t)E_EP(e)[(int64_t)E_NC(e) - 1] + 8) return 0x1401;
        dx = (int32_t)((uint32_t)E_X(pe)[p1] - (uint32_t)E_X(e)[p2]);
        dy = (int32_t)((uint32_t)E_Y(pe)[p1] - (uint32_t)E_Y(e)[p2]);
    }
    scl_ShiftCurrentCharPoints(e, dx, dy);
    if (AT(gd, 0x85, uint8_t)) {  /* USE_MY_METRICS: the parent takes this component's phantoms */
        AT(parent, 0x84, uint8_t) = 1;
        uint16_t p = (uint16_t)(last_point(e) + 1), q = (uint16_t)(p + 1);
        AT(parent, 0x88, int32_t) = E_X(e)[p];
        AT(parent, 0x8c, int32_t) = E_Y(e)[p];
        AT(parent, 0x90, int32_t) = E_X(e)[q];
        AT(parent, 0x94, int32_t) = E_Y(e)[q];
    }
    uint16_t sc = AT(gd, 0x98, uint16_t), psc = AT(parent, 0x98, uint16_t);
    if (psc != 0xffff) sc = (uint16_t)(((sc & 3) | 4) & psc);
    AT(parent, 0x98, uint16_t) = sc;
    int32_t err = scl_UpdateParentElement(e, pe, maxp);
    E_NC(e) = 0;
    return err;
}

/* fsg_SimpleInnerGridFit 0x18000fc0c: phantom points, variation, scaling, rounding of the
 * side bearing and the glyph program for a simple glyph */
TT_EXPORT int32_t tt_fsg_SimpleInnerGridFit(uint8_t *client, uint16_t glyph, int32_t npts, uint8_t *G,
                                            uint8_t *twilight, uint8_t *e, uint8_t hint, void *trace,
                                            uint16_t ppem, uint16_t *aw, uint16_t *ah, int16_t lsb, int16_t tsb,
                                            int16_t vorig, uint8_t same, uint8_t *matrix, int16_t *bbox,
                                            uint16_t inslen, const uint8_t *ins, uint16_t *scan_ctl,
                                            uint16_t *scan_in, uint8_t *changed) {
    uint8_t keep = AT(G, 0x195, uint8_t);
    memcpy(G + 0x78, G + 0x38, 0x40);
    AT(G, 0x171, uint8_t) = 0;
    AT(G, 0x196, uint8_t) = keep == 0;
    *scan_ctl = AT(G, 0x86, uint16_t);
    *scan_in = AT(G, 0x84, uint16_t);
    *changed = 0;
    uint32_t awv = *aw, ahv = *ah;
    int32_t *oox = E_OOX(e), *ooy = E_OOY(e);
    uint16_t lp = last_point(e);
    memset(&ooy[(uint16_t)(lp + 1)], 0, 32);
    memset(&oox[(uint16_t)(lp + 1)], 0, 32);
    /* the 8 phantom points: horizontal origin and advance, vertical origin and advance, then
     * the same pair again (left and right side bearing points) */
    int32_t x0 = (int32_t)bbox[0] - (int32_t)lsb;
    oox[(uint16_t)(lp + 1)] = x0;
    oox[(uint16_t)(lp + 2)] = (int32_t)(awv + (uint32_t)x0);
    oox[(uint16_t)(lp + 5)] = x0;
    oox[(uint16_t)(lp + 6)] = bbox[0];
    int32_t y0 = (int32_t)tsb + (int32_t)bbox[3];
    ooy[(uint16_t)(lp + 3)] = y0;
    ooy[(uint16_t)(lp + 4)] = (int32_t)((uint32_t)y0 - ahv);
    ooy[(uint16_t)(lp + 7)] = y0;
    ooy[(uint16_t)(lp + 8)] = bbox[3];
    for (int i = 0; i < 2; i++) oox[(uint16_t)(i + 3 + lp)] = vorig;
    for (int i = 0; i < 2; i++) oox[(uint16_t)(i + 7 + lp)] = vorig;
    /* the variation callback: whole outline, or only the 4 metric phantoms of an empty glyph */
    if (npts == 0)
        CL_VARIATION(client)(CL_ID(client), glyph, E_EP(e), (uint64_t)(int64_t)E_NC(e),
                             &oox[(uint16_t)(lp + 1)], &ooy[(uint16_t)(lp + 1)], 4);
    else
        CL_VARIATION(client)(CL_ID(client), glyph, E_EP(e), (uint16_t)E_NC(e), oox, ooy, (uint32_t)npts + 4);
    lp = last_point(e);
    *aw = (uint16_t)(oox[(uint16_t)(lp + 2)] - oox[(uint16_t)(lp + 1)]);
    *ah = (uint16_t)(ooy[(uint16_t)(lp + 3)] - ooy[(uint16_t)(lp + 4)]);
    if ((AT(G, 0x88, uint8_t) & 1) || !hint) {
        if (!same) {
            scl_OriginalCharPointsToCurrentFixedFUnits(e);
            scl_OriginalPhantomPointsToCurrentFixedFUnits(e);
        } else {
            scl_ScaleOldCharPoints(e, G);
            scl_ScaleOldPhantomPoints(e, G);
            scl_CopyCurrentCharPoints(e);
            scl_CopyCurrentPhantomPoints(e);
        }
        return 0;
    }
    AT(G, 0x195, uint8_t) = same;
    if (AT(G, 0x171, uint8_t) == 0 && same) {
        AT(G, 0x196, uint8_t) = 0;
    } else {
        AT(G, 0x196, uint8_t) = 1;
        if (!same) scl_InitializeChildScaling(G, matrix, ppem);
    }
    scl_ScaleOldCharPoints(e, G);
    scl_ScaleOldPhantomPoints(e, G);
    /* round the left side bearing point to the grid (to 1/16 pixel when the low bits of
     * G + 0x1c0 are 1: ClearType without bit 4) and move the outline with it, then the 8
     * phantom points by the same amount */
    int32_t *ox = E_OX(e);
    uint16_t p = (uint16_t)(last_point(e) + 1);
    uint32_t v = (uint32_t)ox[p];
    uint32_t r = (AT(G, 0x1c0, uint8_t) & 5) == 1 ? ((v + 2) & 0xfffffffcu) : ((v + 0x20) & 0xffffffc0u);
    uint32_t d = r - v;
    if (d) for (uint32_t i = 0; i < p; i++) ox[i] = (int32_t)((uint32_t)ox[i] + d);
    p = (uint16_t)(last_point(e) + 1);
    v = (uint32_t)ox[p];
    r = (AT(G, 0x1c0, uint8_t) & 5) == 1 ? ((v + 2) & 0xfffffffcu) : ((v + 0x20) & 0xffffffc0u);
    d = r - v;
    if (d) for (uint32_t j = 0; j < 8; j++) ox[p + j] = (int32_t)((uint32_t)ox[p + j] + d);
    scl_CopyCurrentCharPoints(e);
    scl_CopyCurrentPhantomPoints(e);
    scl_RoundCurrentSideBearingPnt(e, G, ppem);
    if (inslen) {
        memset(E_F(e), 0, (uint16_t)(last_point(e) + 9));
        AT(G, 0x16a, uint16_t) = 0x200;
        int32_t err = 0;
        if (!(AT(G, 0x88, uint8_t) & 1)) err = tt_itrp_Execute(twilight, e, ins, ins + inslen, G, trace);
        *changed = AT(G, 0x84, int32_t) != AT(G, 0x44, int32_t);
        *scan_in = AT(G, 0x84, uint16_t);
        *scan_ctl = AT(G, 0x86, uint16_t);
        if (err) return err;
    }
    if (!same) {
        scl_ScaleBackCurrentCharPoints(e, G);
        scl_ScaleBackCurrentPhantomPoints(e, G);
    }
    return 0;
}

/* fsg_CompositeInnerGridFit 0x1800120ac: phantom points and the composite's own program,
 * once all its components are merged */
TT_EXPORT int32_t tt_fsg_CompositeInnerGridFit(uint8_t *G, uint8_t *twilight, uint8_t *e, uint8_t hint, void *trace,
                                               uint16_t ppem, uint16_t *aw, uint16_t *ah, int16_t lsb, int16_t tsb,
                                               int16_t vorig, uint8_t same, uint8_t *matrix, int16_t *bbox,
                                               uint16_t inslen, const uint8_t *ins, uint16_t *scan_ctl,
                                               uint16_t *scan_in, uint8_t *changed, int32_t *delta) {
    memcpy(G + 0x78, G + 0x38, 0x40);
    AT(G, 0x171, uint8_t) = 1;
    AT(G, 0x196, uint8_t) = 1;
    *scan_ctl = AT(G, 0x86, uint16_t);
    *scan_in = AT(G, 0x84, uint16_t);
    *changed = 0;
    scl_CalcOrigPhantomPoints(e, bbox, lsb, tsb, *aw, *ah, vorig);
    int32_t *oox = E_OOX(e), *ooy = E_OOY(e);
    uint16_t lp = last_point(e);
    /* phantom deltas the variation callback gave to the composite */
    for (int k = 0; k < 4; k++) oox[(uint16_t)(lp + 1 + k)] = (int32_t)((uint32_t)oox[(uint16_t)(lp + 1 + k)] + (uint32_t)delta[2 * k]);
    for (int k = 0; k < 4; k++) ooy[(uint16_t)(lp + 1 + k)] = (int32_t)((uint32_t)ooy[(uint16_t)(lp + 1 + k)] + (uint32_t)delta[2 * k + 1]);
    *aw = (uint16_t)(oox[(uint16_t)(lp + 2)] - oox[(uint16_t)(lp + 1)]);
    *ah = (uint16_t)(ooy[(uint16_t)(lp + 3)] - ooy[(uint16_t)(lp + 4)]);
    memcpy(E_OX(e), E_X(e), (size_t)(uint16_t)(lp + 1) << 2);
    memcpy(E_OY(e), E_Y(e), (size_t)(uint16_t)(lp + 1) << 2);
    if ((AT(G, 0x88, uint8_t) & 1) || !hint) {
        if (same) {
            scl_ScaleOldPhantomPoints(e, G);
            scl_CopyCurrentPhantomPoints(e);
        } else {
            scl_OriginalPhantomPointsToCurrentFixedFUnits(e);
        }
        return 0;
    }
    FSG(itrp_SetSameTransformFlag)(G, same);
    if (!same) {
        scl_InitializeChildScaling(G, matrix, ppem);
        scl_ScaleFixedCurrentCharPoints(e, G);
    }
    scl_ScaleOldPhantomPoints(e, G);
    scl_AdjustOldSideBearingPoints(e, G);
    scl_CopyCurrentPhantomPoints(e);
    scl_RoundCurrentSideBearingPnt(e, G, ppem);
    if (inslen) {
        memset(E_F(e), 0, (uint16_t)(last_point(e) + 9));
        int32_t err = FSG(itrp_ExecuteGlyphPgm)(twilight, e, ins, ins + inslen, G, trace, scan_ctl, scan_in, changed);
        if (err) return err;
    }
    if (!same) {
        scl_ScaleBackCurrentCharPoints(e, G);
        scl_ScaleBackCurrentPhantomPoints(e, G);
    }
    return 0;
}

/* the products of the determinant test in fsg_ExecuteGlyph: (a*b + 0x8000 - (a*b < 0)) >> 16,
 * saturated to int32 */
static int32_t det_mul(int32_t a, int32_t b) {
    int64_t p = (int64_t)a * b;
    int64_t q = (p + 0x8000 + (p >> 63)) >> 16;
    if (q > 0x7fffffff) return 0x7fffffff;
    if (q < -0x7fffffff - 1) return -0x7fffffff - 1;
    return (int32_t)q;
}

static inline uint32_t wrap_abs(int32_t v) { return v < 0 ? 0u - (uint32_t)v : (uint32_t)v; }

/* fsg_ExecuteGlyph 0x18000dbdc: one step of the state machine on record `gd` */
TT_EXPORT int32_t tt_fsg_ExecuteGlyph(uint8_t *client, uint8_t *maxp, uint8_t *xform, uint32_t nslots, uint8_t *G,
                                      uint8_t *gd, uint8_t *ws, uint8_t *twilight, void *trace, uint8_t hint,
                                      uint8_t *has_outline, uint32_t *a12, uint32_t *a13) {
    uint8_t last = 0, instr = 0, matrix = 0;
    uint16_t scan_ctl = 0, scan_in = 0;
    *has_outline = 0;
    AutoReleaseGlyph ar = {NULL, NULL};
    uint8_t **pelem = &AT(gd, 0xa8, uint8_t *);
    uint8_t *handle = gd + 0x18;  /* sfac_GHandle */
    uint8_t *rel_client;
    int32_t err;

    if (AT(gd, 0x38, int32_t) != 3) {
        FSG(AutoReleaseGlyph_Release)(&ar);
        rel_client = client;
        ar.client = client;
        ar.handle = handle;
        goto dispatch;
    }

    /* ---- state 3: a new record ---- */
    if (AT(gd, 0x10, uint8_t *)) scl_IncrementChildElement(*pelem, AT(AT(gd, 0x10, uint8_t *), 0xa8, uint8_t *));
    else memcpy(gd + 0xb0, xform + 4, 0x24);
    {
        uint8_t *e = *pelem;
        uint32_t off = 0, len = 0;
        int32_t table = 0;
        AT(gd, 0x28, void *) = NULL;
        AT(gd, 0x20, void *) = NULL;
        AT(gd, 0x30, void *) = NULL;
        AT(gd, 0x18, uint32_t) = 0;
        err = sfac_GetGlyphLocation(client, AT(gd, 0x3c, uint16_t), &off, &len, &table);
        if (err) goto located;
        if (len == 0) {  /* empty glyph */
            E_NC(e) = 1;
            AT(gd, 0x3e, uint64_t) = 0;
            *has_outline = 0;
            last = 0;
            err = 0;
            goto located;
        }
        if (len < 10) { err = 0x140d; goto located; }
        AT(gd, 0x28, void *) = NULL;
        AT(gd, 0x20, void *) = NULL;
        if (AT(client, 0x24 + 8 * (int64_t)table, uint32_t) == 0) { err = 0x1409; goto located; }
        if (len > 0x7fffffff) SafeIntOnOverflow();
        uint32_t at = AT(client, 0x20 + 8 * (int64_t)table, uint32_t) + off;
        if (at < off || at > 0x7fffffff) SafeIntOnOverflow();
        const uint8_t *d = CL_GET(client)(CL_ID(client), (int32_t)at, (int32_t)len, &AT(gd, 0x28, void *));
        AT(gd, 0x20, const uint8_t *) = d;
        if (!d) { err = 0x1408; goto located; }
        AT(gd, 0x18, uint32_t) = len;
        int16_t nc = (int16_t)be16(d);
        E_NC(e) = nc;
        if (nc == -1) {  /* composite */
            last = 1;
            E_NC(e) = 0;
            *has_outline = 0;
        } else if (nc < -1) {
            err = 0x1407;
            goto located;
        } else {
            last = 0;
            *has_outline = 1;
        }
        int16_t xmin = (int16_t)be16(d + 2), ymin = (int16_t)be16(d + 4);
        int16_t xmax = (int16_t)be16(d + 6), ymax = (int16_t)be16(d + 8);
        AT(gd, 0x3e, int16_t) = xmin;
        AT(gd, 0x40, int16_t) = ymin;
        AT(gd, 0x42, int16_t) = xmax;
        AT(gd, 0x44, int16_t) = ymax;
        if (xmin > xmax || ymin > ymax) { err = 0x1400; goto located; }
        const uint8_t *body = d + 10;
        AT(gd, 0x30, const uint8_t *) = body;
        uintptr_t base = (uintptr_t)AT(gd, 0x20, const uint8_t *);
        uintptr_t stop = base + AT(gd, 0x18, uint32_t);
        if (stop < base) SafeIntOnOverflow();
        err = (uintptr_t)body > stop ? 0x1400 : 0;
    }
located:
    rel_client = client;
    ar.client = client;
    ar.handle = handle;
    if (err) goto fail;
    {
        uint8_t vmode = AT(G, 0x1bd, uint8_t);
        uint16_t g = AT(gd, 0x3c, uint16_t);
        err = sfac_ReadGlyphHorMetrics(client, g, gd + 0x46, gd + 0x4a);
        if (err) goto fail;
        int16_t bb[4] = {0, 0, 0, 0};
        void *ctx = NULL, *vctx = NULL;
        const uint8_t *vm = NULL;
        uint16_t nvm = AT(client, 0xda, uint16_t);
        uint32_t size = g < nvm ? (uint32_t)g * 4 + 4 : ((uint32_t)nvm + g) * 2 + 2;
        int16_t tsb;
        if (AT(client, 0xdc, uint8_t) && AT(client, 0xc4, uint32_t) != 0) {  /* vmtx */
            uint32_t at = AT(client, 0xc0, uint32_t);
            if (at > 0x7fffffff) SafeIntOnOverflow();
            vm = CL_GET(client)(CL_ID(client), (int32_t)at, (int32_t)size, &ctx);
            if (!vm) OutputDebugStringA("sfntaccs.c, sfac_GetDataPtr, optional table ignored due to bogus size");
            vctx = ctx;
        }
        if (AT(client, 0xdc, uint8_t) && vm) {
            if (g < nvm) {
                AT(gd, 0x48, uint16_t) = be16(vm + (uint32_t)g * 4);
                tsb = (int16_t)be16(vm + (uint64_t)g * 4 + 2);
            } else {
                const uint8_t *tail = vm + (uint32_t)(nvm * 4);
                AT(gd, 0x48, uint16_t) = be16(tail - 4);
                tsb = (int16_t)be16(tail + 2 * ((int64_t)(int32_t)g - nvm));
            }
        } else {
            /* no vertical metrics: ascender - descender, top side bearing from the bbox */
            err = sfac_ReadGlyphBbox(client, AT(client, 0xe8, uint16_t), bb);
            if (err) {
                if (vctx) CL_RELEASE(client)(vctx);
                goto vorigin;
            }
            AT(gd, 0x48, int16_t) = (int16_t)(AT(client, 0xf0, int16_t) - AT(client, 0xf2, int16_t));
            tsb = (int16_t)(AT(client, 0xf0, int16_t) - bb[3]);
        }
        AT(gd, 0x4c, int16_t) = tsb;
        if (vctx) CL_RELEASE(client)(vctx);
        err = 0;
    vorigin:;
        uint16_t vo;
        if (vmode == 1) {
            int32_t f2 = AT(client, 0xf2, int16_t);
            int32_t half = ((int32_t)AT(client, 0xf0, int16_t) - f2 - (int32_t)AT(gd, 0x46, uint16_t)) / 2;
            vo = (uint16_t)(-(uint16_t)f2 - (uint16_t)half);
        } else if (vmode == 2) {
            vo = 0;
        } else {
            vo = AT(gd, 0x46, uint16_t) >> 1;
        }
        AT(gd, 0xd6, uint16_t) = vo;
        if (err) goto fail;
        AT(gd, 0x38, int32_t) = last ? 1 : 0;
    }

dispatch:;
    int32_t state = AT(gd, 0x38, int32_t);
    if (state == 0) {
        /* ---- simple glyph ---- */
        int32_t npts = 0;
        uint8_t *e = *pelem;
        err = sfac_ReadOutlineData(&npts, E_ONCURVE(e), E_OOY(e), E_OOX(e), handle, maxp, *has_outline,
                                   AT(gd, 0x10, void *) != NULL, E_NC(e), E_SP(e), E_EP(e), gd + 0x9a, gd + 0xa0,
                                   a12, a13);
        if (err) goto fail;
        fsg_CheckOutlineOrientation(*pelem);
        err = FSG(fsg_SimpleInnerGridFit)(client, AT(gd, 0x3c, uint16_t), npts, G, twilight, *pelem, hint, trace,
                                          AT(xform, 0, uint16_t), &AT(gd, 0x46, uint16_t), &AT(gd, 0x48, uint16_t),
                                          AT(gd, 0x4a, int16_t), AT(gd, 0x4c, int16_t), AT(gd, 0xd6, int16_t),
                                          AT(gd, 0xd4, uint8_t), gd + 0xb0, &AT(gd, 0x3e, int16_t),
                                          AT(gd, 0x9a, uint16_t), AT(gd, 0xa0, const uint8_t *), &scan_ctl,
                                          &scan_in, &instr);
        if (err) goto fail;
        /* fsg_DoScanControl, inlined here */
        AT(gd, 0x98, uint16_t) = scan_control(scan_in, AT(xform, 0x30, uint32_t)) ? scan_ctl : 2;
        /* a mirroring component matrix (negative determinant) flips the contour directions */
        int32_t ad = det_mul(AT(gd, 0x70, int32_t), AT(gd, 0x60, int32_t));
        int32_t bc = det_mul(AT(gd, 0x6c, int32_t), AT(gd, 0x64, int32_t));
        if ((int32_t)((uint32_t)ad - (uint32_t)bc) < 0) {
            for (uint16_t i = 0; (int32_t)i < (int32_t)E_NC(*pelem); i++) E_CFLAGS(*pelem)[i] ^= 1;
        }
        if (AT(gd, 0x10, void *)) {
            err = FSG(fsg_MergeGlyphData)(G, gd, AT(xform, 0, uint16_t), maxp);
            if (err) goto fail;
        }
        AT(gd, 0xa0, void *) = NULL;
        AT(gd, 0x9a, uint16_t) = 0;
        goto release_ok;
    }
    if (state == 2) {
        /* ---- composite, after its components ---- */
        err = FSG(fsg_CompositeInnerGridFit)(G, twilight, *pelem, hint, trace, AT(xform, 0, uint16_t),
                                             &AT(gd, 0x46, uint16_t), &AT(gd, 0x48, uint16_t), AT(gd, 0x4a, int16_t),
                                             AT(gd, 0x4c, int16_t), AT(gd, 0xd6, int16_t), AT(gd, 0xd4, uint8_t),
                                             gd + 0xb0, &AT(gd, 0x3e, int16_t), AT(gd, 0x9a, uint16_t),
                                             AT(gd, 0xa0, const uint8_t *), &scan_ctl, &scan_in, &instr,
                                             (int32_t *)(gd + 0xd8));
        if (err) goto fail;
        if (AT(gd, 0x84, uint8_t)) {  /* phantoms of the USE_MY_METRICS component */
            uint8_t *e = *pelem;
            uint16_t p = (uint16_t)(last_point(e) + 1), q = (uint16_t)(p + 1);
            E_X(e)[p] = AT(gd, 0x88, int32_t);
            E_Y(e)[p] = AT(gd, 0x8c, int32_t);
            E_X(e)[q] = AT(gd, 0x90, int32_t);
            E_Y(e)[q] = AT(gd, 0x94, int32_t);
        }
        if (instr) AT(gd, 0x98, uint16_t) = FSG(fsg_DoScanControl)(scan_in, AT(xform, 0x30, uint32_t)) ? scan_ctl : 2;
        if (AT(gd, 0x10, void *)) {
            err = FSG(fsg_MergeGlyphData)(G, gd, AT(xform, 0, uint16_t), maxp);
            if (err) goto fail;
        }
        AT(gd, 0xa0, void *) = NULL;
        AT(gd, 0x9a, uint16_t) = 0;
        goto release_ok;
    }
    if (state != 1) goto release_ok;

    /* ---- composite, first visit: one child record per component ---- */
    {
        uint8_t more_instr = 0;  /* the original reuses the home slot of has_outline for it */
        last = 0;
        instr = 0;
        AT(gd, 0x38, int32_t) = 2;
        uint16_t count = 0;
        do {
            uint16_t maxdepth = AT(maxp, 0x1e, uint16_t);
            if (maxdepth < 1) maxdepth = 1;
            else if (maxdepth > 20) maxdepth = 20;
            if ((uint32_t)AT(gd, 0x4e, uint16_t) + 1 > maxdepth) { err = 0x140b; goto fail; }
            count++;
            uint16_t maxelems = AT(maxp, 0x1c, uint16_t);
            if (maxelems < 3) maxelems = 3;
            if (count > maxelems) { err = 0x140b; goto fail; }
            uint8_t *map = AT(ws, 0x18, uint8_t *);
            uint32_t slot = 0;
            while (slot < nslots && !map[slot]) slot++;
            if (slot == nslots) { err = 0x140c; goto fail; }
            map[slot] = 0;
            uint8_t *child = AT(ws, 0x20, uint8_t *) + (uint64_t)slot * 0xf8;
            FSG(fsg_InitializeGlyphData)(child, ws, 0, (uint16_t)(AT(gd, 0x4e, uint16_t) + 1));
            FSG(fsg_LinkChild)(gd, child);
            int32_t rc = sfac_ReadComponentData(handle, child + 0x50, child + 0x54, child + 0x85, child + 0x86,
                                                &more_instr, child + 0x3c, child + 0x56, child + 0x58, child + 0x5a,
                                                child + 0x5c, child + 0x60, &matrix, &last);
            if (AT(child, 0x3c, uint16_t) >= AT(maxp, 0x04, uint16_t)) { err = 0x1410; goto fail; }
            if (matrix) {
                FSG(mth_MxConcat2x2)((int32_t *)(child + 0x60), (int32_t *)(child + 0xb0));
                /* the same transform survives only a +-1 scale without rotation */
                int32_t a = AT(child, 0x60, int32_t), dd = AT(child, 0x70, int32_t);
                if (!(AT(child, 0x64, int32_t) == 0 && AT(child, 0x6c, int32_t) == 0 && wrap_abs(a) == wrap_abs(dd) &&
                      wrap_abs(a) == 0x10000))
                    AT(child, 0xd4, uint8_t) = 0;
            }
            if (rc) { err = rc; goto fail; }
            instr |= more_instr;
        } while (!last);
        uint8_t *e = *pelem;
        size_t bytes = (size_t)(uint16_t)(count + 4) << 2;
        memset(E_OOY(e), 0, bytes);
        memset(E_OOX(e), 0, bytes);
        /* the variation callback moves the component offsets, given as points, and the 4
         * phantoms after them */
        e = *pelem;
        CL_VARIATION(client)(CL_ID(client), AT(gd, 0x3c, uint16_t), E_EP(e), (uint16_t)E_NC(e), E_OOX(e), E_OOY(e),
                             (uint32_t)count + 4);
        uint16_t i = 0;
        for (uint8_t *c = AT(gd, 0x08, uint8_t *); c != gd && i < count; c = AT(c, 0x00, uint8_t *), i++) {
            AT(c, 0x56, uint16_t) = (uint16_t)(AT(c, 0x56, uint16_t) + (uint16_t)E_OOX(*pelem)[i]);
            AT(c, 0x58, uint16_t) = (uint16_t)(AT(c, 0x58, uint16_t) + (uint16_t)E_OOY(*pelem)[i]);
        }
        e = *pelem;
        for (int k = 0; k < 4; k++) {
            AT(gd, 0xd8 + 8 * k, int32_t) = E_OOX(e)[count + k];
            AT(gd, 0xdc + 8 * k, int32_t) = E_OOY(e)[count + k];
        }
        if (instr) {
            err = sfac_ReadCompositeInstructions(handle, gd + 0xa0, gd + 0x9a);
            if (err) goto fail;
        }
        /* the glyph data stays held: state 2 reads the composite's instructions from it */
        ar.client = NULL;
        ar.handle = NULL;
        return 0;
    }

release_ok:
    if (AT(handle, 0x18, void *)) {
        CL_RELEASE(rel_client)(AT(handle, 0x10, void *));
        AT(handle, 0x10, void *) = NULL;
        AT(handle, 0x18, void *) = NULL;
        AT(handle, 0x08, void *) = NULL;
        AT(handle, 0x00, uint32_t) = 0;
    }
    ar.client = NULL;
    ar.handle = NULL;
    return 0;
fail:
    FSG(AutoReleaseGlyph_Release)(&ar);
    return err;
}

/* fsg_GridFit 0x18000c3e4: the whole glyph, from its root record down through its components;
 * then emboldening, post transform, and the shift that puts the left side bearing point on
 * the (sub)pixel grid */
TT_EXPORT int32_t tt_fsg_GridFit(uint8_t *client, uint8_t *maxp, uint8_t *xform, uint8_t *G, uint8_t *ws,
                                 uint8_t *twilight, void *trace, uint8_t hint, uint16_t *scan, uint8_t *has_outline,
                                 uint16_t *aw, uint8_t embold_flag, uint8_t overscaled, uint16_t ovx, uint16_t ovy) {
    uint16_t ntwilight = MAXP_TWILIGHT(maxp);
    if (ntwilight > 0x7fff) SafeIntOnOverflow();
    uint32_t n12 = 0, n13 = 0;
    uint8_t outline = 0;
    E_SP(twilight)[0] = 0;
    E_EP(twilight)[0] = (int16_t)(ntwilight - 1);
    E_NC(twilight) = 1;
    *has_outline = 0;
    uint16_t maxdepth = AT(maxp, 0x1e, uint16_t);
    uint32_t depth = maxdepth < 1 ? 1 : maxdepth > 20 ? 20 : maxdepth;
    uint16_t maxelems = AT(maxp, 0x1c, uint16_t);
    if (maxelems < 3) maxelems = 3;
    uint32_t nslots = (uint32_t)maxelems + depth + 1;  /* records: components plus nesting */
    if (nslots < depth + 1) SafeIntOnOverflow();
    uint8_t *map = AT(ws, 0x18, uint8_t *);
    if (nslots) memset(map, 1, nslots);
    map = AT(ws, 0x18, uint8_t *);
    uint32_t slot = 0;
    while (slot < nslots && !map[slot]) slot++;
    if (slot == nslots) return 0x140c;
    map[slot] = 0;
    /* the root record: fsg_InitializeGlyphData, inlined, with the client's glyph at depth 0 */
    uint8_t *gd = AT(ws, 0x20, uint8_t *) + (uint64_t)slot * 0xf8;
    AT(gd, 0x98, uint16_t) = 0xffff;
    memcpy(gd + 0x60, identity3x3, 0x24);
    AT(gd, 0x00, void *) = NULL;
    AT(gd, 0x08, void *) = NULL;
    AT(gd, 0x10, void *) = NULL;
    AT(gd, 0x38, int32_t) = 3;
    AT(gd, 0x20, void *) = NULL;
    AT(gd, 0x30, void *) = NULL;
    AT(gd, 0x28, void *) = NULL;
    AT(gd, 0x18, uint32_t) = 0;
    AT(gd, 0x4e, uint16_t) = 0;
    AT(gd, 0x85, uint16_t) = 0;
    AT(gd, 0x84, uint8_t) = 0;
    AT(gd, 0x3e, uint32_t) = 0x7fff7fff;
    AT(gd, 0x42, uint32_t) = 0x80008000;
    AT(gd, 0x9a, uint16_t) = 0;
    AT(gd, 0xa0, void *) = NULL;
    AT(gd, 0x46, uint16_t) = 0;
    AT(gd, 0x4a, uint16_t) = 0;
    AT(gd, 0x50, int32_t) = 2;
    AT(gd, 0x54, uint8_t) = 0;
    AT(gd, 0x3c, uint16_t) = AT(client, 0xe8, uint16_t);
    AT(gd, 0x56, uint64_t) = 0;
    AT(gd, 0x88, uint64_t) = 0;
    AT(gd, 0x90, uint64_t) = 0;
    uint8_t *e0 = AT(ws, 0x10, uint8_t *);
    AT(gd, 0xa8, uint8_t *) = e0;
    E_NC(e0) = 0;
    memcpy(gd + 0xb0, identity3x3, 0x24);
    AT(gd, 0xd4, uint8_t) = 1;
    AT(gd, 0xd8, uint64_t) = 0;
    AT(gd, 0xe0, uint64_t) = 0;
    AT(gd, 0xe8, uint64_t) = 0;
    AT(gd, 0xf0, uint64_t) = 0;
    AT(G, 0x1a0, int32_t) = 10000000;  /* instruction budget for the whole glyph */
    while (gd) {
        int32_t err = FSG(fsg_ExecuteGlyph)(client, maxp, xform, nslots, G, gd, ws, twilight, trace, hint, &outline,
                                            &n12, &n13);
        if (err) return err;
        *has_outline |= outline;
        *scan = AT(gd, 0x98, uint16_t);
        uint8_t *next = AT(gd, 0x08, uint8_t *);
        if (next) {  /* down to the first child; the record comes back in state 2 */
            AT(gd, 0x08, void *) = NULL;
        } else {     /* done: free the slot, go to the sibling or back to the parent */
            next = AT(gd, 0x00, uint8_t *);
            AT(ws, 0x18, uint8_t *)[(gd - AT(ws, 0x20, uint8_t *)) / 0xf8] = 1;
        }
        *aw = AT(gd, 0x46, uint16_t);
        gd = next;
    }
    if (AT(xform, 0x34, uint8_t)) {
        fsg_Embold(ws, G, hint, embold_flag, overscaled);
        if (*aw) *aw = (uint16_t)(*aw + (int16_t)(((int32_t)AT(xform, 0, uint16_t) * 2 - 1) / 100));
    }
    if ((AT(xform, 0x30, uint32_t) & 0x2000) || AT(G, 0x1d4, uint8_t))
        scl_PostTransformGlyph(G, AT(ws, 0x10, uint8_t *), xform + 4);
    /* shift the outline so the left side bearing point lands where the transform puts the
     * origin (xform + 0xc, + 0x18 in 16.16, rounded to 26.6), on the pixel grid when hinting */
    uint8_t *e = AT(ws, 0x10, uint8_t *);
    int32_t tx = (int32_t)((uint32_t)AT(xform, 0x0c, int32_t) + 0x200) >> 10;
    int32_t ty = (int32_t)((uint32_t)AT(xform, 0x18, int32_t) + 0x200) >> 10;
    if (overscaled) {
        tx = (int32_t)((uint32_t)tx * ovx);
        ty = (int32_t)((uint32_t)ty * ovy);
    }
    uint16_t lp = last_point(e);
    uint16_t p = (uint16_t)(lp + 1);
    uint32_t dx = (uint32_t)tx - (uint32_t)E_X(e)[p];
    uint32_t dy = (uint32_t)ty - (uint32_t)E_Y(e)[p];
    if (hint && !AT(G, 0x1d4, uint8_t)) dx = overscaled ? (dx + 2) & 0xfffffffcu : (dx + 0x20) & 0xffffffc0u;
    if (dx || dy) {
        if ((uint16_t)(lp + 9) == 0) return 0;
        for (int32_t i = 0; i < (int32_t)(uint16_t)(last_point(e) + 9); i++) {
            E_X(e)[i] = (int32_t)((uint32_t)E_X(e)[i] + dx);
            E_Y(e)[i] = (int32_t)((uint32_t)E_Y(e)[i] + dy);
        }
    }
    return 0;
}
