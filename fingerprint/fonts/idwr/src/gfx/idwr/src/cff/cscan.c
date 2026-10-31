/* CScan, the run-based scan converter of DWrite's CFF rasterizer (notes/cff.md, group 8
 * "cscan"), ported from DWrite.dll 10.0.26100.9711:
 *
 *   ATMBuildBitMap  0xb9370   the proc table's bitmap consumer (CFFFontInst + 0x5e8)
 *   BuildRuns       0xb9518   runs the interpreter with the CScan path procs, grows the pool
 *   IniCScan        0xb9788   ResetCScan 0xc0e30   CSNewLine 0x1846d0   CSNewPoint 0x184740
 *   CSClose         0xc01c0   CSPathPoints 0xc0208  BuildCrosses 0xc0414  FinishPath 0xc0ec8
 *   CScanFill       0xbf620   CScan 0xbf894   BuildYCross 0xbfa40   AddYCrosses 0xbfb8c
 *   BuildInitialRuns 0xbf368  CrossWindCount 0xbefa0   ReturnBits 0xbf160   MergeRuns 0x183dc0
 *   the crossing helpers: NewCross 0xbe7b8, NewLinkCross 0xc10c8, NewXtraCross 0xc1ac8,
 *   AddYCross 0xbe624, OffsetCenterSlope 0xc0b78, PathXtraCross 0xc0df0, PathXtraOrig 0xc0dc4,
 *   ForwPathCross 0xc18c0, BackPathCross 0xc18f4, PathUpCross 0xc1a98, RunPair 0xc1928;
 *   the buffers: ATM_GrowBuff 0xb8208, GrowRunBuff 0x183250.
 *
 * The data structures are in cscan.h. How it works: the interpreter feeds the device-space
 * path (16.16, y down: CSNewLine negates y) to CSNewLine/CSNewPoint, which buffer up to 0x46
 * points; CSPathPoints hands them to BuildCrosses, which records one crossing per pixel-centre
 * row (y = n + 0.5) a segment crosses, in path order, in the crossing pool. FinishPath links
 * the two ends of each closed path. At endchar the interpreter calls CScanFill -> CScan:
 * BuildYCross sorts the crossings into row lists by x, BuildInitialRuns pairs them into runs
 * (even-odd, or the nonzero rule with the winding option) and rounds each to a pixel column,
 * the dropout group repairs the runs (EditBlackSpace, EditWhiteSpace), and ReturnBits writes
 * them into the run buffer that ConvertRuns (qreduce group) turns into bitmap rows.
 *
 * Arithmetic: BuildCrosses computes the slope as fixdiv(dx, dy) and each crossing's x offset
 * as fixmul(dy0, slope), both inlined in double precision as in cff_math.c.
 *
 * Every call between these functions goes through the original entry point (CFF_ORIG), and
 * every function pointer stored in a record is the original's address: Type1InterpretCharString
 * tells the CScan procs from the outline client's by comparing procs[2] with the original
 * CSNewLine. In oracle mode the exports are the register-preserving thunk below (CS_EXPORT),
 * which also forwards stack arguments: CFF_EXPORT (cff_port.h) does not, so it can only wrap
 * functions with at most four arguments.
 *
 * Negative controls: -DCFF_CONTROL_<NAME> breaks one function (see the CONTROL comments). */
#include <string.h>
#include "cff_port.h"
#include "cscan.h"

#ifdef IDWR_STANDALONE
#define CS_EXPORT(name, impl) _Static_assert(sizeof(&name) == sizeof(&impl), #name)
#define cff_ATM_GrowBuff_impl cff_ATM_GrowBuff
#define cff_ATMBuildBitMap_impl cff_ATMBuildBitMap
#define cff_BuildRuns_impl cff_BuildRuns
#define cff_IniCScan_impl cff_IniCScan
#define cff_AddYCross_impl cff_AddYCross
#define cff_NewCross_impl cff_NewCross
#define cff_CrossWindCount_impl cff_CrossWindCount
#define cff_ReturnBits_impl cff_ReturnBits
#define cff_BuildInitialRuns_impl cff_BuildInitialRuns
#define cff_CScanFill_impl cff_CScanFill
#define cff_CScan_impl cff_CScan
#define cff_BuildYCross_impl cff_BuildYCross
#define cff_AddYCrosses_impl cff_AddYCrosses
#define cff_CSClose_impl cff_CSClose
#define cff_CSPathPoints_impl cff_CSPathPoints
#define cff_BuildCrosses_impl cff_BuildCrosses
#define cff_OffsetCenterSlope_impl cff_OffsetCenterSlope
#define cff_PathXtraOrig_impl cff_PathXtraOrig
#define cff_PathXtraCross_impl cff_PathXtraCross
#define cff_ResetCScan_impl cff_ResetCScan
#define cff_FinishPath_impl cff_FinishPath
#define cff_NewLinkCross_impl cff_NewLinkCross
#define cff_ForwPathCross_impl cff_ForwPathCross
#define cff_BackPathCross_impl cff_BackPathCross
#define cff_RunPair_impl cff_RunPair
#define cff_PathUpCross_impl cff_PathUpCross
#define cff_NewXtraCross_impl cff_NewXtraCross
#define cff_GrowRunBuff_impl cff_GrowRunBuff
#define cff_MergeRuns_impl cff_MergeRuns
#define cff_CSNewLine_impl cff_CSNewLine
#define cff_CSNewPoint_impl cff_CSNewPoint
#elif defined(_WIN32)
/* CFF_EXPORT's thunk (saves RCX, RDX, R8-R11, XMM1-XMM5 for the callers that keep values in
 * them) with a frame that also copies the caller's stack arguments 5..8, so the port reads
 * its 5th..8th arguments where it expects them. Copying four slots whatever the arity is
 * harmless: they are in the caller's frame. */
#define CS_EXPORT(name, impl)                                                          \
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
            "subq $0x98, %rsp\n.seh_stackalloc 0x98\n"                                 \
            "movaps %xmm1, 0x40(%rsp)\n.seh_savexmm %xmm1, 0x40\n"                     \
            "movaps %xmm2, 0x50(%rsp)\n.seh_savexmm %xmm2, 0x50\n"                     \
            "movaps %xmm3, 0x60(%rsp)\n.seh_savexmm %xmm3, 0x60\n"                     \
            "movaps %xmm4, 0x70(%rsp)\n.seh_savexmm %xmm4, 0x70\n"                     \
            "movaps %xmm5, 0x80(%rsp)\n.seh_savexmm %xmm5, 0x80\n"                     \
            ".seh_endprologue\n"                                                       \
            "movq 0xf0(%rsp), %rax\nmovq %rax, 0x20(%rsp)\n"                           \
            "movq 0xf8(%rsp), %rax\nmovq %rax, 0x28(%rsp)\n"                           \
            "movq 0x100(%rsp), %rax\nmovq %rax, 0x30(%rsp)\n"                          \
            "movq 0x108(%rsp), %rax\nmovq %rax, 0x38(%rsp)\n"                          \
            "callq " #impl "\n"                                                        \
            "movaps 0x40(%rsp), %xmm1\n"                                               \
            "movaps 0x50(%rsp), %xmm2\n"                                               \
            "movaps 0x60(%rsp), %xmm3\n"                                               \
            "movaps 0x70(%rsp), %xmm4\n"                                               \
            "movaps 0x80(%rsp), %xmm5\n"                                               \
            "addq $0x98, %rsp\n"                                                       \
            "popq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n"         \
            "retq\n"                                                                   \
            ".seh_endproc\n"                                                           \
            ".section .drectve,\"yn\"\n"                                               \
            ".ascii \" -export:" #name "\"\n"                                          \
            ".text\n")
#endif

/* ------------------------------------------------------------------------------------------
 * original entry points */

#define RVA_ATM_GrowBuff 0xb8208
#define RVA_IniCScan 0xb9788
#define RVA_BuildRuns 0xb9518
#define RVA_AddYCross 0xbe624
#define RVA_NewCross 0xbe7b8
#define RVA_CrossWindCount 0xbefa0
#define RVA_ReturnBits 0xbf160
#define RVA_BuildInitialRuns 0xbf368
#define RVA_CScan 0xbf894
#define RVA_BuildYCross 0xbfa40
#define RVA_AddYCrosses 0xbfb8c
#define RVA_CSClose 0xc01c0
#define RVA_CSPathPoints 0xc0208
#define RVA_BuildCrosses 0xc0414
#define RVA_OffsetCenterSlope 0xc0b78
#define RVA_PathXtraCross 0xc0df0
#define RVA_ResetCScan 0xc0e30
#define RVA_FinishPath 0xc0ec8
#define RVA_NewLinkCross 0xc10c8
#define RVA_ForwPathCross 0xc18c0
#define RVA_BackPathCross 0xc18f4
#define RVA_NewXtraCross 0xc1ac8
#define RVA_GrowRunBuff 0x183250
#define RVA_MergeRuns 0x183dc0
#define RVA_CSNewLine 0x1846d0
#define RVA_CSNewPoint 0x184740
/* other groups */
#define RVA_FFloorFModN 0xb1c1c              /* setup */
#define RVA_fixmul 0xb3340                   /* setup */
#define RVA_ConvertRuns 0xb82e8              /* qreduce */
#define RVA_Type1InterpretCharString 0xb981c /* interp */
#define RVA_SetGSMatrix 0xbe544              /* interp */
#define RVA_FixCrossFlags 0xc1244            /* dropout */
#define RVA_EditBlackSpace 0xc1598           /* dropout */
#define RVA_EditWhiteSpace 0xc1b9c           /* dropout */
#define RVA_FixedFltn 0xc71e0                /* path */
#define RVA_ATM_ManageCString 0x183230       /* front */

typedef int32_t (*ATM_GrowBuff_fn)(CsGrowBuf *buf, int32_t delta, int32_t keep, CsMemRec *mem);
typedef int32_t (*IniCScan_fn)(CsState **out, CsGrowBuf *pool, CsGrowBuf *statebuf, CsZone *zone);
typedef int32_t (*BuildRuns_fn)(void *state, void *chardesc, void *io, CsRunRec *rr, CsMemRec *mem,
                                void *growctx, uint32_t flags);
typedef void *(*AddYCross_fn)(CsState *cs, CsCross *c);
typedef CsCross *(*NewCross_fn)(CsState *cs, int32_t x, int32_t y);
typedef int32_t (*CrossWindCount_fn)(void *unused, CsCross *c);
typedef int32_t (*ReturnBits_fn)(CsState *cs, CsRunRec *rr, CsMemRec *mem, void *growctx);
typedef int32_t (*BuildInitialRuns_fn)(CsCtx *ctx, int32_t center);
typedef int32_t (*CScan_fn)(CsCtx *ctx, CsRunRec *rr, int32_t *bbox, CsMemRec *mem, void *state, void *growctx);
typedef int32_t (*BuildYCross_fn)(CsState *cs, int32_t *any);
typedef void (*AddYCrosses_fn)(CsState *cs, CsCross *end);
typedef int32_t (*CSPathPoints_fn)(CsCtx *ctx, CsPt *pts, int32_t n, int32_t close, void *st);
typedef int32_t (*BuildCrosses_fn)(CsCtx *ctx, CsPt *pts, int32_t n, int32_t small, int32_t center);
typedef int32_t (*OffsetCenterSlope_fn)(int32_t slope);
typedef CsCross *(*PathXtraCross_fn)(CsCross *c, int32_t dir);
typedef int32_t (*FinishPath_fn)(CsCtx *ctx, int32_t closed);
typedef CsCross *(*NewLinkCross_fn)(CsState *cs);
typedef CsCross *(*PathCross_fn)(CsCross *c);
typedef int32_t (*MergeRuns_fn)(CsRunRec *a, CsRunRec *b, int32_t dx, int32_t dy, CsRunRec *out, CsMemRec *mem,
                                void *growctx);
typedef int32_t (*FFloorFModN_fn)(int32_t v, uint32_t n);
typedef int32_t (*fixmul_fn)(int32_t a, int32_t b);
typedef int32_t (*ConvertRuns_fn)(CsRunRec *rr, void *a5, uint32_t scale, CsMemRec *mem, int32_t rows);
typedef int32_t (*Type1InterpretCharString_fn)(void *state, CsMemRec *mem, void *chardesc, void *io, CsRunRec *rr,
                                               void **procs, uint32_t *flags);
typedef int32_t (*SetGSMatrix_fn)(void *m, int32_t which, void *state);
typedef void (*FixCrossFlags_fn)(CsCross *a, CsCross *b);
typedef int32_t (*EditBlackSpace_fn)(CsState *cs);
typedef void (*EditWhiteSpace_fn)(CsState *cs, void *state);

#define AT(p, off, type) (*(type *)((uint8_t *)(p) + (off)))
/* a record at a byte offset (the link records hold byte offsets) */
#define CROSS_AT(c, off) ((CsCross *)((uintptr_t)(c) + (uintptr_t)(intptr_t)(off)))
/* the row of a crossing: the int16 at +0x0e, the high half of y */
#define ROW_OF(c) ((int16_t)((uint32_t)(c)->y >> 16))
/* the row list head of row r (cs->rows is biased: it may point outside the pool) */
#define ROW_HEAD(cs, r) (*(CsCross **)((uintptr_t)(cs)->rows + (uintptr_t)((intptr_t)(r) * 8)))

static inline int32_t wrap_add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t wrap_sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t shl16(int32_t v) { return (int32_t)((uint32_t)v << 16); }

/* ------------------------------------------------------------------------------------------
 * buffers */

/* Grows buf by delta bytes: realloc (keep != 0) or free + malloc (the contents are lost).
 * On failure the buffer is freed and emptied. */
CFF_API int32_t cff_ATM_GrowBuff_impl(CsGrowBuf *buf, int32_t delta, int32_t keep, CsMemRec *mem) {
    uint32_t size = buf->size + (uint32_t)delta;
    CsZone *z;
    void *p;
    if (size < buf->size) {
        z = mem->zone;
        z->procs->free(z, buf->ptr);
        buf->ptr = NULL;
        buf->size = 0;
        return (int32_t)0xfc0200ef;
    }
    if (keep == 0) {
        if (buf->ptr) {
            z = mem->zone;
            z->procs->free(z, buf->ptr);
        }
        z = mem->zone;
        p = z->procs->alloc(z, size);
    } else {
        z = mem->zone;
        p = z->procs->realloc(z, buf->ptr, size);
        if (!p) {
            z = mem->zone;
            z->procs->free(z, buf->ptr);
        }
    }
#ifdef CFF_CONTROL_ATM_GROWBUFF
    p = NULL; /* CONTROL: every grow fails */
#endif
    buf->ptr = p;
    if (!p) {
        buf->size = 0;
        return (int32_t)0xfc020107;
    }
    buf->size = size;
    return 0;
}
CS_EXPORT(cff_ATM_GrowBuff, cff_ATM_GrowBuff_impl);

/* the run buffer's grow proc (CsMemRec.grow): grows growctx->runbuf and points rr at it */
CFF_API int32_t cff_GrowRunBuff_impl(uint32_t delta, CsRunRec *rr, int32_t keep, CsGrowCtx *g) {
    if (CFF_ORIG(ATM_GrowBuff_fn, RVA_ATM_GrowBuff)(g->runbuf, (int32_t)delta, keep, g->mem) != 0)
        return 0;
#ifdef CFF_CONTROL_GROWRUNBUFF
    return 0; /* CONTROL: the run buffer never grows */
#endif
    rr->data = (int16_t *)g->runbuf->ptr;
    rr->cap = g->runbuf->size;
    return 1;
}
CS_EXPORT(cff_GrowRunBuff, cff_GrowRunBuff_impl);

/* ------------------------------------------------------------------------------------------
 * the crossing helpers */

CFF_API CsCross *cff_NewCross_impl(CsState *cs, int32_t x, int32_t y) {
    CsCross *p = cs->pool_cur, *c = p != cs->pool_end ? p : NULL;
    cs->pool_cur = p + 1;
    if (c) {
        c->x = x;
        c->y = y;
        c->flags = 0;
#ifdef CFF_CONTROL_NEWCROSS
        c->x = wrap_add(x, 0x8000); /* CONTROL: spliced crossings half a pixel right */
#endif
    }
    return c;
}
CS_EXPORT(cff_NewCross, cff_NewCross_impl);

CFF_API CsCross *cff_NewLinkCross_impl(CsState *cs) {
    CsCross *p = cs->pool_cur, *c;
    cs->pool_cur = p + 1;
    c = p != cs->pool_end ? p : NULL;
    if (c) {
        c->x = 0;
        c->y = 0;
        c->flags = CS_JUMP | CS_EXTRA;
#ifdef CFF_CONTROL_NEWLINKCROSS
        c->flags = CS_EXTRA; /* CONTROL: the path ends are not joined */
#endif
    }
    return c;
}
CS_EXPORT(cff_NewLinkCross, cff_NewLinkCross_impl);

CFF_API CsCross *cff_NewXtraCross_impl(CsState *cs, int32_t x, int32_t y) {
    CsCross *p = cs->pool_cur, *c;
    cs->pool_cur = p + 1;
    c = p != cs->pool_end ? p : NULL;
    if (c) {
        c->x = x;
        c->y = y;
        c->flags = CS_EXTRA;
#ifdef CFF_CONTROL_NEWXTRACROSS
        c->x = wrap_add(x, 0x10000); /* CONTROL: extra records a pixel right */
#endif
    }
    return c;
}
CS_EXPORT(cff_NewXtraCross, cff_NewXtraCross_impl);

/* inserts one crossing into its row list after every crossing with x <= its x; returns RAX */
CFF_API void *cff_AddYCross_impl(CsState *cs, CsCross *c) {
    CsCross *prev = NULL, *p;
    int16_t row;
    if (c->flags & CS_EXTRA)
        return (void *)(uintptr_t)CS_EXTRA;
    row = ROW_OF(c);
#ifdef CFF_CONTROL_ADDYCROSS
    c->next = ROW_HEAD(cs, row); /* CONTROL: not sorted in */
    ROW_HEAD(cs, row) = c;
    return c->next;
#endif
    for (p = ROW_HEAD(cs, row); p; p = p->next) {
        if (p->x > c->x)
            break;
        prev = p;
    }
    if (prev)
        prev->next = c;
    else
        ROW_HEAD(cs, row) = c;
    c->next = p;
    return p;
}
CS_EXPORT(cff_AddYCross, cff_AddYCross_impl);

/* the class of a slope (dx/dy, 16.16) for the "center" option: 1..5 */
CFF_API int32_t cff_OffsetCenterSlope_impl(int32_t slope) {
    int32_t a = slope;
    if ((int32_t)(0u - (uint32_t)slope) >= 0)
        a = (int32_t)(0u - (uint32_t)slope);
#ifdef CFF_CONTROL_OFFSETCENTERSLOPE
    a += 0x1000; /* CONTROL: shifted class boundaries */
#endif
    if (a < 0x14da0)
        return 1 + (a > 0x21b4);
    if (a < 0x79883)
        return 3 + (a >= 0x26a0a);
    return 5;
}
CS_EXPORT(cff_OffsetCenterSlope, cff_OffsetCenterSlope_impl);

/* the next (dir == 1) or previous record of a path, following one jump */
CFF_API CsCross *cff_PathXtraCross_impl(CsCross *c, int32_t dir) {
    CsCross *p;
    if (dir == 1) {
        p = c + 1;
#ifndef CFF_CONTROL_PATHXTRACROSS /* CONTROL: the forward jump not followed */
        if (p->flags & CS_JUMP)
            p = CROSS_AT(p, p->x);
#endif
    } else {
        p = c - 1;
        if (p->flags & CS_JUMP)
            p = CROSS_AT(p, p->y);
    }
    return p;
}
CS_EXPORT(cff_PathXtraCross, cff_PathXtraCross_impl);

CFF_API CsCross *cff_PathXtraOrig_impl(CsCross *c, int32_t dir) {
    do
        c = CFF_ORIG(PathXtraCross_fn, RVA_PathXtraCross)(c, dir);
    while (c->flags & CS_ORIG);
#ifdef CFF_CONTROL_PATHXTRAORIG
    c = CFF_ORIG(PathXtraCross_fn, RVA_PathXtraCross)(c, dir); /* CONTROL: one record too far */
#endif
    return c;
}
CS_EXPORT(cff_PathXtraOrig, cff_PathXtraOrig_impl);

/* the next crossing of the path (skipping the extra records) */
CFF_API CsCross *cff_ForwPathCross_impl(CsCross *c) {
    CsCross *p = c + 1;
#ifdef CFF_CONTROL_FORWPATHCROSS
    return p; /* CONTROL: extra records not skipped */
#endif
    while (p->flags & CS_EXTRA)
        p = (p->flags & CS_JUMP) ? CROSS_AT(p, p->x) : p + 1;
    return p;
}
CS_EXPORT(cff_ForwPathCross, cff_ForwPathCross_impl);

CFF_API CsCross *cff_BackPathCross_impl(CsCross *c) {
    CsCross *p = c - 1;
#ifdef CFF_CONTROL_BACKPATHCROSS
    return p; /* CONTROL: extra records not skipped */
#endif
    while (p->flags & CS_EXTRA)
        p = (p->flags & CS_JUMP) ? CROSS_AT(p, p->y) : p - 1;
    return p;
}
CS_EXPORT(cff_BackPathCross, cff_BackPathCross_impl);

/* the path neighbour of c in the direction that goes down (larger y) */
CFF_API CsCross *cff_PathUpCross_impl(CsCross *c) {
    CsCross *p = c + 1;
    if (p->flags & CS_JUMP)
        p = CROSS_AT(p, p->x);
#ifdef CFF_CONTROL_PATHUPCROSS
    if (p->y <= c->y) /* CONTROL: the other direction */
#else
    if (p->y > c->y)
#endif
        return CFF_ORIG(PathCross_fn, RVA_ForwPathCross)(c);
    return CFF_ORIG(PathCross_fn, RVA_BackPathCross)(c);
}
CS_EXPORT(cff_PathUpCross, cff_PathUpCross_impl);

/* +1 when the path goes up (to smaller y) after c, else -1 */
CFF_API int32_t cff_CrossWindCount_impl(void *unused, CsCross *c) {
    CsCross *p = c + 1;
    (void)unused;
    if (p->flags & CS_JUMP)
        p = CROSS_AT(p, p->x);
#ifdef CFF_CONTROL_CROSSWINDCOUNT
    p = c + 1; /* CONTROL: the jump to the other end of the path not followed */
#endif
    return p->y < c->y ? 1 : -1;
}
CS_EXPORT(cff_CrossWindCount, cff_CrossWindCount_impl);

/* the left crossing of the run whose right crossing is c; c itself if it starts its row */
CFF_API CsCross *cff_RunPair_impl(CsState *cs, CsCross *c) {
    CsCross *p, *q;
    if (c->flags & CS_ROWSTART)
        return c;
#ifdef CFF_CONTROL_RUNPAIR
    return NULL; /* CONTROL: no pair found */
#endif
    for (p = ROW_HEAD(cs, ROW_OF(c)); p; p = q->next) {
        q = p->next;
        if (!q)
            return NULL;
        if (q == c)
            return p;
    }
    return NULL;
}
CS_EXPORT(cff_RunPair, cff_RunPair_impl);

/* ------------------------------------------------------------------------------------------
 * paths */

CFF_API int32_t cff_IniCScan_impl(CsState **out, CsGrowBuf *pool, CsGrowBuf *statebuf, CsZone *zone) {
    CsState *cs;
    if (statebuf->size < sizeof(CsState)) {
        if (statebuf->ptr)
            zone->procs->free(zone, statebuf->ptr);
        statebuf->ptr = zone->procs->alloc(zone, sizeof(CsState));
        if (!statebuf->ptr)
            return 0;
        statebuf->size = sizeof(CsState);
    }
    cs = (CsState *)statebuf->ptr;
#ifdef CFF_CONTROL_INICSCAN
    return 0; /* CONTROL: no CScan state */
#endif
    *out = cs;
    cs->pool_buf = pool;
    return 1;
}
CS_EXPORT(cff_IniCScan, cff_IniCScan_impl);

/* proc 0: a new glyph */
CFF_API int32_t cff_ResetCScan_impl(void *unused, CsCtx *ctx) {
    CsState *cs = ctx->cs;
    CsGrowBuf *b = cs->pool_buf;
    CsCross *p = (CsCross *)b->ptr, *link;
    (void)unused;
    cs->pool_cur = p;
    cs->pool_start = p;
    cs->xmin = 0x7fffffff;
    cs->ymin = 0x7fffffff;
#ifdef CFF_CONTROL_RESETCSCAN
    cs->xmin = 0; /* CONTROL: the box starts out of range (CScan refuses it) */
    cs->ymin = (int32_t)0xe0000000;
#endif
    cs->pool_end = p + b->size / sizeof(CsCross);
    cs->xmax = (int32_t)0x80000000;
    cs->ymax = (int32_t)0x80000000;
    link = CFF_ORIG(NewLinkCross_fn, RVA_NewLinkCross)(cs);
    cs->link = link;
    if (!link)
        return 0;
    cs->prev_link = NULL;
    cs->newpath = 1;
    cs->list7 = NULL;
    cs->list6 = NULL;
    cs->npts = 0;
    return 1;
}
CS_EXPORT(cff_ResetCScan, cff_ResetCScan_impl);

/* proc 2: a line to p (also the first point of a path); y is negated (y down) */
CFF_API int32_t cff_CSNewLine_impl(CsPt *p, void *st, CsCtx *ctx) {
    CsState *cs = ctx->cs;
    int32_t n = cs->npts;
    if (n >= 0x46) {
        if (!CFF_ORIG(CSPathPoints_fn, RVA_CSPathPoints)(ctx, cs->pts, n, 0, st))
            return 0;
        cs->npts = 0;
        n = 0;
    }
    cs->pts[n].x = p->x;
#ifdef CFF_CONTROL_CSNEWLINE
    cs->pts[n].x = wrap_add(p->x, 0x8000); /* CONTROL: lines half a pixel right */
#endif
    cs->pts[cs->npts].y = (int32_t)(0u - (uint32_t)p->y);
    cs->npts++;
    return 1;
}
CS_EXPORT(cff_CSNewLine, cff_CSNewLine_impl);

/* proc 1: a move to p; right after the first point or two it replaces the last one */
CFF_API int32_t cff_CSNewPoint_impl(CsPt *p, void *st, CsCtx *ctx) {
    CsState *cs = ctx->cs;
    int32_t n = cs->npts;
    if (n >= 0x46) {
        if (!CFF_ORIG(CSPathPoints_fn, RVA_CSPathPoints)(ctx, cs->pts, n, 0, st))
            return 0;
        cs->npts = 0;
        n = 0;
#ifdef CFF_CONTROL_CSNEWPOINT
    } else if (0) { /* CONTROL: moves appended */
#else
    } else if (n == 1 || n == 2) {
#endif
        cs->pts[n - 1].x = p->x;
        cs->pts[cs->npts - 1].y = (int32_t)(0u - (uint32_t)p->y);
        return 1;
    }
    cs->pts[n].x = p->x;
    cs->pts[cs->npts].y = (int32_t)(0u - (uint32_t)p->y);
    cs->npts++;
    return 1;
}
CS_EXPORT(cff_CSNewPoint, cff_CSNewPoint_impl);

/* proc 4: close the path */
CFF_API int32_t cff_CSClose_impl(void *st, CsCtx *ctx) {
    CsState *cs = ctx->cs;
    if (cs->npts <= 1 && cs->newpath != 0)
        return 1;
#ifdef CFF_CONTROL_CSCLOSE
    return CFF_ORIG(CSPathPoints_fn, RVA_CSPathPoints)(ctx, cs->pts, cs->npts, 0, st); /* CONTROL: not closed */
#endif
    return CFF_ORIG(CSPathPoints_fn, RVA_CSPathPoints)(ctx, cs->pts, cs->npts, 1, st);
}
CS_EXPORT(cff_CSClose, cff_CSClose_impl);

CFF_API int32_t cff_CSPathPoints_impl(CsCtx *ctx, CsPt *pts, int32_t n, int32_t close, void *st) {
    CsState *cs = ctx->cs;
    int32_t small, ok;
    if (cs->newpath != 0) {
        if (n < 3 && close != 0) {
            cs->npts = 0;
            cs->newpath = 1;
            return 1;
        }
        /* the start of a path */
        cs->list0 = NULL;
        cs->list1 = NULL;
        cs->closed = 0;
        cs->vdir = 0;
        cs->last = (CsCross *)((uint8_t *)st + 0x5a0);
        cs->last->y = 0x7fffffff;
        if (cs->xmin == 0x7fffffff) {
            if (pts[0].x < pts[1].x) {
                cs->xmin = pts[0].x;
                cs->xmax = pts[1].x;
            } else {
                cs->xmin = pts[1].x;
                cs->xmax = pts[0].x;
            }
            if (pts[0].y < pts[1].y) {
                cs->ymin = pts[0].y;
                cs->ymax = pts[1].y;
            } else {
                cs->ymin = pts[1].y;
                cs->ymax = pts[0].y;
            }
        } else {
            if (pts[0].x < cs->xmin)
                cs->xmin = pts[0].x;
            else if (pts[0].x > cs->xmax)
                cs->xmax = pts[0].x;
            if (pts[0].y < cs->ymin)
                cs->ymin = pts[0].y;
            else if (pts[0].y > cs->ymax)
                cs->ymax = pts[0].y;
        }
        cs->hdir = 0;
        n--;
        cs->first = pts[0];
        cs->prev = pts[0];
        pts++;
    }
    small = ctx->minppem < 0x220000;
#ifdef CFF_CONTROL_CSPATHPOINTS
    small = !small; /* CONTROL: the extra records at the other sizes */
#endif
    if (close == 0) {
        if (!CFF_ORIG(BuildCrosses_fn, RVA_BuildCrosses)(ctx, pts, n, small, ctx->center))
            return 0;
        cs->newpath = 0;
        return 1;
    }
    if (pts[n - 1].x != cs->first.x || pts[n - 1].y != cs->first.y) {
        pts[n] = cs->first;
        n++;
    }
    if (n < 3 && cs->newpath != 0) {
        cs->npts = 0;
        cs->newpath = 1;
        return 1;
    }
    if (!CFF_ORIG(BuildCrosses_fn, RVA_BuildCrosses)(ctx, pts, n, small, ctx->center))
        return 0;
    ok = CFF_ORIG(FinishPath_fn, RVA_FinishPath)(ctx, cs->closed);
    return ok != 0;
}
CS_EXPORT(cff_CSPathPoints, cff_CSPathPoints_impl);

/* fixdiv (cff_math.c), inlined in BuildCrosses: the slope dx/dy */
static int32_t bc_fixdiv(int32_t a, int32_t b) {
    double x;
    if (b == 0)
        return (int32_t)(0x7fffffffu + ((uint32_t)a >> 31));
    x = (double)a / (double)b * 65536.0;
    x += (0.0 > x) ? -0.5 : 0.5;
    if (x >= 2147483647.0)
        return 0x7fffffff;
    if (-2147483648.0 >= x)
        return (int32_t)0x80000000;
    return (int32_t)x;
}

/* fixmul, inlined in BuildCrosses: the x offset of a crossing */
static int32_t bc_fixmul(int32_t a, int32_t b) {
    double x = (double)a * (double)b * (1.0 / 65536.0);
    if (x < 0.0) {
        x += -0.5;
        return x > -2147483648.0 ? (int32_t)x : (int32_t)0x80000000;
    }
#ifdef CFF_CONTROL_BUILDCROSSES
    x += 0.25; /* CONTROL: crossings rounded a quarter unit off */
#else
    x += 0.5;
#endif
    return 2147483647.0 > x ? (int32_t)x : 0x7fffffff;
}

/* a new crossing from the pool (NewCross inlined) */
static inline CsCross *bc_new(CsState *cs, int32_t x, int32_t y) {
    CsCross *p = cs->pool_cur, *c = p != cs->pool_end ? p : NULL;
    cs->pool_cur = p + 1;
    if (c) {
        c->x = x;
        c->y = y;
        c->flags = 0;
    }
    return c;
}

/* The segments from cs->prev through pts[0..n-1]: one crossing per pixel-centre row crossed.
 * small (ppem < 34) records an extra record before every segment; center keeps the slope
 * class of each crossing (OffsetCenterSlope). */
CFF_API int32_t cff_BuildCrosses_impl(CsCtx *ctx, CsPt *pts, int32_t n, int32_t small, int32_t center) {
    CsState *cs = ctx->cs;
    NewCross_fn new_xtra = CFF_ORIG(NewCross_fn, RVA_NewXtraCross);
    OffsetCenterSlope_fn slope_class = CFF_ORIG(OffsetCenterSlope_fn, RVA_OffsetCenterSlope);
    uint16_t vdir = cs->vdir, hdir = cs->hdir;
    CsCross *last = cs->last, *xc, *c;
    CsPt *prev = &cs->prev, *cur = pts;
    int32_t acc = 0;
    if ((uint16_t)prev->y == 0x8000)
        prev->y = wrap_add(prev->y, 1);
    for (;;) {
        int32_t px, py, cx, cy, slope, r, x;
        n = wrap_sub(n, 1);
        if (n < 0)
            break;
        cy = cur->y;
        if ((uint16_t)cy == 0x8000) {
            cy = wrap_add(cy, 1);
            cur->y = cy;
        }
        px = prev->x;
        py = prev->y;
        cx = cur->x;
        if (px < cs->xmin)
            cs->xmin = px;
        if (px > cs->xmax)
            cs->xmax = px;
        if (py < cs->ymin)
            cs->ymin = py;
        if (py > cs->ymax)
            cs->ymax = py;
        slope = bc_fixdiv(wrap_sub(cx, px), wrap_sub(cy, py));
        xc = NULL;
        if (small) {
            xc = new_xtra(cs, px, py);
            if (!xc)
                return 0;
        }
        /* horizontal turns: a record joins list0 or list1 */
        if (px > cx) {
            if (hdir != 1) {
                if (hdir != 0) {
                    if (!xc && !(xc = new_xtra(cs, px, py)))
                        return 0;
                    xc->next = cs->list0;
                    cs->list0 = xc;
                }
                hdir = 1;
            }
        } else if (px == cx) {
            if (hdir != 3) {
                if (hdir != 0) {
                    if (!xc && !(xc = new_xtra(cs, px, py)))
                        return 0;
                    if (hdir == 1) {
                        xc->next = cs->list1;
                        cs->list1 = xc;
                    } else {
                        xc->next = cs->list0;
                        cs->list0 = xc;
                    }
                }
                hdir = 3;
            }
        } else {
            if (hdir != 2) {
                if (hdir != 0) {
                    if (!xc && !(xc = new_xtra(cs, px, py)))
                        return 0;
                    xc->next = cs->list1;
                    cs->list1 = xc;
                }
                hdir = 2;
            }
        }
        /* vertical turns get an extra record too (not linked); then the crossings */
        if (py > cy) {
            if (vdir != 5 && !xc && !new_xtra(cs, px, py))
                return 0;
            vdir = 5;
            r = (int32_t)(((uint32_t)py - 0x8000u) & 0xffff8000u) | 0x8000;
            if (r < cy)
                goto next;
            x = wrap_add(bc_fixmul(wrap_sub(r, py), slope), px);
            c = bc_new(cs, x, r);
            if (!c)
                return 0;
            if (center)
                c->flags = (uint16_t)((c->flags & 0xff0f) | (uint16_t)(slope_class(slope) << 4));
            acc = 1;
            if (c->y >= last->y) {
                if (c->y > last->y) {
                    c->flags = (uint16_t)((c->flags & 0xfffe) | CS_UP);
                    last->flags |= CS_LINKED;
                } else if (c->x >= last->x) {
                    last->flags |= CS_SAMEY_RIGHT;
                } else {
                    c->flags = (uint16_t)((c->flags & 0xfffb) | CS_SAMEY_LEFT);
                }
            } else {
                last->flags = (uint16_t)((last->flags & 0xfffd) | CS_DOWN);
                c->flags |= CS_LINKED;
            }
            last = c;
            for (;;) {
                r = wrap_sub(r, 0x10000);
                if (r < cy)
                    break;
                x = wrap_sub(x, slope);
                c = bc_new(cs, x, r);
                if (!c)
                    return 0;
                if (center)
                    c->flags = (uint16_t)((c->flags & 0xff0f) | (uint16_t)(slope_class(slope) << 4));
                last->flags = (uint16_t)((last->flags & 0xfffd) | CS_DOWN);
                c->flags |= CS_LINKED;
                last = c;
            }
        } else if (py == cy) {
            if (vdir != 6 && !xc && !new_xtra(cs, px, py))
                return 0;
            vdir = 6;
        } else {
            uint16_t f;
            if (vdir != 4 && !xc && !new_xtra(cs, px, py))
                return 0;
            vdir = 4;
            r = (int32_t)(((uint32_t)py + 0x8000u) & 0xffff8000u) | 0x8000;
            if (r > cy)
                goto next;
            x = wrap_add(bc_fixmul(wrap_sub(r, py), slope), px);
            c = bc_new(cs, x, r);
            if (!c)
                return 0;
            f = 0;
            if (center) {
                f = (uint16_t)(slope_class(slope) << 4);
                c->flags = f;
            }
            acc = 1;
            if (r > last->y) {
                c->flags = (uint16_t)(f | CS_UP);
                last->flags |= CS_LINKED;
            } else if (r < last->y) {
                last->flags = (uint16_t)((last->flags & 0xfffd) | CS_DOWN);
                c->flags |= CS_LINKED;
            } else if (x >= last->x) {
                last->flags |= CS_SAMEY_RIGHT;
            } else {
                c->flags = (uint16_t)(f | CS_SAMEY_LEFT);
            }
            last = c;
            for (;;) {
                r = wrap_add(r, 0x10000);
                if (r > cy)
                    break;
                x = wrap_add(x, slope);
                c = bc_new(cs, x, r);
                if (!c)
                    return 0;
                if (center)
                    c->flags = (uint16_t)((c->flags & 0xff0f) | (uint16_t)(slope_class(slope) << 4));
                c->flags &= 0xfffe;
                c->flags |= CS_UP;
                last->flags |= CS_LINKED;
                last = c;
            }
        }
    next:
        prev = cur;
        cur++;
    }
    cs->closed |= acc;
    cs->vdir = vdir;
    cs->hdir = hdir;
    cs->last = last;
    cs->prev = *prev;
    return 1;
}
CS_EXPORT(cff_BuildCrosses, cff_BuildCrosses_impl);

/* the end of a path: link records join its two ends; a closed path's turns are kept */
CFF_API int32_t cff_FinishPath_impl(CsCtx *ctx, int32_t closed) {
    CsState *cs = ctx->cs;
    NewLinkCross_fn new_link = CFF_ORIG(NewLinkCross_fn, RVA_NewLinkCross);
    CsCross *lastc = cs->pool_cur - 1, *l, *first, *nx, *t;
    if (lastc != cs->link) {
        cs->link->y = (int32_t)((uint32_t)(uintptr_t)lastc - (uint32_t)(uintptr_t)cs->link);
        l = new_link(cs);
        if (!l)
            return 0;
        l->x = (int32_t)((uint32_t)(uintptr_t)cs->link - (uint32_t)(uintptr_t)l + 0x18u);
        cs->prev_link = cs->link;
        l = new_link(cs);
        cs->link = l;
        if (!l)
            return 0;
        cs->prev_link->x = (int32_t)((uint32_t)(uintptr_t)l - (uint32_t)(uintptr_t)cs->prev_link + 0x18u);
        if (lastc->x < cs->xmin)
            cs->xmin = lastc->x;
        if (lastc->x > cs->xmax)
            cs->xmax = lastc->x;
        if (lastc->y < cs->ymin)
            cs->ymin = lastc->y;
        if (lastc->y > cs->ymax)
            cs->ymax = lastc->y;
        if (closed != 0) {
            if (lastc->flags & CS_EXTRA)
                lastc = CFF_ORIG(PathCross_fn, RVA_BackPathCross)(lastc);
#ifndef CFF_CONTROL_FINISHPATH /* CONTROL: the closing flags not fixed */
            CFF_ORIG(FixCrossFlags_fn, RVA_FixCrossFlags)(lastc,
                                                          CFF_ORIG(PathCross_fn, RVA_ForwPathCross)(lastc));
#endif
            first = cs->prev_link + 1;
            nx = first + 1;
            if (nx->flags & CS_JUMP)
                nx = CROSS_AT(nx, nx->x);
            if (first->x > nx->x) {
                if (cs->hdir != 1)
                    goto to_list0;
            } else if (first->x == nx->x) {
                if (cs->hdir != 3) {
                    if (cs->hdir == 1)
                        goto to_list1;
                    goto to_list0;
                }
            } else if (cs->hdir != 2) {
            to_list1:
                first->next = cs->list1;
                cs->list1 = first;
            }
            if (0) {
            to_list0:
                first->next = cs->list0;
                cs->list0 = first;
            }
            if (cs->list1) {
                for (t = cs->list1; t->next; t = t->next)
                    ;
                t->next = cs->list6;
                cs->list6 = cs->list1;
            }
            if (cs->list0) {
                for (t = cs->list0; t->next; t = t->next)
                    ;
                t->next = cs->list7;
                cs->list7 = cs->list0;
            }
        }
    }
    cs->npts = 0;
    cs->newpath = 1;
    return 1;
}
CS_EXPORT(cff_FinishPath, cff_FinishPath_impl);

/* ------------------------------------------------------------------------------------------
 * runs */

/* the row lists: cs->rows over the pool after the crossings, then every crossing sorted in */
CFF_API void cff_AddYCrosses_impl(CsState *cs, CsCross *end) {
    CsCross *c, **pp, *p;
    for (c = cs->pool_start; c != end; c++) {
        if (c->flags & CS_EXTRA)
            continue;
        pp = &ROW_HEAD(cs, ROW_OF(c));
#ifdef CFF_CONTROL_ADDYCROSSES
        c->x = wrap_add(c->x, 0x4000); /* CONTROL: every crossing a quarter pixel right */
#endif
        for (p = *pp; p && p->x <= c->x; p = p->next)
            pp = &p->next;
        *pp = c;
        c->next = p;
    }
}
CS_EXPORT(cff_AddYCrosses, cff_AddYCrosses_impl);

CFF_API int32_t cff_BuildYCross_impl(CsState *cs, int32_t *any) {
    int32_t ymax = cs->ymax, need, avail;
    int64_t r12, size;
    int16_t di, si;
    CsCross *base;
    uint64_t span;
    if (ymax == (int32_t)0x80000000) {
        *any = 0;
        return 0;
    }
    r12 = (int16_t)((uint32_t)cs->ymin >> 16);
    base = cs->pool_cur;
    di = (int16_t)(uint16_t)((uint32_t)(ymax >> 16) + 3u);
    si = (int16_t)(uint16_t)(r12 - 3);
    span = (uint64_t)(int64_t)(int32_t)((uint32_t)((int32_t)di - (int32_t)si) * 8u + 8u);
    need = (int32_t)(uint32_t)(span / 24) + 1;
    avail = (int32_t)(uint32_t)((uint64_t)(((intptr_t)cs->pool_end - (intptr_t)base) >> 3) * 0xaaaaaaaaaaaaaaabull);
    if (avail < need)
        return (int32_t)0xfc0a024a;
    size = (int64_t)need * 24;
    cs->rows = (CsCross **)base;
    memset(base, 0, (size_t)size);
    cs->pool_cur = (CsCross *)((uintptr_t)cs->pool_cur + (uintptr_t)size);
    cs->rows = (CsCross **)((uintptr_t)cs->rows + (uintptr_t)(-(int64_t)si * 8));
    CFF_ORIG(AddYCrosses_fn, RVA_AddYCrosses)(cs, base);
    for (;;) {
        di = (int16_t)(di - 1);
        cs->bottom = di;
        if (ROW_HEAD(cs, di))
            break;
        if (di == si) {
            *any = 0;
            return 0;
        }
    }
    si = (int16_t)(si + 1);
    cs->top = si;
    if (!ROW_HEAD(cs, r12 - 2)) {
        int64_t old;
        do {
            old = si;
            si = (int16_t)(si + 1);
            cs->top = si;
        } while (!ROW_HEAD(cs, old + 1));
    }
#ifdef CFF_CONTROL_BUILDYCROSS
    if (cs->bottom - cs->top >= 30) {
        *any = 0; /* CONTROL: glyphs of 30 rows or more come out empty */
        return 0;
    }
#endif
    *any = 1;
    return 0;
}
CS_EXPORT(cff_BuildYCross, cff_BuildYCross_impl);

/* .data 0x180231be0: the "center" offsets per slope class, 16.16 */
static const int32_t center_offsets[6] = {0, -4096, -19195, -23988, -28454, -32768};

/* Pairs the crossings of each row into runs and rounds them to pixel columns (+0x12):
 * cs->left/right become the column range. Returns the number of crossings the winding rule
 * dropped. */
CFF_API int32_t cff_BuildInitialRuns_impl(CsCtx *ctx, int32_t center) {
    CsState *cs = ctx->cs;
    int32_t wind = ctx->wind, minleft = 0x7fff, maxright = (int32_t)0xffff8000, dropped = 0;
    int32_t off[16] = {0};
    CsCross **row = (CsCross **)((uintptr_t)cs->rows + (uintptr_t)((intptr_t)cs->top * 8));
    int32_t left_pix, right_pix = 0, k;
    if (center != 0) {
        memcpy(off, center_offsets, sizeof center_offsets);
        if (ctx->ppem2e0 > 0x10000) {
            fixmul_fn fixmul = CFF_ORIG(fixmul_fn, RVA_fixmul);
            int32_t scale = wrap_sub(wrap_add(ctx->ppem2e0, ctx->ppem2e0), 0x10000);
            for (int i = 1; i < 6; i++) {
                int32_t v = fixmul(off[i], scale);
                off[i] = v < (int32_t)0xffff0000 ? (int32_t)0xffff0000 : v;
            }
        }
    }
    for (k = (int32_t)cs->bottom - (int32_t)cs->top; k >= 0; k--, row++) {
        CsCross *a = *row, *b;
        int32_t lastx;
        if (!a)
            continue;
        lastx = (int32_t)0x80000000;
        do {
            a->flags |= CS_ROWSTART;
            if (wind != 0) {
                CrossWindCount_fn wc = CFF_ORIG(CrossWindCount_fn, RVA_CrossWindCount);
                int32_t w = wc(NULL, a);
                b = a->next;
                while (b) {
                    CsCross *after = b->next;
                    w += wc(NULL, b);
                    if (w == 0)
                        break;
                    b->next = NULL;
                    b->flags |= CS_WOUND;
                    dropped++;
                    b->pix = 0;
                    b = after;
                }
                a->next = b;
            } else {
                b = a->next;
            }
            if (center != 0) {
                int32_t oa = off[(a->flags >> 4) & 0xf], ob;
                a->x = wrap_sub(a->x, oa);
                ob = off[(b->flags >> 4) & 0xf];
                b->x = wrap_add(b->x, ob);
                if (a->x >= b->x) {
                    int32_t m = wrap_add(wrap_add(wrap_sub(a->x, ob), b->x), oa) >> 1;
                    a->x = m;
                    b->x = wrap_add(m, 1);
                }
            }
            if (a->x <= lastx) {
                a->x = wrap_add(lastx, 1);
                if (b->x <= a->x)
                    b->x = wrap_add(a->x, 1);
            }
#ifdef CFF_CONTROL_BUILDINITIALRUNS
            left_pix = wrap_add(a->x, 0x7000) >> 16; /* CONTROL: left edges rounded off */
#else
            left_pix = wrap_add(a->x, 0x8000) >> 16;
#endif
            right_pix = wrap_add(b->x, 0x8000) >> 16;
            if ((int16_t)left_pix == (int16_t)right_pix) {
                left_pix = wrap_add(b->x, a->x) >> 17;
                right_pix = wrap_add(1, left_pix);
            }
            a->pix = (int16_t)left_pix;
            b->pix = (int16_t)right_pix;
            if (b->x > lastx)
                lastx = b->x;
            a = b->next;
        } while (a);
        if ((int16_t)right_pix > maxright)
            maxright = (int16_t)right_pix;
        if ((*row)->pix < minleft)
            minleft = (*row)->pix;
    }
    cs->left = (int16_t)minleft;
    cs->right = (int16_t)(maxright - 1);
    return dropped;
}
CS_EXPORT(cff_BuildInitialRuns, cff_BuildInitialRuns_impl);

/* the runs of rows top..bottom-1 into rr (count, then pairs), merging overlapping runs */
CFF_API int32_t cff_ReturnBits_impl(CsState *cs, CsRunRec *rr, CsMemRec *mem, void *growctx) {
    uint32_t need;
    int16_t *out, *start;
    int32_t y;
    rr->q18 = 0;
    rr->top = cs->top;
    rr->bottom = cs->bottom;
    rr->left = cs->left;
    rr->right = cs->right;
    need = (uint32_t)((uint64_t)(((intptr_t)cs->pool_cur - (intptr_t)cs->pool_start) >> 3) * 0xaaaaaaaaaaaaaaabull);
    need = need - (uint32_t)(int32_t)cs->top + (uint32_t)(int32_t)cs->bottom;
    need = need * 2 + 4;
    if ((int32_t)need <= 0)
        return (int32_t)0xfb0a0c96;
    if (rr->cap < need) {
        if (!mem->grow || !mem->grow(need - rr->cap, rr, 0, growctx)) {
            rr->count = 0;
            return (int32_t)0xfb0a0ca1;
        }
    }
    out = rr->data;
    for (y = cs->top; y < cs->bottom; y++) {
        CsCross *c = ROW_HEAD(cs, y);
        int16_t *slot = out;
        int32_t last;
        if (!c) {
            out++;
            *slot = 0;
            continue;
        }
        start = out;
        last = (int32_t)0xffff8000;
        out++;
        do {
            CsCross *b = c->next;
            int32_t l = c->pix, r = b->pix;
            c = b->next;
            if (l >= r)
                continue;
            if (l > last) {
                out[0] = (int16_t)l;
                out[1] = (int16_t)r;
                out += 2;
                last = r;
                continue;
            }
            if (l < out[-2])
                out[-2] = (int16_t)l;
            if (out[-1] < r) {
                out[-1] = (int16_t)r;
                last = r;
            }
            if ((int64_t)(((intptr_t)out - (intptr_t)start) & ~(intptr_t)1) <= 8)
                continue;
            for (;;) {
                int16_t pl;
                if (out[-3] < l)
                    break;
                pl = out[-4];
                out[-3] = (int16_t)r;
                if (l < pl)
                    out[-4] = (int16_t)l;
                out -= 2;
                if ((int64_t)(((intptr_t)out - (intptr_t)start) & ~(intptr_t)1) <= 8)
                    break;
            }
        } while (c);
        *slot = (int16_t)(((intptr_t)out - (intptr_t)start - 2) >> 2);
    }
    rr->count = (int32_t)(((intptr_t)out - (intptr_t)rr->data) >> 1);
#ifdef CFF_CONTROL_RETURNBITS
    if (rr->count > 1 && rr->data[0] > 0)
        rr->data[2]++; /* CONTROL: the first run of the top row one pixel longer */
#endif
    return 0;
}
CS_EXPORT(cff_ReturnBits, cff_ReturnBits_impl);

/* the crossings into runs (rr), bbox the pixel box in 16.16 */
CFF_API int32_t cff_CScan_impl(CsCtx *ctx, CsRunRec *rr, int32_t *bbox, CsMemRec *mem, void *state, void *growctx) {
    CsState *cs = ctx->cs;
    int32_t ppem = ctx->minppem, any = 0, r;
    int16_t right, bottom;
    if (cs->xmin < (int32_t)0xe0c00000 || cs->xmax > 0x1f400000 || cs->ymin < (int32_t)0xe0c00000 ||
        cs->ymax > 0x1f400000)
        return (int32_t)0xfc0a0ce5;
    cs->pool_cur--;
    if (cs->prev_link)
        cs->prev_link->x = 0;
    r = CFF_ORIG(BuildYCross_fn, RVA_BuildYCross)(cs, &any);
    if (r != 0)
        return r;
    if (any == 0)
        goto empty;
    CFF_ORIG(BuildInitialRuns_fn, RVA_BuildInitialRuns)(ctx, ctx->center);
#ifndef CFF_CONTROL_CSCAN /* CONTROL: no dropout repair */
    if (!CFF_ORIG(EditBlackSpace_fn, RVA_EditBlackSpace)(cs))
        return (int32_t)0xfc0a0d0b;
#endif
    if (ctx->white != 0 && ctx->white2 != 0 && (uint32_t)ppem - 0x68001u <= 0xafffeu)
        CFF_ORIG(EditWhiteSpace_fn, RVA_EditWhiteSpace)(cs, state);
    right = (int16_t)(cs->right + 1);
    bottom = (int16_t)(cs->bottom + 1);
    cs->right = right;
    cs->bottom = bottom;
    if (cs->left >= right || cs->top >= bottom)
        goto empty;
    bbox[0] = shl16(cs->left);
    bbox[1] = shl16(cs->top);
    bbox[2] = shl16(cs->right);
    bbox[3] = shl16(cs->bottom);
    return CFF_ORIG(ReturnBits_fn, RVA_ReturnBits)(cs, rr, mem, growctx);
empty:
    bbox[0] = bbox[1] = bbox[2] = bbox[3] = 0;
    memset(rr, 0, 12); /* the box and the count */
    return 0;
}
CS_EXPORT(cff_CScan, cff_CScan_impl);

/* CScanFill's frame: rsp + 0x40 .. rbp + 0x07 */
typedef struct CsFillFrame {
    union {            /* rsp + 0x40, the first 0x20 bytes of a run record */
        uint8_t bytes[0x20];
        uint64_t align;
    } copy;
    CsGrowCtx growctx; /* rbp - 0x71 */
    CsRunRec tmp;      /* rbp - 0x59 */
    CsMemRec mem;      /* rbp - 0x29 */
} CsFillFrame;
_Static_assert(offsetof(CsFillFrame, growctx) == 0x20, "CsFillFrame");

/* endchar: the runs of the glyph into rr; when rr already holds runs (a seac base glyph), the
 * new runs are merged in, shifted by (int16)offset, offset >> 16 */
CFF_API int32_t cff_CScanFill_impl(void *state, CsRunRec *rr, void *io, uint32_t offset, CsMemRec *mem, CsCtx *ctx,
                                   void *growctx) {
    CsGrowBuf **bufs = AT(io, 0, CsGrowBuf **);
    CsGrowBuf *rb2 = bufs[2], *rb0 = bufs[0];
    SetGSMatrix_fn setgs = CFF_ORIG(SetGSMatrix_fn, RVA_SetGSMatrix);
    CScan_fn cscan = CFF_ORIG(CScan_fn, RVA_CScan);
    int32_t *bbox = AT(io, 0x10, int32_t *);
    uint32_t need;
    int32_t r;
    CsFillFrame f;
    memset(&f, 0, sizeof f);
    if (AT(state, 0x2bc, uint32_t) & 0x10000)
        setgs((uint8_t *)state + 8, 1, state);
    if (rr->left == rr->right || rr->top == rr->bottom) {
        r = cscan(ctx, rr, bbox, mem, state, growctx);
#ifdef CFF_CONTROL_CSCANFILL
        if (r == 0 && rr->right > rr->left)
            rr->top++; /* CONTROL: the top row dropped */
#endif
        if (AT(state, 0x2bc, uint32_t) & 0x10000)
            setgs((uint8_t *)state + 8, 0, state);
        return r;
    }
    f.tmp.data = (int16_t *)rb2->ptr;
    f.tmp.cap = rb2->size;
    f.mem = *mem;
    f.mem.grow = CFF_ORIG(int (*)(uint32_t, CsRunRec *, int, void *), RVA_GrowRunBuff);
    f.growctx.p00 = growctx;
    f.growctx.mem = mem;
    f.growctx.runbuf = rb2;
    r = cscan(ctx, &f.tmp, bbox, &f.mem, state, &f.growctx);
    if (((uint32_t)r & 0xff000000u) == 0xfb000000u)
        r = (int32_t)0xfc141689;
    if (AT(state, 0x2bc, uint32_t) & 0x10000)
        setgs((uint8_t *)state + 8, 0, state);
    if (r != 0)
        return r;
    need = (uint32_t)rr->count * 2;
    if (need > rb0->size) {
        CsZone *z = mem->zone;
        if (rb0->ptr)
            z->procs->free(z, rb0->ptr);
        z = mem->zone;
        rb0->ptr = z->procs->alloc(z, need);
        if (!rb0->ptr) {
            rb0->size = 0;
            return (int32_t)0xfc14169b;
        }
        rb0->size = need;
    }
    memcpy(f.copy.bytes, rr, 0x20);
    memcpy(f.copy.bytes + offsetof(CsRunRec, data), &rb0->ptr, sizeof(void *));
    if (need != 0)
        memmove(rb0->ptr, rr->data, need);
    return CFF_ORIG(MergeRuns_fn, RVA_MergeRuns)(&f.tmp, (CsRunRec *)f.copy.bytes, (int16_t)offset, (int16_t)(offset >> 16), rr, mem,
                                                 growctx);
}
CS_EXPORT(cff_CScanFill, cff_CScanFill_impl);

/* ------------------------------------------------------------------------------------------
 * the bitmap */

/* runs the interpreter with the CScan procs; grows the crossing pool (by 0x6000 bytes, up to
 * 0x18000) and starts again while it runs out */
CFF_API int32_t cff_BuildRuns_impl(void *state, void *chardesc, void *io, CsRunRec *rr, CsMemRec *mem, void *growctx,
                                   uint32_t flags) {
    uint32_t sflags = AT(state, 0x2bc, uint32_t);
    int32_t p0 = AT(state, 0x2a8, int32_t), p1 = AT(state, 0x2ac, int32_t), r;
    CsGrowBuf **bufs = AT(io, 0, CsGrowBuf **);
    CsCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    if ((p1 > p0 ? p1 : p0) >= 0x1900000)
        return (int32_t)0xf9020073;
    ctx.minppem = p1 < p0 ? p1 : p0;
    ctx.white = (sflags >> 4) & 1;
    ctx.white2 = ((sflags >> 30) & 1) || (int32_t)sflags < 0;
    ctx.center = (sflags >> 6) & 1;
    ctx.wind = (AT(io, 0x18, uint32_t) >> 13) & 1;
#ifdef CFF_CONTROL_BUILDRUNS
    ctx.minppem = 0; /* CONTROL: every size handled as a small one */
#endif
    ctx.ppem2e0 = AT(state, 0x2e0, int32_t);
    if (!CFF_ORIG(IniCScan_fn, RVA_IniCScan)(&ctx.cs, bufs[0], bufs[5], mem->zone))
        return (int32_t)0xfc020086;
    ctx.st4 = AT(state, 4, int32_t);
    ctx.procs = ctx.procs5;
    ctx.self = &ctx;
    ctx.procs5[0] = idwr_orig(RVA_ResetCScan);
    ctx.procs5[1] = idwr_orig(RVA_CSNewPoint);
    ctx.procs5[2] = idwr_orig(RVA_CSNewLine);
    ctx.procs5[3] = idwr_orig(RVA_FixedFltn);
    ctx.procs5[4] = idwr_orig(RVA_CSClose);
    ctx.tail_ptr = ctx.tail;
    ctx.growctx = growctx;
    ctx.self2 = &ctx;
    ctx.z80 = NULL;
    for (;;) {
        CsGrowBuf *pool;
        uint32_t delta;
        ctx.flags = flags;
        memset(rr, 0, 12); /* the box and the count */
        r = CFF_ORIG(Type1InterpretCharString_fn, RVA_Type1InterpretCharString)(state, mem, chardesc, io, rr,
                                                                                 ctx.procs5, &ctx.flags);
        if (r == 0) {
            int32_t *box = AT(io, 0x10, int32_t *);
            box[0] = shl16(rr->left);
            box[2] = shl16(rr->right);
            box[1] = shl16(rr->top);
            box[3] = shl16(rr->bottom);
            rr->q20 = 0;
            return 0;
        }
        if (((uint32_t)r & 0xff000000u) != 0xfc000000u)
            return r;
        pool = bufs[0];
        delta = 0x6000;
        if (0x18000ull - pool->size < 0x6000ull) {
            delta = 0x18000u - pool->size;
            if ((int32_t)delta <= 0)
                return (int32_t)0xf90200c7;
        }
        r = CFF_ORIG(ATM_GrowBuff_fn, RVA_ATM_GrowBuff)(pool, (int32_t)delta, 0, mem);
        if (r != 0)
            return r;
    }
}
CS_EXPORT(cff_BuildRuns, cff_BuildRuns_impl);

/* ATMBuildBitMap's frame: rbp - 0x41 .. rbp + 0x37 */
typedef struct CsBitmapFrame {
    CsGrowCtx growctx; /* rbp - 0x41 */
    CsMemRec mem;      /* rbp - 0x29 */
    CsRunRec rr;       /* rbp + 0x07 */
} CsBitmapFrame;

/* the proc table's bitmap consumer: (ATM state, memory record, charstring, _t_CharIO, ...) */
CFF_API int32_t cff_ATMBuildBitMap_impl(void *state, CsMemRec *mem, void *chardesc, void *io, void *a5, uint32_t a6,
                                        void *a7) {
    CsBitmapFrame f;
    CsGrowBuf *runbuf = AT(io, 0, CsGrowBuf **)[3];
    uint32_t scale;
    int32_t r, rows;
    memset(&f, 0, sizeof f);
    f.mem.p00 = mem->p00;
    f.mem.grow = CFF_ORIG(int (*)(uint32_t, CsRunRec *, int, void *), RVA_GrowRunBuff);
    f.mem.p10 = idwr_orig(RVA_ATM_ManageCString);
    f.mem.v18 = mem->v18;
    f.mem.zone = mem->zone;
    f.mem.p28 = mem->p28;
    f.growctx.p00 = a7;
    f.growctx.mem = mem;
    f.growctx.runbuf = runbuf;
    f.rr.data = (int16_t *)runbuf->ptr;
    f.rr.cap = runbuf->size;
    r = CFF_ORIG(BuildRuns_fn, RVA_BuildRuns)(state, chardesc, io, &f.rr, &f.mem, &f.growctx, (a6 & 1) | 0x20);
    if (((uint32_t)r & 0xff000000u) == 0xfb000000u)
        return (int32_t)0xfc02040c;
    if (r != 0)
        return r;
    scale = AT(state, 0, uint16_t);
    if (scale != 1) {
        FFloorFModN_fn ffloor = CFF_ORIG(FFloorFModN_fn, RVA_FFloorFModN);
        int32_t *box = AT(io, 0x10, int32_t *);
        box[0] = ffloor(box[0], scale);
        box = AT(io, 0x10, int32_t *);
        box[1] = ffloor(box[1], scale);
        box = AT(io, 0x10, int32_t *);
        box[2] = ffloor(wrap_add(shl16((int32_t)scale), wrap_sub(box[2], 1)), scale);
        box = AT(io, 0x10, int32_t *);
        box[3] = ffloor(wrap_add(shl16((int32_t)scale), wrap_sub(box[3], 1)), scale);
    }
    scale = AT(state, 0, uint16_t);
    rows = wrap_add(wrap_sub(AT(state, 0x5d4, int32_t), 1), (int32_t)scale) / (int32_t)scale;
#ifdef CFF_CONTROL_ATMBUILDBITMAP
    if (f.rr.count > 2 && f.rr.data[0] > 0 && f.rr.data[2] - f.rr.data[1] >= 2)
        f.rr.data[1]++; /* CONTROL: the first run one pixel shorter */
#endif
    return CFF_ORIG(ConvertRuns_fn, RVA_ConvertRuns)(&f.rr, a5, scale, mem, rows);
}
CS_EXPORT(cff_ATMBuildBitMap, cff_ATMBuildBitMap_impl);

/* ------------------------------------------------------------------------------------------
 * MergeRuns (CScanFill, when the run buffer already holds runs): out = a OR (b shifted by
 * dx, dy). Rows are merged run by run: overlapping and touching-inside runs of both sides
 * become one run; the corpus never reaches it (checked against the original directly). */

#define MR_NONE_L ((int16_t)0x7fff)
#define MR_NONE_R ((int16_t)-0x8000)

/* one row of n pairs, copied (count first), each coordinate plus d */
static int16_t *mr_copy_row(int16_t *out, const int16_t **src, int16_t d) {
    const int16_t *p = *src;
    int16_t n = *p;
    *out = n;
    out++;
    p++;
    while (n > 0) {
        n--;
        out[0] = (int16_t)(d + p[0]);
        out[1] = (int16_t)(d + p[1]);
        out += 2;
        p += 2;
    }
    *src = p;
    return out;
}

CFF_API int32_t cff_MergeRuns_impl(CsRunRec *a, CsRunRec *b, int32_t dx32, int32_t dy32, CsRunRec *out, CsMemRec *mem,
                                   void *growctx) {
    int16_t dx = (int16_t)dx32, dy = (int16_t)dy32;
    int16_t btop, bbot, otop, obot, y;
    int32_t bspan, span, aspan, gap;
    uint32_t sum, need;
    const int16_t *pa, *pb;
    int16_t *o, *start;
    if (a->data == out->data)
        return (int32_t)0xfe070174;
    if (b->data == out->data)
        return (int32_t)0xfe070176;
    out->q18 = 0;
    if (b->left == b->right || b->top == b->bottom) {
        /* nothing to merge: a */
        int32_t bytes = (int32_t)((uint32_t)a->count * 2);
        if (bytes > (int32_t)out->cap) {
            if (!mem->grow || !mem->grow((uint32_t)bytes - out->cap, out, 0, growctx))
                return (int32_t)0xfb070181;
        }
        out->count = a->count;
        memcpy(out, a, 8);
        memmove(out->data, a->data, (size_t)((int64_t)a->count * 2));
        return 0;
    }
    if (a->left == a->right || a->top == a->bottom) {
        /* only b, shifted */
        uint32_t rows;
        if ((uint64_t)((int64_t)b->count * 2) > (uint64_t)out->cap) {
            if (!mem->grow || !mem->grow((uint32_t)b->count * 2 - out->cap, out, 0, growctx))
                return (int32_t)0xfb070190;
        }
        o = out->data;
        pb = b->data;
        out->count = b->count;
        memcpy(out, b, 8);
        out->left = (int16_t)(out->left + dx);
        out->right = (int16_t)(out->right + dx);
        out->top = (int16_t)(out->top + dy);
        out->bottom = (int16_t)(out->bottom + dy);
        if (b->top >= b->bottom)
            return 0;
        rows = (uint16_t)(b->bottom - b->top);
        do {
            uint16_t n = (uint16_t)*pb++;
            *o++ = (int16_t)n;
            while (n != 0) {
                o[0] = (int16_t)(dx + pb[0]);
                o[1] = (int16_t)(dx + pb[1]);
                o += 2;
                pb += 2;
                n--;
            }
        } while (--rows != 0);
        return 0;
    }
    /* the merged box */
    btop = (int16_t)(dy + b->top);
    bbot = (int16_t)(dy + b->bottom);
    out->left = (int32_t)a->left >= (int32_t)b->left + dx ? (int16_t)(b->left + dx) : a->left;
    out->right = (int32_t)a->right <= (int32_t)b->right + dx ? (int16_t)(b->right + dx) : a->right;
    otop = a->top >= btop ? btop : a->top;
    obot = a->bottom <= bbot ? bbot : a->bottom;
    out->top = otop;
    out->bottom = obot;
    sum = (uint32_t)b->count + (uint32_t)a->count;
    if ((uint32_t)a->count > sum)
        return (int32_t)0xfb0701ca;
    bspan = (int32_t)bbot - (int32_t)btop;
    span = (int32_t)obot - (int32_t)otop;
    aspan = (int32_t)a->bottom - (int32_t)a->top;
    if (aspan < 0 || bspan < 0 || aspan > span || bspan > span)
        return (int32_t)0xfb0701d5;
    gap = span - bspan - aspan; /* rows of neither: one count each */
    need = sum + (uint32_t)gap;
    if (gap > 0 && need < sum)
        return (int32_t)0xfb0701dc;
    if (need > need * 2)
        return (int32_t)0xfb0701e2;
    need *= 2;
    if (out->cap < need) {
        if (!mem->grow || !mem->grow(need - out->cap, out, 0, growctx))
            return (int32_t)0xfb0701ef;
    }
    start = o = out->data;
    pa = a->data;
    pb = b->data;
    y = otop;
    /* rows above a: b's (or empty) */
    for (; y < a->top; y++) {
        if (y >= bbot)
            *o++ = 0;
        else
            o = mr_copy_row(o, &pb, dx);
    }
    /* rows above b: a's (or empty) */
    for (; y < btop; y++) {
        if (y >= a->bottom)
            *o++ = 0;
        else
            o = mr_copy_row(o, &pa, 0);
    }
    /* rows of both */
    if (y < a->bottom) {
        int16_t aL = MR_NONE_L, aR = MR_NONE_R, bL = MR_NONE_L, bR = MR_NONE_R;
        while (y < bbot) {
            int32_t na = *pa++, nb = *pb++, aflag = 0, bflag = 0;
            int16_t *slot = o++, count = 0;
            if (na != 0 || nb != 0) {
                if (na != 0) {
                    aL = pa[0];
                    na--;
                    aR = pa[1];
                    pa += 2;
                    aflag = 1;
                }
                if (nb != 0) {
                    bL = (int16_t)(dx + pb[0]);
                    nb--;
                    bR = (int16_t)(dx + pb[1]);
                    pb += 2;
                    bflag = 1;
                } else {
                    bL = MR_NONE_L;
                    bR = MR_NONE_R;
                }
                while (aflag || bflag) {
                    int16_t runL, runR;
                    if (aL <= bL) {
                        runL = aL;
                        aflag = 0;
                        if (aR < bL) { /* a's run alone */
                            runR = aR;
                            if (na != 0) {
                                aL = pa[0];
                                na--;
                                aR = pa[1];
                                pa += 2;
                                aflag = 1;
                            } else {
                                aL = MR_NONE_L;
                                aR = MR_NONE_R;
                            }
                            goto emit;
                        }
                    } else {
                        runL = bL;
                        bflag = 0;
                        if (bR < aL) { /* b's run alone */
                            runR = bR;
                            if (nb != 0) {
                                nb--;
                                bL = (int16_t)(dx + pb[0]);
                                bflag = 1;
                                bR = (int16_t)(dx + pb[1]);
                                pb += 2;
                            } else {
                                bL = MR_NONE_L;
                                bR = MR_NONE_R;
                            }
                            goto emit;
                        }
                        aflag = 0;
                    }
                    /* the runs overlap: extend while a run of either side starts inside */
                    runR = aR > bR ? aR : bR;
                    bflag = aflag;
                    for (;;) {
                        int32_t ext = 0;
                        while (na != 0 && aR <= runR) {
                            aL = pa[0];
                            aflag = 1;
                            aR = pa[1];
                            pa += 2;
                            na--;
                        }
                        if (aflag && aR <= runR)
                            aflag = 0;
                        if (aL <= runR && runR < aR) {
                            ext = 1;
                            runR = aR;
                            aflag = 0;
                            if (na != 0) {
                                aL = pa[0];
                                na--;
                                aflag = 1;
                                aR = pa[1];
                                pa += 2;
                            }
                        }
                        while (nb != 0 && bR <= runR) {
                            bL = (int16_t)(dx + pb[0]);
                            bR = (int16_t)(dx + pb[1]);
                            bflag = 1;
                            pb += 2;
                            nb--;
                        }
                        if (bflag && bR <= runR)
                            bflag = 0;
                        if (bL <= runR && runR < bR) {
                            runR = bR;
                            bflag = 0;
                            if (nb != 0) {
                                bL = (int16_t)(dx + pb[0]);
                                nb--;
                                bflag = 1;
                                bR = (int16_t)(dx + pb[1]);
                                pb += 2;
                            }
                            continue;
                        }
                        if (!ext)
                            break;
                    }
                emit:
                    count++;
                    o[0] = runL;
                    o[1] = runR;
                    o += 2;
#ifdef CFF_CONTROL_MERGERUNS
                    o[-1]++; /* CONTROL: merged runs one pixel longer */
#endif
                    if (!aflag) {
                        aL = MR_NONE_L;
                        aR = MR_NONE_R;
                    }
                    if (!bflag) {
                        bL = MR_NONE_L;
                        bR = MR_NONE_R;
                    }
                }
                aL = MR_NONE_L;
                aR = MR_NONE_R;
            }
            *slot = count;
            y++;
            if (y >= a->bottom)
                break;
        }
    }
    /* the rows below: the side that goes on */
    if (a->bottom > bbot) {
        if (y < a->bottom) {
            uint32_t rows = (uint16_t)(a->bottom - y);
            do
                o = mr_copy_row(o, &pa, 0);
            while (--rows != 0);
        }
    } else if (y < bbot) {
        uint32_t rows = (uint16_t)(bbot - y);
        do
            o = mr_copy_row(o, &pb, dx);
        while (--rows != 0);
    }
    out->count = (int32_t)(((intptr_t)o - (intptr_t)start) >> 1);
    return 0;
}
CS_EXPORT(cff_MergeRuns, cff_MergeRuns_impl);
