/* CFF rasterizer, group "path" (notes/cff.md group 7): outline recording, path replay,
 * flattening and the bold simulation of outlines. Ported from the x64 assembly of
 * System32\DWrite.dll 10.0.26100.9711; RVAs relative to the image base 0x180000000.
 *
 *   ATMCharOutline       0x0b8da0  proc +0x5f0: interpret a glyph into the outline record, replay it
 *   BCParsePath          0x0b86b4  replays the outline record into client procs, keeps the box
 *   PlayOutline          0x0ebe14  replays the emboldened SB buffer into the client procs
 *   OutlineGetMemory     0x0b9280  next block of the outline record (0x1f48 bytes, at most 0x15e)
 *   OutlineFreeMemory    0x0b8d40  frees the record's blocks
 *   OutlineNewChar       0x0e6320  the outline procs Type1InterpretCharString calls ...
 *   OutlineMoveTo        0x0e4960
 *   OutlineCallMoveTo    0x0d7cc0  ... the deferred moveto, written before the next segment
 *   OutlineLineTo        0x0d7c40
 *   OutlineCurveTo       0x0d7b60
 *   OutlineClosePath     0x0d7b00
 *   OutlineEndChar       0x0e2c40
 *   OutlineSeac          0x1832a4  the accent offset of seac (no corpus glyph uses seac)
 *   SBNewChar ... SBMoveTo, SBLineTo, SBCurveTo, SBClosePath, SBEndChar
 *                        0x0ef180, 0x183480, 0x183410, 0x183350, 0x183310, 0x0ef1d0
 *   AddTo                0x0ef214  grows the SB buffer (state +0x5b8) by 0x2000 bytes
 *   AdobeBoldSimulation  0x0ecc28  emboldens the SB buffer in place
 *   initBline            0x10c548  bold line list helpers ...
 *   linkBline            0x0f224c
 *   findExtrema          0x0f2c6c
 *   dotProduct           0x10c900
 *   FixedFltn            0x0c71e0  curve flattening (CScan, interpreter)
 *   FixFltnCurveTo       0x0c7140  curve flattening into ATMQNewPoint (QReducer)
 *   InternalFixedFltn    0x0c7284  bisects until the curve fits 0x7f0000, depth <= 10
 *   FMiniFltn            0x0c74d4  16-bit (24.8 relative) subdivision, 6 levels
 *
 * Data layouts pinned down here (all offsets in bytes):
 *
 * The outline record (ATMCharOutline's frame, 0x60 bytes, the client data of the Outline*
 * procs): +0x00 the client data of AdobeInternalGetOutline (ATMCharOutline arg 7), +0x08 1 once
 * a line or curve was recorded, +0x10 the client procs (arg 5), +0x18 the memory record
 * (CFFFontInst + 0x610; its +0x28 points to an allocator object whose vtable has alloc at [0]
 * and free at [2]), +0x20 the first block, +0x28 the current block, +0x30/+0x38 the position
 * and block saved by the first NewChar (a second NewChar restarts there), +0x40 the write
 * pointer, +0x48 the end of the block, +0x50 the block count, +0x54 1 while a moveto is
 * pending, +0x58 its point. A block is a link (8 bytes) then int32 records:
 *   0x15 moveto  [+0xc,+0x10] point, 0x14 bytes     0x05 lineto  same layout, 0x14 bytes
 *   0x08 curveto [+4..+0x20] four points, 0x24      0x09 closepath, 0xc bytes
 *   0x0e endchar, 4 bytes                            0x10 continue in the next block
 *   0x60c seac   [+0xc,+0x10] the accent offset, 0x14 bytes (toggles the offset on and off)
 * BCParsePath also accepts 4 and 0x16 (moveto), 6 and 7 (lineto), 0x1e and 0x1f (curveto)
 * and 0x0f (endchar proc without ending the parse). Gaps in the records are never written.
 *
 * The record Type1InterpretCharString gets (arg 7, 0x20 bytes): flags ((arg6 & 1) | 0x200,
 * | 0x10 for the seac accent, (arg6 & 1) | 0x288 for the seac base), 0, a 0x38-byte zeroed
 * area (the interpreter leaves the seac offsets in its first two dwords; the seac path copies
 * io[1] to its +0x10), the client data, the outline record.
 *
 * The proc tables (BCParsePath, PlayOutline): newchar, moveto, lineto, curveto, closepath,
 * endchar. BCParsePath and PlayOutline call them as newchar(a, b), moveto(pt, a, b),
 * curveto(p0, p1, p2, p3, a, b), with (a, b) = (state, client data), or (state, state) for
 * the SB procs.
 *
 * The SB buffer (ATM state +0x5b8 base, +0x5c0 write pointer, +0x5c8 end): dword 0 its size
 * in bytes, then 1 newchar, 2 moveto (x, y), 3 lineto (x, y), 4 curveto (3 points, the first
 * control point is the current point), 5 closepath, 6 endchar.
 *
 * The bold line node (AdobeBoldSimulation, 0x50 bytes from malloc): +0x00 the point in the
 * SB buffer, +0x08 next, +0x10 the contour's first node (set by closepath), +0x18 the next
 * non-degenerate line, +0x20 the previous one (linkBline), +0x28 (x0, y0), +0x30 (x1, y1),
 * +0x38 offset start, +0x40 offset end, +0x48 the intersection with the previous line.
 *
 * The flattening record (InternalFixedFltn arg 6): +0x00 the point proc's client data,
 * +0x10 the tolerance (24.8 in its high bytes), +0x18 a proc record whose +0x10 is the point
 * proc, called as proc(&pt, arg5, client data).
 *
 * Memory: _o_malloc, _o_realloc and my_free are DWrite's own CRT imports in oracle mode (the
 * SB buffer is allocated by ATMCharOutline and grown by AddTo, which may be swapped one at a
 * time), the C library in the standalone build.
 *
 * Exports: every port is exported through a register-preserving thunk (PATH_EXPORT), as
 * CFF_EXPORT does, which in addition copies six stack arguments for the functions with more
 * than four (CFF_EXPORT's frame does not forward them). Calls to the group's own functions
 * go through CFF_ORIG, so each port can be swapped alone.
 *
 * Negative controls: -DCFF_CONTROL_<NAME> breaks <NAME> (see each function). */
#include "cff_port.h"
#ifdef IDWR_STANDALONE
#include <stdlib.h>
#endif

#ifdef IDWR_STANDALONE
#define cff_ATMCharOutline_impl cff_ATMCharOutline
#define cff_BCParsePath_impl cff_BCParsePath
#define cff_PlayOutline_impl cff_PlayOutline
#define cff_OutlineGetMemory_impl cff_OutlineGetMemory
#define cff_OutlineFreeMemory_impl cff_OutlineFreeMemory
#define cff_OutlineNewChar_impl cff_OutlineNewChar
#define cff_OutlineMoveTo_impl cff_OutlineMoveTo
#define cff_OutlineCallMoveTo_impl cff_OutlineCallMoveTo
#define cff_OutlineLineTo_impl cff_OutlineLineTo
#define cff_OutlineCurveTo_impl cff_OutlineCurveTo
#define cff_OutlineClosePath_impl cff_OutlineClosePath
#define cff_OutlineEndChar_impl cff_OutlineEndChar
#define cff_OutlineSeac_impl cff_OutlineSeac
#define cff_SBNewChar_impl cff_SBNewChar
#define cff_SBMoveTo_impl cff_SBMoveTo
#define cff_SBLineTo_impl cff_SBLineTo
#define cff_SBCurveTo_impl cff_SBCurveTo
#define cff_SBClosePath_impl cff_SBClosePath
#define cff_SBEndChar_impl cff_SBEndChar
#define cff_AddTo_impl cff_AddTo
#define cff_AdobeBoldSimulation_impl cff_AdobeBoldSimulation
#define cff_initBline_impl cff_initBline
#define cff_linkBline_impl cff_linkBline
#define cff_findExtrema_impl cff_findExtrema
#define cff_dotProduct_impl cff_dotProduct
#define cff_FixedFltn_impl cff_FixedFltn
#define cff_FixFltnCurveTo_impl cff_FixFltnCurveTo
#define cff_InternalFixedFltn_impl cff_InternalFixedFltn
#define cff_FMiniFltn_impl cff_FMiniFltn
#endif

/* ------------------------------------------------------------------------------------------
 * exports */

#ifdef IDWR_STANDALONE
#define PATH_EXPORT(name, impl) CFF_EXPORT(name, impl)
#define PATH_EXPORT_STACK(name, impl) CFF_EXPORT(name, impl)
#else
#define PATH_EXPORT(name, impl) CFF_EXPORT(name, impl)
/* CFF_EXPORT's thunk with a larger frame: the caller's stack arguments 5..10 are copied to
 * the thunk's outgoing area, so the port sees them at the usual places. Copying six
 * arguments reads at most 0x30 bytes of the caller's frame beyond the ones it passed. */
#define PATH_EXPORT_STACK(name, impl)                                                  \
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

/* ------------------------------------------------------------------------------------------
 * the group's entry points, called through the originals so each port swaps alone */

#define RVA_BCParsePath 0x0b86b4
#define RVA_OutlineFreeMemory 0x0b8d40
#define RVA_OutlineGetMemory 0x0b9280
#define RVA_FixedFltn 0x0c71e0
#define RVA_InternalFixedFltn 0x0c7284
#define RVA_FMiniFltn 0x0c74d4
#define RVA_OutlineClosePath 0x0d7b00
#define RVA_OutlineCurveTo 0x0d7b60
#define RVA_OutlineLineTo 0x0d7c40
#define RVA_OutlineCallMoveTo 0x0d7cc0
#define RVA_OutlineEndChar 0x0e2c40
#define RVA_OutlineMoveTo 0x0e4960
#define RVA_OutlineNewChar 0x0e6320
#define RVA_PlayOutline 0x0ebe14
#define RVA_AdobeBoldSimulation 0x0ecc28
#define RVA_SBNewChar 0x0ef180
#define RVA_SBEndChar 0x0ef1d0
#define RVA_AddTo 0x0ef214
#define RVA_linkBline 0x0f224c
#define RVA_findExtrema 0x0f2c6c
#define RVA_initBline 0x10c548
#define RVA_dotProduct 0x10c900
#define RVA_OutlineSeac 0x1832a4
#define RVA_SBClosePath 0x183310
#define RVA_SBCurveTo 0x183350
#define RVA_SBLineTo 0x183410
#define RVA_SBMoveTo 0x183480
/* other groups */
#define RVA_fixmul 0x0b3340
#define RVA_ATMQNewPoint 0x0b2920
#define RVA_Type1InterpretCharString 0x0b981c

/* ------------------------------------------------------------------------------------------
 * memory: DWrite's CRT in oracle mode */

#ifdef IDWR_STANDALONE
static void *crt_malloc(size_t n) { return malloc(n); }
static void *crt_realloc(void *p, size_t n) { return realloc(p, n); }
static void my_free(void *p) { free(p); }
#else
static void *crt_malloc(size_t n) { return (*(void *(**)(size_t))idwr_orig(0x1be3e0))(n); }  /* __imp__o_malloc */
static void *crt_realloc(void *p, size_t n) {
    return (*(void *(**)(void *, size_t))idwr_orig(0x1be400))(p, n);  /* __imp__o_realloc */
}
static void my_free(void *p) { CFF_ORIG(void (*)(void *), 0x08de18)(p); }  /* my_free: if (p) _o_free(p) */
#endif

/* ------------------------------------------------------------------------------------------
 * types */

typedef int32_t (*Proc2)(void *a, void *b);
typedef int32_t (*ProcPt)(int32_t *pt, void *a, void *b);
typedef int32_t (*ProcCurve)(int32_t *p0, int32_t *p1, int32_t *p2, int32_t *p3, void *a, void *b);

typedef struct PathProcs {
    Proc2 newchar;    /* +0x00 */
    ProcPt moveto;    /* +0x08 */
    ProcPt lineto;    /* +0x10 */
    ProcCurve curveto;/* +0x18 */
    Proc2 closepath;  /* +0x20 */
    Proc2 endchar;    /* +0x28 */
} PathProcs;

typedef struct OutlineRec {
    void *client;      /* +0x00 */
    int32_t drawn;     /* +0x08 */
    int32_t pad0c;
    void *procs;       /* +0x10 */
    uint8_t *mem;      /* +0x18 */
    uint8_t *first;    /* +0x20 */
    uint8_t *block;    /* +0x28 */
    uint8_t *mark;     /* +0x30 */
    uint8_t *markblk;  /* +0x38 */
    uint8_t *cur;      /* +0x40 */
    uint8_t *end;      /* +0x48 */
    int32_t nblocks;   /* +0x50 */
    int32_t pending;   /* +0x54 */
    int32_t mv[2];     /* +0x58 */
} OutlineRec;
_Static_assert(sizeof(OutlineRec) == 0x60, "OutlineRec");
_Static_assert(__builtin_offsetof(OutlineRec, cur) == 0x40, "OutlineRec.cur");
_Static_assert(__builtin_offsetof(OutlineRec, mv) == 0x58, "OutlineRec.mv");

#define BLOCK_SIZE 0x1f48

typedef struct BNode {
    int32_t *pt;          /* +0x00 */
    struct BNode *next;   /* +0x08 */
    struct BNode *close;  /* +0x10 */
    struct BNode *nextnd; /* +0x18 */
    struct BNode *prevnd; /* +0x20 */
    int32_t x0, y0;       /* +0x28 */
    int32_t x1, y1;       /* +0x30 */
    int32_t ox0, oy0;     /* +0x38 */
    int32_t ox1, oy1;     /* +0x40 */
    int32_t ix, iy;       /* +0x48 */
} BNode;
_Static_assert(sizeof(BNode) == 0x50, "BNode");

/* fields of the ATM font state (CFFFontInst + 0x9a0) */
static inline int32_t st_i32(const uint8_t *s, uint32_t off) { int32_t v; memcpy(&v, s + off, 4); return v; }
static inline void st_set_i32(uint8_t *s, uint32_t off, int32_t v) { memcpy(s + off, &v, 4); }
static inline uint8_t *st_ptr(const uint8_t *s, uint32_t off) { uint8_t *v; memcpy(&v, s + off, 8); return v; }
static inline void st_set_ptr(uint8_t *s, uint32_t off, const void *v) { memcpy(s + off, &v, 8); }
#define ST_SB_BASE 0x5b8
#define ST_SB_CUR 0x5c0
#define ST_SB_END 0x5c8

static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t neg32(int32_t a) { return (int32_t)(0u - (uint32_t)a); }

/* cvttsd2si r32: out of range and NaN give 0x80000000 */
static inline int32_t cvtt(double d) {
    if (d > -2147483649.0 && d < 2147483648.0) return (int32_t)d;
    return (int32_t)0x80000000;
}

/* the allocator object of the memory record: vtable [0] alloc(obj, size), [2] free(obj, p) */
typedef void *(*AllocFn)(void *obj, size_t size);
typedef void (*FreeFn)(void *obj, void *p);
static inline void *mem_obj(const OutlineRec *o) { void *obj; memcpy(&obj, o->mem + 0x28, 8); return obj; }
static inline void **obj_vtbl(void *obj) { void **v; memcpy(&v, obj, 8); return v; }

/* ------------------------------------------------------------------------------------------
 * the outline record */

/* OutlineGetMemory 0xb9280 (63 insns): the first block, the next one (linked by a 0x10
 * record), or the one after the current one when a NewChar restarted the recording.
 * Control: the first block starts with an extra figure. */
#if defined(CFF_CONTROL_OUTLINEGETMEMORY) || defined(CFF_CONTROL_OUTLINENEWCHAR)
/* a deliberate extra figure: moveto (0, 0), lineto (1, 1) px, closepath */
static uint8_t *control_figure(uint8_t *c) {
    int32_t r[] = {0x15, 0, 0, 0, 0, 5, 0, 0, 0x10000, 0x10000, 9, 0, 0};
    memcpy(c, r, sizeof r);
    return c + sizeof r;
}
#endif
CFF_API int32_t cff_OutlineGetMemory_impl(OutlineRec *o) {
    uint8_t *blk;
    if (!o->first) {
        void *obj = mem_obj(o);
        blk = ((AllocFn)obj_vtbl(obj)[0])(obj, BLOCK_SIZE);
        if (!blk) return 0;
        o->first = blk;
        o->block = blk;
        memset(blk, 0, 8);
        o->mark = NULL;
        o->markblk = NULL;
        o->nblocks = 1;
    } else {
        uint8_t *next;
        memcpy(&next, o->block, 8);
        if (!next) {
            if (o->nblocks >= 0x15e) return 0;
            o->nblocks++;
            void *obj = mem_obj(o);
            blk = ((AllocFn)obj_vtbl(obj)[0])(obj, BLOCK_SIZE);
            if (!blk) return 0;
            memcpy(o->block, &blk, 8);
            if ((uintptr_t)o->cur >= (uintptr_t)o->end) return 0;
            int32_t op = 0x10;
            memcpy(o->cur, &op, 4);
            o->block = blk;
            memset(blk, 0, 8);
        } else {
            int32_t op = 0x10;
            memcpy(o->cur, &op, 4);
            o->block = next;
            blk = next;
        }
    }
    o->cur = blk + 8;
#ifdef CFF_CONTROL_OUTLINEGETMEMORY
    if (blk == o->first) o->cur = control_figure(o->cur);
#endif
    o->end = blk + BLOCK_SIZE;
    return 1;
}
PATH_EXPORT(cff_OutlineGetMemory, cff_OutlineGetMemory_impl);

/* OutlineFreeMemory 0xb8d40 (23 insns). Control: also clears `drawn` (which ATMCharOutline
 * reads afterwards: the box is zeroed). */
CFF_API void cff_OutlineFreeMemory_impl(OutlineRec *o) {
    uint8_t *blk = o->first;
    while (blk) {
        uint8_t *next;
        memcpy(&next, blk, 8);
        void *obj = mem_obj(o);
        ((FreeFn)obj_vtbl(obj)[2])(obj, blk);
        blk = next;
    }
    o->block = NULL;
    o->first = NULL;
#ifdef CFF_CONTROL_OUTLINEFREEMEMORY
    o->drawn = 0;
#endif
}
PATH_EXPORT(cff_OutlineFreeMemory, cff_OutlineFreeMemory_impl);

static inline int32_t room(OutlineRec *o) {
    if ((uintptr_t)(o->cur + 0x28) < (uintptr_t)o->end) return 1;
    return CFF_ORIG(int32_t (*)(OutlineRec *), RVA_OutlineGetMemory)(o);
}
static inline void put32(uint8_t *p, uint32_t off, int32_t v) { memcpy(p + off, &v, 4); }

/* OutlineNewChar 0xe6320 (25 insns): the first call marks the position, later calls go back
 * to it. Control: the first call records an extra figure. */
CFF_API int32_t cff_OutlineNewChar_impl(void *unused, OutlineRec *o) {
    (void)unused;
    o->pending = 0;
    if (o->mark) {
        o->cur = o->mark;
        o->block = o->markblk;
        o->end = o->markblk + BLOCK_SIZE;
        return 1;
    }
#ifdef CFF_CONTROL_OUTLINENEWCHAR
    o->cur = control_figure(o->cur);
#endif
    o->mark = o->cur;
    o->markblk = o->block;
    return 1;
}
PATH_EXPORT(cff_OutlineNewChar, cff_OutlineNewChar_impl);

/* OutlineMoveTo 0xe4960 (12 insns): only remembers the point. Control: y + 1. */
CFF_API int32_t cff_OutlineMoveTo_impl(const int32_t *pt, void *unused, OutlineRec *o) {
    (void)unused;
    o->mv[0] = pt[0];
    o->mv[1] = pt[1];
#ifdef CFF_CONTROL_OUTLINEMOVETO
    o->mv[1] += 1;
#endif
    o->pending = 1;
    return 1;
}
PATH_EXPORT(cff_OutlineMoveTo, cff_OutlineMoveTo_impl);

/* OutlineCallMoveTo 0xd7cc0 (25 insns): writes the pending moveto. Control: x + 1. */
CFF_API int32_t cff_OutlineCallMoveTo_impl(OutlineRec *o) {
    o->pending = 0;
    if (!room(o)) return 0;
    uint8_t *c = o->cur;
    put32(c, 0, 0x15);
#ifdef CFF_CONTROL_OUTLINECALLMOVETO
    put32(c, 0xc, o->mv[0] + 1);
#else
    put32(c, 0xc, o->mv[0]);
#endif
    put32(c, 0x10, o->mv[1]);
    o->cur = c + 0x14;
    return 1;
}
PATH_EXPORT(cff_OutlineCallMoveTo, cff_OutlineCallMoveTo_impl);

/* OutlineLineTo 0xd7c40 (37 insns). Control: x + 1. */
CFF_API int32_t cff_OutlineLineTo_impl(const int32_t *pt, void *unused, OutlineRec *o) {
    (void)unused;
    if (o->pending == 1 && !CFF_ORIG(int32_t (*)(OutlineRec *), RVA_OutlineCallMoveTo)(o)) return 0;
    o->drawn = 1;
    if (!room(o)) return 0;
    uint8_t *c = o->cur;
    put32(c, 0, 5);
#ifdef CFF_CONTROL_OUTLINELINETO
    put32(c, 0xc, pt[0] + 1);
#else
    put32(c, 0xc, pt[0]);
#endif
    put32(c, 0x10, pt[1]);
    o->cur = c + 0x14;
    return 1;
}
PATH_EXPORT(cff_OutlineLineTo, cff_OutlineLineTo_impl);

/* OutlineCurveTo 0xd7b60 (59 insns). Control: the last y + 1. */
CFF_API int32_t cff_OutlineCurveTo_impl(const int32_t *p0, const int32_t *p1, const int32_t *p2, const int32_t *p3,
                                        void *unused, OutlineRec *o) {
    (void)unused;
    if (o->pending == 1 && !CFF_ORIG(int32_t (*)(OutlineRec *), RVA_OutlineCallMoveTo)(o)) return 0;
    o->drawn = 1;
    if (!room(o)) return 0;
    uint8_t *c = o->cur;
    put32(c, 0, 8);
    put32(c, 0x4, p0[0]);
    put32(c, 0x8, p0[1]);
    put32(c, 0xc, p1[0]);
    put32(c, 0x10, p1[1]);
    put32(c, 0x14, p2[0]);
    put32(c, 0x18, p2[1]);
    put32(c, 0x1c, p3[0]);
#ifdef CFF_CONTROL_OUTLINECURVETO
    put32(c, 0x20, p3[1] + 1);
#else
    put32(c, 0x20, p3[1]);
#endif
    o->cur = c + 0x24;
    return 1;
}
PATH_EXPORT_STACK(cff_OutlineCurveTo, cff_OutlineCurveTo_impl);

/* OutlineClosePath 0xd7b00 (29 insns). Control: records nothing. */
CFF_API int32_t cff_OutlineClosePath_impl(void *unused, OutlineRec *o) {
    (void)unused;
    if (o->pending == 1 && !CFF_ORIG(int32_t (*)(OutlineRec *), RVA_OutlineCallMoveTo)(o)) return 0;
    if (!room(o)) return 0;
#ifdef CFF_CONTROL_OUTLINECLOSEPATH
    return 1;
#endif
    put32(o->cur, 0, 9);
    o->cur += 0xc;
    return 1;
}
PATH_EXPORT(cff_OutlineClosePath, cff_OutlineClosePath_impl);

/* OutlineEndChar 0xe2c40 (20 insns). Control: a lineto (0, 0) before the endchar. */
CFF_API int32_t cff_OutlineEndChar_impl(void *unused, OutlineRec *o) {
    (void)unused;
    if (!room(o)) return 0;
#ifdef CFF_CONTROL_OUTLINEENDCHAR
    put32(o->cur, 0, 5);
    put32(o->cur, 0xc, 0);
    put32(o->cur, 0x10, 0);
    o->cur += 0x14;
#endif
    put32(o->cur, 0, 0xe);
    o->cur += 4;
    return 1;
}
PATH_EXPORT(cff_OutlineEndChar, cff_OutlineEndChar_impl);

/* OutlineSeac 0x1832a4 (29 insns): the accent offset, and forgets the NewChar mark.
 * Control: x + 1 (no corpus glyph reaches it). */
CFF_API int32_t cff_OutlineSeac_impl(const int32_t *pt, OutlineRec *o) {
    if (!room(o)) return 0;
    uint8_t *c = o->cur;
#ifdef CFF_CONTROL_OUTLINESEAC
    put32(c, 0xc, pt[0] + 1);
#else
    put32(c, 0xc, pt[0]);
#endif
    put32(c, 0x10, pt[1]);
    put32(c, 0, 0x60c);
    o->mark = NULL;
    o->markblk = NULL;
    o->cur = c + 0x14;
    return 1;
}
PATH_EXPORT(cff_OutlineSeac, cff_OutlineSeac_impl);

/* ------------------------------------------------------------------------------------------
 * BCParsePath 0xb86b4 (445 insns): replays the outline record into `procs` and stores the
 * box of the moveto points that start drawn segments and of every line and curve point
 * (xmin, ymin, xmax, ymax; all 0 when nothing was drawn). `swap` exchanges x and y.
 * Control: line end points are left out of the box. */
CFF_API int32_t cff_BCParsePath_impl(uint8_t *blk, void *unused2, const PathProcs *procs, int32_t *bbox, int32_t swap,
                                     int32_t unused6, void *a7, void *a8) {
    (void)unused2;
    (void)unused6;
    int32_t done = 0, empty = 1, started = 0, seac = 0, endcalled = 0;
    int32_t dx = 0, dy = 0;
    int32_t mv[2] = {0, 0};
    int32_t p0[2] = {0, 0}, p1[2] = {0, 0}, p2[2] = {0, 0}, p3[2] = {0, 0};
    int32_t xmax = 0, ymax = 0, xmin = 0, ymin = 0;
    int32_t i = 0;
#define BOX(x, y)                                                                                                    \
    do {                                                                                                             \
        int32_t bx_ = (x), by_ = (y);                                                                                \
        if (bx_ > xmax) xmax = bx_;                                                                                  \
        if (bx_ < xmin) xmin = bx_;                                                                                  \
        if (by_ > ymax) ymax = by_;                                                                                  \
        if (by_ < ymin) ymin = by_;                                                                                  \
    } while (0)
#define SEGMENT_START()                                                                                              \
    do {                                                                                                             \
        if (empty) {                                                                                                 \
            xmax = mv[0];                                                                                            \
            ymax = mv[1];                                                                                            \
            xmin = mv[0];                                                                                            \
            ymin = mv[1];                                                                                            \
        }                                                                                                            \
        empty = 0;                                                                                                   \
        if (!started) {                                                                                              \
            started = 1;                                                                                             \
            BOX(mv[0], mv[1]);                                                                                       \
        }                                                                                                            \
    } while (0)

    if (procs->newchar && !procs->newchar(a7, a8)) return (int32_t)0xfc060044;
    for (;;) {
        if (done) {
            bbox[0] = xmin;
            bbox[1] = ymin;
            bbox[2] = xmax;
            bbox[3] = ymax;
            if (empty) {
                bbox[1] = 0;
                bbox[0] = 0;
                bbox[3] = 0;
                bbox[2] = 0;
            }
            return 0;
        }
        const int32_t *e = (const int32_t *)(blk + 8 + (int64_t)i * 4);
        int32_t op = e[0];
        switch (op) {
        case 0x60c:
            if (seac) {
                seac = 0;
                dx = 0;
                dy = 0;
            } else {
                seac = 1;
                if (swap) {
                    dx = e[4];
                    dy = e[3];
                } else {
                    dx = e[3];
                    dy = e[4];
                }
            }
            i += 5;
            break;
        case 4:
        case 0x15:
        case 0x16:
            if (swap) {
                mv[0] = add32(e[4], dx);
                mv[1] = add32(e[3], dy);
            } else {
                mv[0] = add32(e[3], dx);
                mv[1] = add32(e[4], dy);
            }
            if (procs->moveto && !procs->moveto(mv, a7, a8)) return (int32_t)0xfc060087;
            started = 0;
            i += 5;
            break;
        case 5:
        case 6:
        case 7:
            if (swap) {
                p0[0] = add32(e[4], dx);
                p0[1] = add32(e[3], dy);
            } else {
                p0[0] = add32(e[3], dx);
                p0[1] = add32(e[4], dy);
            }
            if (procs->lineto && !procs->lineto(p0, a7, a8)) return (int32_t)0xfc0600a1;
            SEGMENT_START();
#ifndef CFF_CONTROL_BCPARSEPATH
            BOX(p0[0], p0[1]);
#endif
            i += 5;
            break;
        case 8:
        case 0x1e:
        case 0x1f:
            if (swap) {
                p0[0] = add32(e[2], dx);
                p0[1] = add32(e[1], dy);
                p1[0] = add32(e[4], dx);
                p1[1] = add32(e[3], dy);
                p2[0] = add32(e[6], dx);
                p2[1] = add32(e[5], dy);
                p3[0] = add32(e[8], dx);
                p3[1] = add32(e[7], dy);
            } else {
                p0[0] = add32(e[1], dx);
                p0[1] = add32(e[2], dy);
                p1[0] = add32(e[3], dx);
                p1[1] = add32(e[4], dy);
                p2[0] = add32(e[5], dx);
                p2[1] = add32(e[6], dy);
                p3[0] = add32(e[7], dx);
                p3[1] = add32(e[8], dy);
            }
            if (procs->curveto && !procs->curveto(p0, p1, p2, p3, a7, a8)) return (int32_t)0xfc0600e8;
            SEGMENT_START();
            BOX(p0[0], p0[1]);
            BOX(p1[0], p1[1]);
            BOX(p2[0], p2[1]);
            BOX(p3[0], p3[1]);
            i += 9;
            break;
        case 9:
            if (procs->closepath && !procs->closepath(a7, a8)) return (int32_t)0xfc06012c;
            started = 0;
            i += 3;
            break;
        case 0xe:
            if (!endcalled && procs->endchar && !procs->endchar(a7, a8)) return (int32_t)0xfc060156;
            if (!seac) done = 1;
            started = 0;
            i += 1;
            break;
        case 0xf:
            if (procs->endchar && !procs->endchar(a7, a8)) return (int32_t)0xfc06013d;
            started = 0;
            endcalled = 1;
            i += 1;
            break;
        case 0x10:
            memcpy(&blk, blk, 8);
            i = 0;
            if (!blk) return (int32_t)0xfe060163;
            break;
        default:
            return (int32_t)0xfe06016b;
        }
    }
#undef BOX
#undef SEGMENT_START
}
PATH_EXPORT_STACK(cff_BCParsePath, cff_BCParsePath_impl);

/* PlayOutline 0xebe14 (114 insns): replays the SB buffer into the client procs; returns 1
 * at the endchar that ends the buffer, 0 on a proc failure or an unknown record.
 * Control: the curve's first control point gets x + 1. */
CFF_API int32_t cff_PlayOutline_impl(const PathProcs *procs, uint8_t *state, void *client) {
    int32_t cp[2] = {0, 0}, c1[2] = {0, 0}, c2[2] = {0, 0}, c3[2] = {0, 0};
    const int32_t *r = (const int32_t *)(st_ptr(state, ST_SB_BASE) + 4);
    for (;;) {
        switch (r[0]) {
        case 1:
            if (procs->newchar && !procs->newchar(state, client)) return 0;
            r += 1;
            break;
        case 2:
        case 3: {
            ProcPt fn = r[0] == 2 ? procs->moveto : procs->lineto;
            cp[0] = r[1];
            cp[1] = r[2];
            r += 3;
            if (fn && !fn(cp, state, client)) return 0;
            break;
        }
        case 4:
            c1[0] = r[1];
            c1[1] = r[2];
#ifdef CFF_CONTROL_PLAYOUTLINE
            c1[0] += 1;
#endif
            c2[0] = r[3];
            c2[1] = r[4];
            c3[0] = r[5];
            c3[1] = r[6];
            r += 7;
            if (procs->curveto && !procs->curveto(cp, c1, c2, c3, state, client)) return 0;
            cp[0] = c3[0];
            cp[1] = c3[1];
            break;
        case 5:
            if (procs->closepath && !procs->closepath(state, client)) return 0;
            r += 1;
            break;
        case 6:
            if (procs->endchar && !procs->endchar(state, client)) return 0;
            r += 1;
            if ((uintptr_t)r >= (uintptr_t)st_ptr(state, ST_SB_CUR)) return 1;
            break;
        default:
            return 0;
        }
    }
}
PATH_EXPORT(cff_PlayOutline, cff_PlayOutline_impl);

/* ------------------------------------------------------------------------------------------
 * the SB buffer */

/* AddTo 0xef214 (43 insns): room for n more dwords; grows by 0x2000 bytes with _o_realloc,
 * frees the buffer when that fails. Control: fails for curves (7 dwords). */
CFF_API int32_t cff_AddTo_impl(uint8_t *state, int32_t n) {
#ifdef CFF_CONTROL_ADDTO
    if (n == 7) return 0;
#endif
    uint8_t *cur = st_ptr(state, ST_SB_CUR);
    if ((uintptr_t)(cur + (int64_t)n * 4) <= (uintptr_t)st_ptr(state, ST_SB_END)) return 1;
    uint8_t *old = st_ptr(state, ST_SB_BASE);
    int32_t size;
    memcpy(&size, old, 4);
    if ((int32_t)((uint32_t)size + 0x2000) >= size) {
        uint8_t *nb = crt_realloc(old, (size_t)((int64_t)size + 0x2000));
        old = st_ptr(state, ST_SB_BASE);
        if (nb) {
            st_set_ptr(state, ST_SB_BASE, nb);
            st_set_ptr(state, ST_SB_CUR, nb + (((int64_t)(st_ptr(state, ST_SB_CUR) - old)) >> 2) * 4);
            int32_t sz;
            memcpy(&sz, nb, 4);
            sz = add32(sz, 0x2000);
            memcpy(nb, &sz, 4);
            uint8_t *b = st_ptr(state, ST_SB_BASE);
            memcpy(&sz, b, 4);
            st_set_ptr(state, ST_SB_END, b + (((uint64_t)(int64_t)sz >> 2) - 1) * 4);
            return 1;
        }
    }
    my_free(old);
    st_set_ptr(state, ST_SB_BASE, NULL);
    return 0;
}
PATH_EXPORT(cff_AddTo, cff_AddTo_impl);

#define ADDTO(state, n) CFF_ORIG(int32_t (*)(uint8_t *, int32_t), RVA_AddTo)(state, n)

static inline void sb_put(uint8_t *state, uint32_t off, int32_t v) { memcpy(st_ptr(state, ST_SB_CUR) + off, &v, 4); }
static inline void sb_advance(uint8_t *state, uint32_t n) { st_set_ptr(state, ST_SB_CUR, st_ptr(state, ST_SB_CUR) + n); }

/* SBNewChar 0xef180. Control: records 6 (endchar). */
CFF_API int32_t cff_SBNewChar_impl(uint8_t *state, void *unused) {
    (void)unused;
    if (!ADDTO(state, 1)) return 0;
#ifdef CFF_CONTROL_SBNEWCHAR
    sb_put(state, 0, 6);
#else
    sb_put(state, 0, 1);
#endif
    sb_advance(state, 4);
    return 1;
}
PATH_EXPORT(cff_SBNewChar, cff_SBNewChar_impl);

/* SBEndChar 0xef1d0. Control: records nothing. */
CFF_API int32_t cff_SBEndChar_impl(uint8_t *state, void *unused) {
    (void)unused;
    if (!ADDTO(state, 1)) return 0;
#ifdef CFF_CONTROL_SBENDCHAR
    return 1;
#endif
    sb_put(state, 0, 6);
    sb_advance(state, 4);
    return 1;
}
PATH_EXPORT(cff_SBEndChar, cff_SBEndChar_impl);

/* SBClosePath 0x183310. Control: records nothing. */
CFF_API int32_t cff_SBClosePath_impl(uint8_t *state, void *unused) {
    (void)unused;
    if (!ADDTO(state, 1)) return 0;
#ifdef CFF_CONTROL_SBCLOSEPATH
    return 1;
#endif
    sb_put(state, 0, 5);
    sb_advance(state, 4);
    return 1;
}
PATH_EXPORT(cff_SBClosePath, cff_SBClosePath_impl);

/* SBMoveTo 0x183480. Control: y + 1. */
CFF_API int32_t cff_SBMoveTo_impl(const int32_t *pt, uint8_t *state, void *unused) {
    (void)unused;
    if (!ADDTO(state, 3)) return 0;
    sb_put(state, 0, 2);
    sb_put(state, 4, pt[0]);
#ifdef CFF_CONTROL_SBMOVETO
    sb_put(state, 8, pt[1] + 1);
#else
    sb_put(state, 8, pt[1]);
#endif
    sb_advance(state, 0xc);
    return 1;
}
PATH_EXPORT(cff_SBMoveTo, cff_SBMoveTo_impl);

/* SBLineTo 0x183410. Control: y + 1. */
CFF_API int32_t cff_SBLineTo_impl(const int32_t *pt, uint8_t *state, void *unused) {
    (void)unused;
    if (!ADDTO(state, 3)) return 0;
    sb_put(state, 0, 3);
    sb_put(state, 4, pt[0]);
#ifdef CFF_CONTROL_SBLINETO
    sb_put(state, 8, pt[1] + 1);
#else
    sb_put(state, 8, pt[1]);
#endif
    sb_advance(state, 0xc);
    return 1;
}
PATH_EXPORT(cff_SBLineTo, cff_SBLineTo_impl);

/* SBCurveTo 0x183350: the three points after the current one. Control: the end y + 1. */
CFF_API int32_t cff_SBCurveTo_impl(const int32_t *p0, const int32_t *p1, const int32_t *p2, const int32_t *p3,
                                   uint8_t *state, void *unused) {
    (void)p0;
    (void)unused;
    if (!ADDTO(state, 7)) return 0;
    sb_put(state, 0, 4);
    sb_put(state, 4, p1[0]);
    sb_put(state, 8, p1[1]);
    sb_put(state, 0xc, p2[0]);
    sb_put(state, 0x10, p2[1]);
    sb_put(state, 0x14, p3[0]);
#ifdef CFF_CONTROL_SBCURVETO
    sb_put(state, 0x18, p3[1] + 1);
#else
    sb_put(state, 0x18, p3[1]);
#endif
    sb_advance(state, 0x1c);
    return 1;
}
PATH_EXPORT_STACK(cff_SBCurveTo, cff_SBCurveTo_impl);

/* ------------------------------------------------------------------------------------------
 * the bold simulation of outlines */

/* initBline 0x10c548 (19 insns). Control: does not link the node. */
CFF_API void cff_initBline_impl(BNode *prev, BNode *node, int32_t *pt) {
    node->pt = pt;
    node->close = NULL;
    node->next = NULL;
    node->nextnd = NULL;
    node->prevnd = NULL;
#ifndef CFF_CONTROL_INITBLINE
    if (prev) prev->next = node;
#else
    (void)prev;
#endif
}
PATH_EXPORT(cff_initBline, cff_initBline_impl);

static inline int degenerate(const BNode *n) { return n->x0 == n->x1 && n->y0 == n->y1; }

/* linkBline 0xf224c (64 insns): links each line to the next non-degenerate line of its
 * contour (+0x18, the last one back to the first), and each degenerate line to the next
 * linked one after it (+0x20). Control: the last line of a contour is not linked back. */
CFF_API void cff_linkBline_impl(BNode *head) {
    BNode *first = NULL, *prev = NULL;
    if (!head) return;
    for (BNode *n = head; n; n = n->next) {
        if (!degenerate(n)) {
            if (prev) prev->nextnd = n;
            else first = n;
            prev = n;
        }
        if (n->close) {
#ifndef CFF_CONTROL_LINKBLINE
            if (prev && prev != first) prev->nextnd = first;
#endif
            prev = NULL;
        }
    }
    for (BNode *c = head; c; c = c->next) {
        if (!degenerate(c)) continue;
        BNode *a = c->close;
        if (!a) {
            a = c->next;
            if (!a) continue;
        }
        for (;;) {
            if (a == c) break;
            if (a->nextnd) {
                c->prevnd = a;
                break;
            }
            if (a->close) a = a->close;
            else {
                a = a->next;
                if (!a) break;
            }
        }
    }
}
PATH_EXPORT(cff_linkBline, cff_linkBline_impl);

/* findExtrema 0xf2c6c (38 insns): the extremes of the end points of non-degenerate lines.
 * Control: the extreme values are not updated, so the four nodes are the last line. */
CFF_API void cff_findExtrema_impl(BNode *n, int32_t *maxx, BNode **maxxn, int32_t *minx, BNode **minxn, int32_t *maxy,
                                  BNode **maxyn, int32_t *miny, BNode **minyn) {
    int32_t x = n->x1;
    if (n->x0 == x && n->y0 == n->y1) return;
#ifdef CFF_CONTROL_FINDEXTREMA
#define SET(v, x) (void)0
#else
#define SET(v, x) (*(v) = (x))
#endif
    if (x > *maxx) {
        SET(maxx, x);
        *maxxn = n;
    }
    if (x < *minx) {
        SET(minx, x);
        *minxn = n;
    }
    int32_t y = n->y1;
    if (y > *maxy) {
        SET(maxy, y);
        *maxyn = n;
    }
    if (y < *miny) {
        SET(miny, y);
        *minyn = n;
    }
#undef SET
}
PATH_EXPORT_STACK(cff_findExtrema, cff_findExtrema_impl);

/* dotProduct 0x10c900 (46 insns): the turn at the end of a line into the next one, 1 or -1
 * (1 when the lines do not meet). Control: inverted. */
CFF_API int32_t cff_dotProduct_impl(const BNode *n) {
    const BNode *m = n->nextnd;
    if (!m || n->x1 != m->x0 || n->y1 != m->y0) return 1;
    double a = (double)sub32(m->x1, m->x0) * (double)sub32(n->y0, n->y1);
    double b = (double)sub32(m->y1, m->y0) * (double)sub32(n->x0, n->x1);
    double d = a - b;
#ifdef CFF_CONTROL_DOTPRODUCT
    return d > 0.0 ? -1 : 1;
#endif
    return d > 0.0 ? 1 : -1;
}
PATH_EXPORT(cff_dotProduct, cff_dotProduct_impl);

typedef void (*FindExtremaFn)(BNode *, int32_t *, BNode **, int32_t *, BNode **, int32_t *, BNode **, int32_t *, BNode **);

/* AdobeBoldSimulation 0xecc28 (605 insns): emboldens the SB buffer in place. Each line of
 * the flattened path (curve control polygons included) is offset by (ox, oy) from the signs
 * of its direction, oriented by the turns at the four extremes; consecutive offset lines are
 * intersected (clamped to the offsets), and the points move to the intersections shifted by
 * (xneg, yneg). The amounts come from the ppem (state +0x2a8, +0x2ac) and the overscale mode
 * (+0x5d0); the advance in io[1] + 0x20 grows by xneg + xpos.
 * Control: the x offset of rightward lines is xpos + 1. */
CFF_API int32_t cff_AdobeBoldSimulation_impl(int32_t *bbox, uint8_t *state) {
    int32_t (*fixmul)(int32_t, int32_t) = CFF_ORIG(int32_t (*)(int32_t, int32_t), RVA_fixmul);
    void (*initBline)(BNode *, BNode *, int32_t *) = CFF_ORIG(void (*)(BNode *, BNode *, int32_t *), RVA_initBline);
    FindExtremaFn findExtrema = CFF_ORIG(FindExtremaFn, RVA_findExtrema);
    int32_t (*dotProduct)(BNode *) = CFF_ORIG(int32_t (*)(BNode *), RVA_dotProduct);
    const uint32_t mask = 0xffff0000u;
    int32_t maxx = (int32_t)0x80000000, maxy = (int32_t)0x80000000, minx = 0x7fffffff, miny = 0x7fffffff;
    BNode *maxxn = NULL, *maxyn = NULL, *minxn = NULL, *minyn = NULL;
    int32_t xpos, xneg, ypos, yneg; /* r14, r15, [rsp+0x58], r12 */
    int32_t mode = st_i32(state, 0x5d0);
    int32_t ppem = st_i32(state, 0x2a8);
    int32_t rppem = (int32_t)(((uint32_t)ppem + 0x8000) & mask);
    int general = 1;
    if ((uint32_t)mode - 1u <= 1u && rppem <= 0x12c0000) {
        yneg = ypos = xneg = 0;
        int32_t a = (int32_t)(((uint32_t)fixmul(sub32(ppem, 0x300000), 0xaaa) + 0x8000) & mask) + 0x30000;
        xpos = a >= 0x30000 ? a : 0x30000;
        if (xpos > 0x60000) xpos = 0x60000;
        general = 0;
    } else if (mode == 3 && rppem <= 0x1900000) {
        yneg = ypos = xneg = 0;
        int32_t a = (int32_t)(((uint32_t)fixmul(sub32(ppem, 0x400000), 0xaaa) + 0x8000) & mask) + 0x40000;
        xpos = a >= 0x40000 ? a : 0x40000;
        if (xpos > 0x80000) xpos = 0x80000;
        general = 0;
    } else if (mode == 4 && rppem <= 0xc80000) {
        yneg = ypos = xneg = 0;
        int32_t a = (int32_t)(((uint32_t)fixmul(sub32(ppem, 0x200000), 0xaaa) + 0x8000) & mask) + 0x20000;
        int32_t c = a >= 0x20000 ? a : 0x20000;
        if (c > 0x40000) c = 0x40000;
        xpos = c;
        general = 0;
    }
    if (general) {
        int32_t a = fixmul(rppem, 0x28f);
        int32_t ry = (int32_t)(((uint32_t)st_i32(state, 0x2ac) + 0x8000) & mask);
        xpos = (int32_t)(((uint32_t)a + 0x10000) & mask);
        xneg = (int32_t)(((uint32_t)a + 0x8000) & mask);
        int32_t b = fixmul(ry, 0x28f);
        yneg = (int32_t)((uint32_t)b & mask);
        ypos = (int32_t)(((uint32_t)b + 0x8000) & mask);
    }
    if ((st_i32(state, 0x2bc) & 0xc0000000u) == 0) {
        xneg = xpos = fixmul(ppem, 0x28f);
        yneg = ypos = fixmul(st_i32(state, 0x2ac), 0x28f);
    }

    int32_t *p = (int32_t *)st_ptr(state, ST_SB_BASE);
    BNode *head = crt_malloc(sizeof(BNode));
    if (!head) return 0;
    int32_t ok = 0;
    head->next = NULL;
    if (p[1] != 1 || p[2] != 2) {
        ok = 1;
        goto out;
    }
    p += 3;
    head->close = NULL;
    head->pt = p;
    head->nextnd = NULL;
    head->prevnd = NULL;
    BNode *cur = head, *start = head;
#define FIND(n) findExtrema(n, &maxx, &maxxn, &minx, &minxn, &maxy, &maxyn, &miny, &minyn)
    for (;;) {
        start->x0 = p[0];
        start->y0 = p[1];
        p += 2;
        for (;;) {
            if ((uintptr_t)p >= (uintptr_t)st_ptr(state, ST_SB_CUR)) goto out;
            int32_t op = *p++;
            if (op == 2) break;
            if (op == 3) {
                BNode *n = crt_malloc(sizeof(BNode));
                if (!n) goto out;
                initBline(cur, n, p);
                n->x0 = cur->x1 = p[0];
                n->y0 = cur->y1 = p[1];
                p += 2;
                FIND(cur);
                cur = n;
            } else if (op == 4) {
                BNode *n1 = crt_malloc(sizeof(BNode));
                if (!n1) goto out;
                initBline(cur, n1, p);
                n1->x0 = cur->x1 = p[0];
                n1->y0 = cur->y1 = p[1];
                FIND(cur);
                BNode *n2 = crt_malloc(sizeof(BNode));
                if (!n2) goto out;
                initBline(n1, n2, p + 2);
                n2->x0 = n1->x1 = p[2];
                n2->y0 = n1->y1 = p[3];
                FIND(n1);
                BNode *n3 = crt_malloc(sizeof(BNode));
                if (!n3) goto out;
                initBline(n2, n3, p + 4);
                n3->x0 = n2->x1 = p[4];
                n3->y0 = n2->y1 = p[5];
                p += 6;
                FIND(n2);
                cur = n3;
            } else if (op == 5) {
                cur->close = start;
                int32_t x = start->x0, y = start->y0;
                cur->x1 = x;
                cur->y1 = y;
                if (cur->x0 == x && cur->y0 == y) continue;
                if (x > maxx) {
                    maxx = x;
                    maxxn = cur;
                }
                if (x < minx) {
                    minx = x;
                    minxn = cur;
                }
                if (y > maxy) {
                    maxy = y;
                    maxyn = cur;
                }
                if (y < miny) {
                    miny = y;
                    minyn = cur;
                }
            } else if (op == 6) {
                if (!cur->close) goto out;
                if ((uintptr_t)p < (uintptr_t)st_ptr(state, ST_SB_CUR)) continue;
                goto finish;
            } else
                goto out;
        }
        /* moveto: a new contour */
        BNode *n = crt_malloc(sizeof(BNode));
        if (!n) goto out;
        initBline(cur, n, p);
        cur = start = n;
    }
#undef FIND

finish: {
    CFF_ORIG(void (*)(BNode *), RVA_linkBline)(head);
    uint32_t cnt = ~(uint32_t)dotProduct(maxxn) >> 31;
    cnt += dotProduct(maxyn) < 0 ? 0 : 1;
    cnt += dotProduct(minxn) < 0 ? 0 : 1;
    cnt += dotProduct(minyn) < 0 ? 0 : 1;
    for (BNode *n = head; n; n = n->next) {
        int32_t ox, oy;
        if (!n->nextnd) {
            oy = 0;
            ox = 0;
        } else {
            int32_t dy = cnt >= 2 ? sub32(n->y1, n->y0) : sub32(n->y0, n->y1);
            int32_t dx = cnt >= 2 ? sub32(n->x1, n->x0) : sub32(n->x0, n->x1);
#ifdef CFF_CONTROL_ADOBEBOLDSIMULATION
            int32_t xp = xpos + 1;
#else
            int32_t xp = xpos;
#endif
            ox = dy < 0 ? neg32(xneg) : (dy > 0 ? xp : 0);
            if (dx < 0) oy = ypos;
            else oy = dx > 0 ? neg32(yneg) : 0;
        }
        n->ox0 = add32(n->x0, ox);
        n->ox1 = add32(n->x1, ox);
        n->oy0 = add32(n->y0, oy);
        n->oy1 = add32(n->y1, oy);
    }
    for (BNode *a = head; a; a = a->next) {
        BNode *b = a->nextnd;
        if (!b) continue;
        int32_t ix = b->ox0, iy;
        if (a->ox1 == ix && a->oy1 == b->oy0) {
            b->ix = ix;
            b->iy = b->oy0;
            continue;
        }
        iy = b->oy0;
        double d1x = (double)a->ox1 - (double)a->ox0;
        double d1y = (double)a->oy1 - (double)a->oy0;
        double d2y = (double)b->oy1 - (double)iy;
        double d2x = (double)b->ox1 - (double)ix;
        double den = d1x * d2y - d1y * d2x;
        if (den != 0.0) {
            double t = ((double)a->oy0 - (double)iy) * d2x - ((double)a->ox0 - (double)ix) * d2y;
            t = t / den;
            iy = cvtt(d1y * t + (double)a->oy0);
            ix = cvtt(d1x * t + (double)a->ox0);
        }
        b->ix = ix;
        b->iy = iy;
        int32_t x0 = b->x0;
        if (sub32(ix, x0) > xpos) {
            ix = add32(x0, xpos);
            b->ix = ix;
        }
        if (sub32(ix, x0) < neg32(xneg)) b->ix = sub32(x0, xneg);
        int32_t y0 = b->y0;
        if (sub32(iy, y0) > ypos) {
            iy = add32(y0, ypos);
            b->iy = iy;
        }
        if (sub32(iy, y0) < neg32(yneg)) b->iy = sub32(y0, yneg);
    }
    for (BNode *n = head; n; n = n->next) {
        BNode *q = n->prevnd;
        if (q) {
            n->x0 = add32(q->ix, xneg);
            n->y0 = add32(q->iy, yneg);
        } else if (n->nextnd) {
            n->x0 = add32(n->ix, xneg);
            n->y0 = add32(n->iy, yneg);
        }
        if (n->x0 > bbox[2]) bbox[2] = n->x0;
        if (n->x0 < bbox[0]) bbox[0] = n->x0;
        if (n->y0 > bbox[3]) bbox[3] = n->y0;
        if (n->y0 < bbox[1]) bbox[1] = n->y0;
        n->pt[0] = n->x0;
        n->pt[1] = n->y0;
    }
    ok = 1;
}

out: {
    uint8_t *io1 = st_ptr(st_ptr(state, 0x358), 8);
    int32_t adv = st_i32(io1, 0x20);
    if (adv != 0) st_set_i32(io1, 0x20, add32(add32(adv, xneg), xpos));
    BNode *n = head;
    do {
        BNode *next = n->next;
        my_free(n);
        n = next;
    } while (n);
    return ok;
}
}
PATH_EXPORT(cff_AdobeBoldSimulation, cff_AdobeBoldSimulation_impl);

/* ------------------------------------------------------------------------------------------
 * ATMCharOutline 0xb8da0 (291 insns): proc +0x5f0 of CFFFontInst, for outlines and metrics.
 * Interprets the charstring into the outline record (the Outline* procs), then replays it
 * into the client procs with BCParsePath (which leaves the box in io[2]), or, for bold
 * (flags & 2), into the SB buffer, emboldens it and replays that with PlayOutline. */

typedef int32_t (*T1InterpretFn)(uint8_t *state, uint8_t *mem, void *cs, uint8_t *io, uint64_t zero,
                                 PathProcs *procs, uint32_t *rec);
typedef int32_t (*ParsePathFn)(uint8_t *, void *, const PathProcs *, int32_t *, int32_t, int32_t, void *, void *);

typedef struct ATMFrame {   /* ATMCharOutline's frame from rsp+0x40 */
    int32_t seac[2];        /* +0x00 rsp+0x40 */
    void *client_procs;     /* +0x08 */
    uint32_t rec_flags;     /* +0x10 rsp+0x50: the record of Type1InterpretCharString */
    uint32_t rec_zero;
    uint64_t *rec_box;
    void *rec_client;
    OutlineRec *rec_or;
    void *client_spill;     /* +0x30 */
    uint64_t pad;
    OutlineRec orec;        /* +0x40 rsp+0x80 (rbp-0x80) */
    PathProcs procs;        /* +0xa0 rbp-0x20 */
    uint64_t iocopy[6];     /* +0xd0 rbp+0x10 */
    PathProcs sbprocs;      /* +0x100 rbp+0x40 */
    uint64_t box[7];        /* +0x130 rbp+0x70 */
} ATMFrame;
_Static_assert(__builtin_offsetof(ATMFrame, orec) == 0x40, "ATMFrame.orec");
_Static_assert(__builtin_offsetof(ATMFrame, box) == 0x130, "ATMFrame.box");

/* Control: the bold flag is ignored (plain outlines for bold faces). */
CFF_API int32_t cff_ATMCharOutline_impl(uint8_t *state, uint8_t *mem, void *cs, uint8_t *io, const PathProcs *client_procs,
                                        uint32_t flags, void *client) {
    T1InterpretFn interpret = CFF_ORIG(T1InterpretFn, RVA_Type1InterpretCharString);
    int32_t (*getMemory)(OutlineRec *) = CFF_ORIG(int32_t (*)(OutlineRec *), RVA_OutlineGetMemory);
    int32_t (*seacFn)(const int32_t *, OutlineRec *) = CFF_ORIG(int32_t (*)(const int32_t *, OutlineRec *), RVA_OutlineSeac);
    ParsePathFn parsePath = CFF_ORIG(ParsePathFn, RVA_BCParsePath);
    ATMFrame f;
    int32_t r;
#ifdef CFF_CONTROL_ATMCHAROUTLINE
    flags &= ~2u;
#endif
    f.client_procs = (void *)client_procs;
    f.client_spill = client;
    memset(f.box, 0, sizeof f.box);
    memset(&f.orec, 0, sizeof f.orec);
    uint32_t bit0 = flags & 1;
    f.orec.procs = (void *)client_procs;
    f.rec_zero = 0;
    f.orec.first = NULL;
    f.orec.pending = 0;
    f.procs.newchar = CFF_ORIG(Proc2, RVA_OutlineNewChar);
    f.procs.moveto = CFF_ORIG(ProcPt, RVA_OutlineMoveTo);
    f.procs.lineto = CFF_ORIG(ProcPt, RVA_OutlineLineTo);
    f.procs.curveto = CFF_ORIG(ProcCurve, RVA_OutlineCurveTo);
    f.procs.closepath = CFF_ORIG(Proc2, RVA_OutlineClosePath);
    f.procs.endchar = CFF_ORIG(Proc2, RVA_OutlineEndChar);
    f.orec.mem = mem;
    f.rec_flags = bit0 | 0x200;
    f.rec_box = f.box;
    f.rec_or = &f.orec;
    f.orec.client = client;
    f.rec_client = client;
    if (!getMemory(&f.orec)) return (int32_t)0xfc020530;
    r = interpret(state, mem, cs, io, 0, &f.procs, &f.rec_flags);
    if (r == 1) {
        /* seac: the accent with the offset, then the base */
        memcpy(f.seac, f.box, 8);
        seacFn(f.seac, &f.orec);
        uint8_t *io1;
        memcpy(&io1, io + 8, 8);
        f.rec_flags |= 0x10;
        memcpy(&f.box[2], io1, 0x28);
        memcpy(f.iocopy, io, 0x30);
        uint64_t *pbox2 = &f.box[2];
        memcpy((uint8_t *)f.iocopy + 8, &pbox2, 8);
        int32_t bx;
        memcpy(&bx, f.box, 4);
        st_set_i32(state, 0x3d0, bx);
        r = interpret(state, mem, cs, (uint8_t *)f.iocopy, 0, &f.procs, &f.rec_flags);
        int32_t sum = add32(st_i32(state, 0x3d0), st_i32(state, 0x3c4));
        st_set_i32(state, 0x3c4, (int32_t)0x80000000);
        if (r == 0) {
            seacFn(f.seac, &f.orec);
            f.procs.newchar = NULL;
            f.rec_flags = bit0 | 0x288;
            r = interpret(state, mem, cs, io, 0, &f.procs, &f.rec_flags);
        }
        if (sum > st_i32(state, 0x3c4)) {
            memcpy(&io1, io + 8, 8);
            memcpy(io1 + 0x20, &f.box[6], 8);
        }
        if (r == 1) {
            r = (int32_t)0xfe020557;
            goto cleanup;
        }
    }
    if (r == 0) {
        uint32_t v = (uint32_t)st_i32(state, 0x2bc);
        if (room(&f.orec)) {
            put32(f.orec.cur, 0, 0xe);
            f.orec.cur += 4;
        }
        int32_t fl = (int32_t)((((((v >> 3) ^ v) >> 3) ^ v) >> 16) & 1);
        int32_t *bbox;
        memcpy(&bbox, io + 0x10, 8);
        if (!(flags & 2)) {
            r = parsePath(f.orec.first, mem, client_procs, bbox, 0, fl, state, client);
        } else {
            uint8_t *buf = st_ptr(state, ST_SB_BASE);
            if (!buf) {
                buf = crt_malloc(0x2000);
                st_set_ptr(state, ST_SB_BASE, buf);
                if (!buf) {
                    r = (int32_t)0xfc020569;
                    goto blocks;
                }
            }
            int32_t sz = 0x2000;
            memcpy(buf, &sz, 4);
            buf = st_ptr(state, ST_SB_BASE);
            st_set_ptr(state, ST_SB_CUR, buf + 4);
            st_set_ptr(state, ST_SB_END, buf + 0x2000);
            f.sbprocs.newchar = CFF_ORIG(Proc2, RVA_SBNewChar);
            f.sbprocs.moveto = CFF_ORIG(ProcPt, RVA_SBMoveTo);
            f.sbprocs.lineto = CFF_ORIG(ProcPt, RVA_SBLineTo);
            f.sbprocs.curveto = CFF_ORIG(ProcCurve, RVA_SBCurveTo);
            f.sbprocs.closepath = CFF_ORIG(Proc2, RVA_SBClosePath);
            f.sbprocs.endchar = CFF_ORIG(Proc2, RVA_SBEndChar);
            r = parsePath(f.orec.first, mem, &f.sbprocs, bbox, 0, fl, state, state);
            if (r == 0) {
                memcpy(&bbox, io + 0x10, 8);
                if (!CFF_ORIG(int32_t (*)(int32_t *, uint8_t *), RVA_AdobeBoldSimulation)(bbox, state)) {
                    r = (int32_t)0xfc02057d;
                    goto blocks;
                }
                if (!CFF_ORIG(int32_t (*)(const PathProcs *, uint8_t *, void *), RVA_PlayOutline)(client_procs, state, client))
                    r = (int32_t)0xfc020581;
            }
        }
    }
    if (((uint32_t)r & 0xff000000u) != 0xfc000000u) goto cleanup;
blocks:
    if (f.orec.nblocks >= 0x15e) r = (int32_t)0xfe02058c;
cleanup:
    CFF_ORIG(void (*)(OutlineRec *), RVA_OutlineFreeMemory)(&f.orec);
    my_free(st_ptr(state, ST_SB_BASE));
    st_set_ptr(state, ST_SB_BASE, NULL);
    if (f.orec.drawn == 0) {
        int32_t z = 0;
        uint8_t *bb;
        memcpy(&bb, io + 0x10, 8);
        memcpy(bb + 0xc, &z, 4);
        memcpy(&bb, io + 0x10, 8);
        memcpy(bb + 8, &z, 4);
        memcpy(&bb, io + 0x10, 8);
        memcpy(bb + 4, &z, 4);
        memcpy(&bb, io + 0x10, 8);
        memcpy(bb, &z, 4);
    }
    return r;
}
PATH_EXPORT_STACK(cff_ATMCharOutline, cff_ATMCharOutline_impl);

/* ------------------------------------------------------------------------------------------
 * flattening */

typedef int32_t (*PointFn)(int32_t *pt, void *a, void *b);

/* FMiniFltn 0xc74d4 (296 insns): the curve relative to (minx, miny) in 24.8 shorts, bisected
 * on a stack of at most 6 curves until it is monotonic in x and y and flat within the
 * tolerance (max(8, s/4 + s/8) with s = tol >> 8); emits each piece's end point, the last
 * one exactly p3. Control: the tolerance is one unit larger. */
CFF_API int32_t cff_FMiniFltn_impl(const int32_t *p0, const int32_t *p1, const int32_t *p2, const int32_t *p3, int32_t minx,
                                   int32_t miny, int32_t tol7, PointFn proc, void *a9, void *a10) {
    int16_t buf[8 * 6];
    int16_t *b = buf + 8;
    int16_t lvl = 1;
    buf[0] = (int16_t)(sub32(p0[0], minx) >> 8);
    buf[1] = (int16_t)(sub32(p0[1], miny) >> 8);
    buf[2] = (int16_t)(sub32(p1[0], minx) >> 8);
    buf[3] = (int16_t)(sub32(p1[1], miny) >> 8);
    buf[4] = (int16_t)(sub32(p2[0], minx) >> 8);
    buf[5] = (int16_t)(sub32(p2[1], miny) >> 8);
    buf[6] = (int16_t)(sub32(p3[0], minx) >> 8);
    buf[7] = (int16_t)(sub32(p3[1], miny) >> 8);
    int16_t s = (int16_t)(tol7 >> 8);
    int16_t t = (int16_t)(s >> 2);
    int16_t tol = (int16_t)(t + (int16_t)(t >> 1));
    if (tol < 8) tol = 8;
    int32_t tolz = (uint16_t)tol;
#ifdef CFF_CONTROL_FMINIFLTN
    tolz += 1;
#endif
    for (;;) {
        if (lvl < 6) {
            int32_t x0 = b[-8], y0 = b[-7], x1 = b[-6], y1 = b[-5], x2 = b[-4], y2 = b[-3], x3 = b[-2], y3 = b[-1];
            int flat = 0;
            if (((x0 <= x1 && x1 <= x2 && x2 <= x3) || (x3 <= x2 && x2 <= x1 && x1 <= x0)) &&
                ((y0 <= y1 && y1 <= y2 && y2 <= y3) || (y3 <= y2 && y2 <= y1 && y1 <= y0))) {
                int32_t fx = x3 - x0;
                fx = fx < 0 ? -fx : fx;
                int32_t d = x1 - x0;
                d = d < 0 ? -d : d;
                int32_t v = fx - 3 * d;
                v = v < 0 ? -v : v;
                if (v <= tolz) {
                    d = x2 - x0;
                    d = d < 0 ? -d : d;
                    v = 2 * fx - 3 * d;
                    v = v < 0 ? -v : v;
                    if (v <= tolz) {
                        int32_t fy = y3 - y0;
                        fy = fy < 0 ? -fy : fy;
                        d = y1 - y0;
                        d = d < 0 ? -d : d;
                        v = fy - 3 * d;
                        v = v < 0 ? -v : v;
                        if (v <= tolz) {
                            d = y2 - y0;
                            d = d < 0 ? -d : d;
                            v = 2 * fy - 3 * d;
                            v = v < 0 ? -v : v;
                            if (v <= tolz) flat = 1;
                        }
                    }
                }
            }
            if (!flat) {
                /* bisect: the first half goes on top (b[0..7]), the second replaces b[-8..-1] */
                int16_t *n = b;
                n[0] = (int16_t)x0;
                int32_t a = (x0 + x1) >> 1;
                n[2] = (int16_t)a;
                int32_t bb = (int16_t)((x2 + x1) >> 1);
                int32_t c = (x3 + x2) >> 1;
                int32_t dd = ((int16_t)a + bb) >> 1;
                b[-4] = (int16_t)c;
                int32_t e = ((int16_t)c + bb) >> 1;
                b[-6] = (int16_t)e;
                n[4] = (int16_t)dd;
                int32_t m = ((int16_t)dd + (int16_t)e) >> 1;
                n[1] = (int16_t)y0;
                n[6] = (int16_t)m;
                b[-8] = (int16_t)m;
                int32_t ay = (y1 + y0) >> 1;
                int32_t by = (int16_t)((y2 + y1) >> 1);
                int32_t cy = (y3 + y2) >> 1;
                int32_t dy = ((int16_t)ay + by) >> 1;
                n[3] = (int16_t)ay;
                int32_t ey = ((int16_t)cy + by) >> 1;
                b[-3] = (int16_t)cy;
                n[5] = (int16_t)dy;
                b[-5] = (int16_t)ey;
                int32_t my = ((int16_t)dy + (int16_t)ey) >> 1;
                n[7] = (int16_t)my;
                b[-7] = (int16_t)my;
                b += 8;
                lvl++;
                continue;
            }
        }
        lvl--;
        int32_t pt[2];
        if (lvl == 0) {
            pt[0] = p3[0];
            pt[1] = p3[1];
        } else {
            pt[0] = (int32_t)(((uint32_t)(int32_t)b[-2] << 8) + (uint32_t)minx);
            pt[1] = (int32_t)(((uint32_t)(int32_t)b[-1] << 8) + (uint32_t)miny);
        }
        if (!proc(pt, a9, a10)) return 0;
        if (lvl == 0) return 1;
        b -= 8;
    }
}
PATH_EXPORT_STACK(cff_FMiniFltn, cff_FMiniFltn_impl);

typedef int32_t (*IFFFn)(const int32_t *, const int32_t *, const int32_t *, const int32_t *, void *, uint8_t *, int32_t);

static inline int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }
static inline int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }

/* InternalFixedFltn 0xc7284 (169 insns): FMiniFltn when the control box is under 0x7f0000
 * in x and y, else the de Casteljau halves (32-bit, >> 1), at most 10 levels deep.
 * Control: the size limit is 0x400000. */
CFF_API int32_t cff_InternalFixedFltn_impl(const int32_t *p0, const int32_t *p1, const int32_t *p2, const int32_t *p3,
                                           void *a5, uint8_t *rec, int32_t depth) {
#ifdef CFF_CONTROL_INTERNALFIXEDFLTN
    const int32_t limit = 0x400000;
#else
    const int32_t limit = 0x7f0000;
#endif
    if (depth > 10) return 0;
    int32_t xmin = min32(min32(min32(p1[0], p0[0]), p2[0]), p3[0]);
    int32_t xmax = max32(max32(max32(p1[0], p0[0]), p2[0]), p3[0]);
    if (sub32(xmax, xmin) < limit) {
        int32_t ymin = min32(min32(min32(p1[1], p0[1]), p2[1]), p3[1]);
        int32_t ymax = max32(max32(max32(p1[1], p0[1]), p2[1]), p3[1]);
        if (sub32(ymax, ymin) < limit) {
            void *client;
            int32_t tol;
            uint8_t *procs;
            PointFn proc;
            memcpy(&client, rec, 8);
            memcpy(&tol, rec + 0x10, 4);
            memcpy(&procs, rec + 0x18, 8);
            memcpy(&proc, procs + 0x10, 8);
            return CFF_ORIG(int32_t (*)(const int32_t *, const int32_t *, const int32_t *, const int32_t *, int32_t, int32_t,
                                        int32_t, PointFn, void *, void *),
                            RVA_FMiniFltn)(p0, p1, p2, p3, xmin, ymin, tol, proc, a5, client) != 0;
        }
    }
    int32_t q0[2] = {p0[0], p0[1]}, q3[2] = {p3[0], p3[1]};
    int32_t a[2], bpt[2], c[2], d[2], e[2], m1[2], m2[2];
    a[0] = add32(p0[0], p1[0]) >> 1;
    a[1] = add32(p0[1], p1[1]) >> 1;
    bpt[0] = add32(p1[0], p2[0]) >> 1;
    bpt[1] = add32(p1[1], p2[1]) >> 1;
    c[0] = add32(p3[0], p2[0]) >> 1;
    c[1] = add32(p3[1], p2[1]) >> 1;
    d[0] = add32(a[0], bpt[0]) >> 1;
    d[1] = add32(a[1], bpt[1]) >> 1;
    e[0] = add32(bpt[0], c[0]) >> 1;
    e[1] = add32(bpt[1], c[1]) >> 1;
    m1[0] = m2[0] = add32(e[0], d[0]) >> 1;
    m1[1] = m2[1] = add32(e[1], d[1]) >> 1;
    IFFFn self = CFF_ORIG(IFFFn, RVA_InternalFixedFltn);
    if (!self(q0, a, d, m1, a5, rec, depth + 1)) return 0;
    if (!self(m2, e, c, q3, a5, rec, depth + 1)) return 0;
    return 1;
}
PATH_EXPORT_STACK(cff_InternalFixedFltn, cff_InternalFixedFltn_impl);

/* FixedFltn 0xc71e0 (10 insns). Control: starts at depth 11 (every curve fails). */
CFF_API int32_t cff_FixedFltn_impl(const int32_t *p0, const int32_t *p1, const int32_t *p2, const int32_t *p3, void *a5,
                                   uint8_t *rec) {
#ifdef CFF_CONTROL_FIXEDFLTN
    const int32_t depth = 11;
#else
    const int32_t depth = 0;
#endif
    return CFF_ORIG(IFFFn, RVA_InternalFixedFltn)(p0, p1, p2, p3, a5, rec, depth);
}
PATH_EXPORT_STACK(cff_FixedFltn, cff_FixedFltn_impl);

/* FixFltnCurveTo 0xc7140 (38 insns): flattens into ATMQNewPoint(pt, state, client), with the
 * tolerance at state +0x528. Control: the tolerance is doubled. */
CFF_API int32_t cff_FixFltnCurveTo_impl(const int32_t *p0, const int32_t *p1, const int32_t *p2, const int32_t *p3,
                                        uint8_t *state, void *client) {
    struct {
        uint64_t procs[6]; /* rbp-0x29: a proc record, +0x10 the point proc */
        uint64_t rec[7];   /* rbp+7: the flattening record */
    } fr;
    memset(&fr, 0, sizeof fr);
    int32_t tol = st_i32(state, 0x528);
#ifdef CFF_CONTROL_FIXFLTNCURVETO
    tol = add32(tol, tol);
#endif
    memcpy((uint8_t *)fr.rec + 0x10, &tol, 4);
    void *qnp = idwr_orig(RVA_ATMQNewPoint);
    memcpy((uint8_t *)fr.procs + 0x10, &qnp, 8);
    uint64_t *pp = fr.procs;
    memcpy((uint8_t *)fr.rec + 0x18, &pp, 8);
    memcpy(fr.rec, &client, 8);
    return CFF_ORIG(IFFFn, RVA_InternalFixedFltn)(p0, p1, p2, p3, state, (uint8_t *)fr.rec, 0) != 0;
}
PATH_EXPORT_STACK(cff_FixFltnCurveTo, cff_FixFltnCurveTo_impl);
