/* CScan dropout control and pixel fix-ups of the CFF rasterizer (notes/cff.md, group 9
 * "dropout"), ported from the x64 assembly of DWrite.dll 10.0.26100.9711.
 *
 * Entered from cscan: EditBlackSpace and EditWhiteSpace (CScan), FixCrossFlags (FinishPath).
 * Calls out to the cscan cross-list helpers, fixmul and fixdiv through CFF_ORIG, and to the
 * other functions of this group through their original entry points too, so each port
 * swaps alone.
 *
 * Data layout (shared with cscan):
 *
 *   CScan state (first argument of most functions):
 *     +0x18  cross * *rows: per scanline the head of a singly linked list of crossings,
 *            sorted by x, in (left, right) pairs: the runs of the row
 *     +0x30, +0x38  two lists of inflection crossings (DoXInflections)
 *     +0x50, +0x58  the cross allocator: next record, end (NewCross & co.)
 *     +0x60, +0x68  the splice under construction: the link record, the start crossing
 *     +0x2d0 s16 first row, +0x2d2 s16 last row, +0x2d4 s16 min x, +0x2d6 s16 max x
 *   cross record (0x18 bytes, laid out consecutively along each contour):
 *     +0x00  next crossing in the row list
 *     +0x08  s32 x (16.16), or for a link record (flags 0x1000) the byte offset to the next
 *     +0x0c  s32 y (16.16), or for a link record the byte offset back; +0x0e s16 its row
 *     +0x10  u16 flags: 0x3 direction to the next row crossing (1 forward, 2 back), 0xc the
 *            same for horizontal neighbours, 0x100 up, 0x200 run end adjusted, 0x400 left
 *            edge of a run, 0x800 spliced, 0x1000 link, 0x2000 extra (not on a scanline),
 *            0x4000 contour end
 *     +0x12  s16 the run's pixel column (left: first pixel; right: one past the last)
 *   edge record (0x18 bytes, FillInColumn/FillInInflection -> SetPixelInDropOut):
 *     +0x00 s32 coordinate, +0x08 cross *, +0x10 s32 direction
 *   rule description (FixPixelProblem's two 13-byte stack records): [0] the context byte
 *     of BuildCxtn, then entries of 3 bytes {action, priority, rotation} ended by a zero
 *     priority.
 *
 * The data tables are copied byte for byte: .rdata 0x18020a300..0x18020a33f (the rotation
 * table of GetRuleDesc and, at +0x2c, ScanLineCxtn's neighbourhood bits), .data
 * 0x180231bb8..0x180231bdf (action lists), 0x180231c00..0x180231cbb (47 patterns),
 * 0x180231cc0..0x180231cdb (action/priority pairs).
 *
 * Exports: functions with more than four arguments are exported with DROP_EXPORT_STK, a
 * variant of cff_port.h's thunk that also copies the fifth and sixth (stack) arguments:
 * CFF_EXPORT's thunk moves the stack, so a port behind it would read its stack arguments
 * from the thunk's frame.
 *
 * Negative controls: -DCFF_CONTROL_<NAME> (the function name in capitals) breaks <NAME>. */
#include "cff_port.h"

#ifdef IDWR_STANDALONE
#define DROP_EXPORT_STK(name, impl) CFF_EXPORT(name, impl)
#define FinishSplice_impl cff_FinishSplice
#define SetLeftFlag_impl cff_SetLeftFlag
#define StartSplice_impl cff_StartSplice
#define SetPixelInDropOut_impl cff_SetPixelInDropOut
#define ExpandCharBBox_impl cff_ExpandCharBBox
#define ExtendRunDropOut_impl cff_ExtendRunDropOut
#define SplicePixel_impl cff_SplicePixel
#define FillInInflection_impl cff_FillInInflection
#define FillInColumn_impl cff_FillInColumn
#define CheckHorizWhite_impl cff_CheckHorizWhite
#define ConnectedRunOpposite_impl cff_ConnectedRunOpposite
#define CheckBlackDown_impl cff_CheckBlackDown
#define FixCrossFlags_impl cff_FixCrossFlags
#define DoXInflections_impl cff_DoXInflections
#define EditBlackSpace_impl cff_EditBlackSpace
#define EditBlackHoriz_impl cff_EditBlackHoriz
#define FillInPairs_impl cff_FillInPairs
#define EditWhiteSpace_impl cff_EditWhiteSpace
#define EditWhiteHoriz_impl cff_EditWhiteHoriz
#define FillInRange_impl cff_FillInRange
#define ExpandWhiteDropOuts_impl cff_ExpandWhiteDropOuts
#define FixRuns_impl cff_FixRuns
#define GetRuleDesc_impl cff_GetRuleDesc
#define BuildCxtn_impl cff_BuildCxtn
#define FixPixelProblem_impl cff_FixPixelProblem
#define DoFixupAction_impl cff_DoFixupAction
#define PixelIsBlack_impl cff_PixelIsBlack
#define ScanLineCxtn_impl cff_ScanLineCxtn
#define PatternToActions_impl cff_PatternToActions
#define CheckDeletePixel_impl cff_CheckDeletePixel
#define FixupExtendLeft_impl cff_FixupExtendLeft
#define ConnectWhitePairs_impl cff_ConnectWhitePairs
#define DetermineTieBreaker_impl cff_DetermineTieBreaker
#define FixupExtendRight_impl cff_FixupExtendRight
#else
/* CFF_EXPORT, plus a copy of the caller's stack arguments 5 and 6 ([entry rsp + 0x28],
 * [entry rsp + 0x30]) into the outgoing argument area of the thunk's frame. */
#define DROP_EXPORT_STK(name, impl)                                                    \
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
            "subq $0x88, %rsp\n.seh_stackalloc 0x88\n"                                 \
            "movaps %xmm1, 0x30(%rsp)\n.seh_savexmm %xmm1, 0x30\n"                     \
            "movaps %xmm2, 0x40(%rsp)\n.seh_savexmm %xmm2, 0x40\n"                     \
            "movaps %xmm3, 0x50(%rsp)\n.seh_savexmm %xmm3, 0x50\n"                     \
            "movaps %xmm4, 0x60(%rsp)\n.seh_savexmm %xmm4, 0x60\n"                     \
            "movaps %xmm5, 0x70(%rsp)\n.seh_savexmm %xmm5, 0x70\n"                     \
            ".seh_endprologue\n"                                                       \
            "movq 0xe0(%rsp), %rax\n"                                                  \
            "movq %rax, 0x20(%rsp)\n"                                                  \
            "movq 0xe8(%rsp), %rax\n"                                                  \
            "movq %rax, 0x28(%rsp)\n"                                                  \
            "callq " #impl "\n"                                                        \
            "movaps 0x30(%rsp), %xmm1\n"                                               \
            "movaps 0x40(%rsp), %xmm2\n"                                               \
            "movaps 0x50(%rsp), %xmm3\n"                                               \
            "movaps 0x60(%rsp), %xmm4\n"                                               \
            "movaps 0x70(%rsp), %xmm5\n"                                               \
            "addq $0x88, %rsp\n"                                                       \
            "popq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n"         \
            "retq\n"                                                                   \
            ".seh_endproc\n"                                                           \
            ".section .drectve,\"yn\"\n"                                               \
            ".ascii \" -export:" #name "\"\n"                                          \
            ".text\n")
#endif

/* ------------------------------------------------------------------------------------------
 * memory access: the records are read and written as bytes at offsets, like the original */

static inline int16_t rd16(const char *p, long o) {
    int16_t v;
    memcpy(&v, p + o, 2);
    return v;
}
static inline uint16_t ru16(const char *p, long o) {
    uint16_t v;
    memcpy(&v, p + o, 2);
    return v;
}
static inline int32_t rd32(const char *p, long o) {
    int32_t v;
    memcpy(&v, p + o, 4);
    return v;
}
static inline char *rdp(const char *p, long o) {
    char *v;
    memcpy(&v, p + o, sizeof v);
    return v;
}
static inline void wr16(char *p, long o, uint16_t v) { memcpy(p + o, &v, 2); }
static inline void wr32(char *p, long o, int32_t v) { memcpy(p + o, &v, 4); }
static inline void wrp(char *p, long o, char *v) { memcpy(p + o, &v, sizeof v); }

static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t shl16(int32_t a) { return (int32_t)((uint32_t)a << 16); }
static inline uint32_t lo32(const void *p) { return (uint32_t)(uintptr_t)p; }

#define NEXT(c) rdp(c, 0)
#define CX(c) rd32(c, 8)
#define CY(c) rd32(c, 0xc)
#define ROW(c) rd16(c, 0xe)
#define FL(c) ru16(c, 0x10)
#define FL8(c) ((uint8_t)(c)[0x10])
#define PX(c) rd16(c, 0x12)
#define SETFL(c, v) wr16(c, 0x10, (uint16_t)(v))
#define SETPX(c, v) wr16(c, 0x12, (uint16_t)(v))
#define ROWHEAD(st, r) rdp(rdp(st, 0x18), (long)(r) * 8)
#define NOCROSS ((char *)(intptr_t)-1)

/* follow link records (flags 0x1000) forward */
static inline char *skiplink(char *c) {
    while (FL(c) & 0x1000) c += CX(c);
    return c;
}

typedef struct {
    int32_t x;
    int32_t pad;
    char *cross;
    int32_t dir;
    int32_t pad2;
} DropEdge;
_Static_assert(sizeof(DropEdge) == 0x18, "edge record");

/* ------------------------------------------------------------------------------------------
 * callees, through their original entry points */

typedef char *(*cross_fn)(char *);
typedef char *(*cross_dir_fn)(char *, int32_t);
typedef char *(*runpair_fn)(char *, char *);
typedef char *(*newcross_fn)(char *, int32_t, int32_t);
typedef char *(*newlink_fn)(char *);
typedef void (*addy_fn)(char *, char *);
typedef int32_t (*fix_fn)(int32_t, int32_t);

#define FORW(c) CFF_ORIG(cross_fn, 0xc18c0)(c)
#define BACK(c) CFF_ORIG(cross_fn, 0xc18f4)(c)
#define PATHUP(c) CFF_ORIG(cross_fn, 0xc1a98)(c)
#define XTRA(c, d) CFF_ORIG(cross_dir_fn, 0xc0df0)(c, d)
#define XTRAORIG(c, d) CFF_ORIG(cross_dir_fn, 0xc0dc4)(c, d)
#define RUNPAIR(st, c) CFF_ORIG(runpair_fn, 0xc1928)(st, c)
#define NEWCROSS(st, x, y) CFF_ORIG(newcross_fn, 0xbe7b8)(st, x, y)
#define NEWXTRA(st, x, y) CFF_ORIG(newcross_fn, 0xc1ac8)(st, x, y)
#define NEWLINK(st) CFF_ORIG(newlink_fn, 0xc10c8)(st)
#define ADDY(st, c) CFF_ORIG(addy_fn, 0xbe624)(st, c)
#define FIXMUL(a, b) CFF_ORIG(fix_fn, 0xb3340)(a, b)
#define FIXDIV(a, b) CFF_ORIG(fix_fn, 0xb316c)(a, b)

typedef int32_t (*finish_fn)(char *);
typedef void (*setleft_fn)(char *, char *);
typedef int32_t (*start_fn)(char *, char *);
typedef int32_t (*setpixel_fn)(char *, int32_t, DropEdge *, DropEdge *, int32_t);
typedef void (*bbox_fn)(char *, int32_t, int32_t);
typedef int32_t (*extend_fn)(char *, int32_t, int32_t, DropEdge *);
typedef int32_t (*splice_fn)(char *, int32_t, int32_t, char *, int32_t);
typedef int32_t (*fillinfl_fn)(char *, char *, char *, int32_t);
typedef int32_t (*fillcol_fn)(char *, char *, int32_t, int32_t, int32_t, int32_t);
typedef int32_t (*horizwhite_fn)(char *, char *, char *, char *, int32_t);
typedef char *(*cro_fn)(char *, char *, int32_t);
typedef int32_t (*blackdown_fn)(char *, char *, char *);
typedef char *(*fixflags_fn)(char *, char *);
typedef int32_t (*st_fn)(char *);
typedef int32_t (*blackhoriz_fn)(char *, char *, int32_t);
typedef int32_t (*fillpairs_fn)(char *, char *, char *, char *, int32_t, int32_t);
typedef void (*whitehoriz_fn)(char *, char *, int32_t, char *);
typedef int32_t (*fillrange_fn)(char *, char *, int32_t, int32_t, int32_t, int32_t);
typedef void (*st_ctx_fn)(char *, char *);
typedef void (*fixruns_fn)(char *, char *, char *, char *);
typedef void (*rule_fn)(char *, uint8_t *, char *, int32_t, char *, int32_t);
typedef uint8_t (*cxtn_fn)(char *, char *, int32_t, int32_t);
typedef void (*fixpixel_fn)(char *, char *, int32_t, char *, int32_t, char *);
typedef int32_t (*fixup_fn)(char *, uint8_t *, uint8_t *, char *, int32_t);
typedef int32_t (*pixel_fn)(char *, int32_t, int32_t);
typedef uint8_t (*scanline_fn)(char *, char *, int32_t, int32_t, int32_t);
typedef void (*pattern_fn)(uint8_t *, const uint8_t *, int32_t);
typedef int32_t (*delete_fn)(char *, int32_t);
typedef int32_t (*tie_fn)(char *, int32_t, char *, int32_t, char *);
/* the point transform at ctx + 0x588: (point by value, out, ctx) */
typedef void (*tfm_fn)(uint64_t, int32_t *, char *);

#define FINISHSPLICE(st) CFF_ORIG(finish_fn, 0xbe674)(st)
#define SETLEFTFLAG(st, c) CFF_ORIG(setleft_fn, 0xbe788)(st, c)
#define STARTSPLICE(st, c) CFF_ORIG(start_fn, 0xbe7ec)(st, c)
#define SETPIXEL(st, x, a, b, f) CFF_ORIG(setpixel_fn, 0xbe8a0)(st, x, a, b, f)
#define EXPANDBBOX(st, x, r) CFF_ORIG(bbox_fn, 0xbe968)(st, x, r)
#define EXTENDRUN(st, x, r, e) CFF_ORIG(extend_fn, 0xbe9c8)(st, x, r, e)
#define SPLICE(st, x, r, c, d) CFF_ORIG(splice_fn, 0xbeb08)(st, x, r, c, d)
#define FILLINFL(st, c, u, x) CFF_ORIG(fillinfl_fn, 0xbefd4)(st, c, u, x)
#define FILLCOL(st, c, d, x, f, g) CFF_ORIG(fillcol_fn, 0xbfbdc)(st, c, d, x, f, g)
#define HORIZWHITE(st, a, b, c, d) CFF_ORIG(horizwhite_fn, 0xc0bbc)(st, a, b, c, d)
#define CRO(st, c, f) CFF_ORIG(cro_fn, 0xc10fc)(st, c, f)
#define BLACKDOWN(st, a, b) CFF_ORIG(blackdown_fn, 0xc118c)(st, a, b)
#define FIXFLAGS(a, b) CFF_ORIG(fixflags_fn, 0xc1244)(a, b)
#define DOXINFL(st) CFF_ORIG(st_fn, 0xc12e0)(st)
#define BLACKHORIZ(st, c, d) CFF_ORIG(blackhoriz_fn, 0xc196c)(st, c, d)
#define FILLPAIRS(st, a, b, c, d, f) CFF_ORIG(fillpairs_fn, 0xc1af8)(st, a, b, c, d, f)
#define WHITEHORIZ(st, c, d, x) CFF_ORIG(whitehoriz_fn, 0xc1db0)(st, c, d, x)
#define FILLRANGE(st, c, d, a, b, f) CFF_ORIG(fillrange_fn, 0xc1ea8)(st, c, d, a, b, f)
#define EXPANDWHITE(st, x) CFF_ORIG(st_ctx_fn, 0xc1f84)(st, x)
#define FIXRUNS(st, a, b, x) CFF_ORIG(fixruns_fn, 0xc2028)(st, a, b, x)
#define GETRULE(st, d, c, x, co, xo) CFF_ORIG(rule_fn, 0xc20f4)(st, d, c, x, co, xo)
#define BUILDCXTN(st, c, x, f) CFF_ORIG(cxtn_fn, 0xc2250)(st, c, x, f)
#define FIXPIXEL(st, a, xa, b, xb, x) CFF_ORIG(fixpixel_fn, 0xc279c)(st, a, xa, b, xb, x)
#define DOFIXUP(st, d, e, c, x) CFF_ORIG(fixup_fn, 0xc28e0)(st, d, e, c, x)
#define PIXELBLACK(st, x, r) CFF_ORIG(pixel_fn, 0xc2c04)(st, x, r)
#define SCANLINE(a, b, x, m, f) CFF_ORIG(scanline_fn, 0xc2c58)(a, b, x, m, f)
#define PATTERN(d, e, r) CFF_ORIG(pattern_fn, 0xc2d74)(d, e, r)
#define CHECKDELETE(c, x) CFF_ORIG(delete_fn, 0xc2e18)(c, x)
#define EXTENDLEFT(st, x, r) CFF_ORIG(pixel_fn, 0x10c47c)(st, x, r)
#define CONNECTWHITE(st, a, b, x) CFF_ORIG(fixruns_fn, 0x184568)(st, a, b, x)
#define TIEBREAK(a, xa, b, xb, x) CFF_ORIG(tie_fn, 0x1845cc)(a, xa, b, xb, x)
#define EXTENDRIGHT(st, x, r) CFF_ORIG(pixel_fn, 0x18465c)(st, x, r)

/* ------------------------------------------------------------------------------------------
 * data tables */

/* .rdata 0x18020a300: 9 (+3) s32 rotations of GetRuleDesc; at +0x28 ScanLineCxtn's
 * neighbourhood bits, indexed from +0x2c by 3 * mode + dx (mode, dx in -1..1) */
static const uint8_t rdata_20a300[0x40] = {
    0x06, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00,
    0x08, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x20, 0x10, 0x80, 0x00, 0x08, 0x01, 0x02,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f,
};

/* .data 0x180231bb8: action lists of 8 bytes, (code, rotation) pairs ended by code 0xd */
static const uint8_t data_231bb8[0x28] = {
    0x00, 0x07, 0x05, 0x00, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x07, 0x05, 0x00, 0x0d, 0x00, 0x00, 0x00,
    0x00, 0x07, 0x00, 0x01, 0x05, 0x00, 0x0d, 0x00, 0x00, 0x01, 0x05, 0x00, 0x0d, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x05, 0x00, 0x0d, 0x00, 0x00, 0x00,
};

/* .data 0x180231c00: 47 patterns {u16 key (context << 8 | rotated class), code, rotation},
 * sorted by key; code 0xd: the rotation byte is an index into data_231bb8 */
static const uint8_t data_231c00[47 * 4] = {
    0x02, 0x01, 0x0d, 0x00, 0x03, 0x01, 0x0d, 0x01, 0x04, 0x01, 0x0d, 0x02, 0x05, 0x01, 0x0d, 0x03,
    0x06, 0x01, 0x0d, 0x04, 0x03, 0x02, 0x09, 0x00, 0x04, 0x02, 0x04, 0x00, 0x05, 0x02, 0x04, 0x00,
    0x06, 0x02, 0x04, 0x00, 0x07, 0x02, 0x09, 0x00, 0x03, 0x03, 0x0a, 0x00, 0x04, 0x03, 0x06, 0x00,
    0x05, 0x03, 0x06, 0x00, 0x06, 0x03, 0x06, 0x00, 0x04, 0x05, 0x01, 0x01, 0x05, 0x05, 0x01, 0x01,
    0x06, 0x05, 0x01, 0x01, 0x04, 0x06, 0x06, 0x00, 0x05, 0x06, 0x06, 0x00, 0x06, 0x06, 0x06, 0x00,
    0x07, 0x06, 0x0a, 0x00, 0x04, 0x07, 0x08, 0x00, 0x05, 0x07, 0x08, 0x00, 0x06, 0x07, 0x08, 0x00,
    0x05, 0x09, 0x03, 0x01, 0x06, 0x09, 0x02, 0x01, 0x05, 0x0a, 0x0b, 0x00, 0x06, 0x0a, 0x07, 0x00,
    0x07, 0x0a, 0x0b, 0x00, 0x05, 0x0b, 0x0b, 0x00, 0x06, 0x0b, 0x07, 0x00, 0x05, 0x0e, 0x0c, 0x00,
    0x06, 0x0e, 0x08, 0x00, 0x07, 0x0e, 0x0c, 0x00, 0x05, 0x0f, 0x0c, 0x00, 0x06, 0x0f, 0x08, 0x00,
    0x06, 0x12, 0x02, 0x03, 0x07, 0x12, 0x03, 0x03, 0x06, 0x13, 0x02, 0x03, 0x06, 0x17, 0x02, 0x03,
    0x06, 0x19, 0x02, 0x01, 0x06, 0x1a, 0x07, 0x00, 0x07, 0x1a, 0x0b, 0x00, 0x06, 0x1b, 0x08, 0x00,
    0x06, 0x1e, 0x08, 0x00, 0x07, 0x1e, 0x0c, 0x00, 0x06, 0x1f, 0x08, 0x00,
};

/* .data 0x180231cc0: per code {action, priority}; action 1 deletes a pixel, 2 moves one */
static const uint8_t data_231cc0[0x1c] = {
    0x02, 0x28, 0x02, 0x20, 0x02, 0x24, 0x02, 0x0e, 0x01, 0x18, 0x01, 0x18, 0x01, 0x14,
    0x01, 0x16, 0x01, 0x12, 0x01, 0x0a, 0x01, 0x06, 0x01, 0x08, 0x01, 0x04, 0x01, 0x01,
};

/* ------------------------------------------------------------------------------------------
 * splicing a pixel into the cross lists */

/* 0x1800be674: closes the splice StartSplice opened, linking the new crossings into the
 * contour; 0 when the allocator is exhausted */
CFF_API int32_t FinishSplice_impl(char *st) {
    char *n = NEWLINK(st);
    if (!n) return 0;
    char *s = rdp(st, 0x68), *l = rdp(st, 0x60);
    long off;
    if (!(FL(s + 0x18) & 0x1000)) {
        SETFL(s, FL(s) | 0x3000);
        wr32(s, 8, (int32_t)(lo32(l) - lo32(s) + 0x18));
        char *c = s - 0x18;
        if (FL(c) & 0x1000) c += CY(c);
        wr32(c, 0x20, (int32_t)(lo32(l) - lo32(c)));
        wr32(l, 0xc, (int32_t)(lo32(c) - lo32(l)));
        wr32(s, 0xc, (int32_t)(lo32(n) - lo32(s) - 0x18));
        wr32(n, 8, (int32_t)(lo32(s) - lo32(n) + 0x18));
        off = 0x30;
    } else {
        char *c = s + 0x18;
        wr32(n, 8, (int32_t)(lo32(c) - lo32(n) + (uint32_t)CX(c)));
        wr32(c, 8, (int32_t)(lo32(l) - lo32(c) + 0x18));
        int32_t o = CX(n);
        wr32(n, (long)o - 0xc, (int32_t)(0u - (uint32_t)o));
        wr32(l, 0xc, (int32_t)(lo32(s) - lo32(l)));
        off = 0x18;
    }
    for (char *p = l + off;; p += 0x18) {
        SETFL(p, FL(p) | 0x800);
        if (p == n) break;
    }
#ifdef CFF_CONTROL_FINISHSPLICE
    return 0;
#endif
    return 1;
}
CFF_EXPORT(cff_FinishSplice, FinishSplice_impl);

/* 0x1800be788: flags c as the left edge of its run when it is at an even list position */
CFF_API void SetLeftFlag_impl(char *st, char *c) {
    char *p = ROWHEAD(st, ROW(c));
    for (;;) {
        if (p == c) {
#ifndef CFF_CONTROL_SETLEFTFLAG
            SETFL(p, FL(p) | 0x400);
#endif
            return;
        }
        p = NEXT(p);
        if (p == c) return;
        p = NEXT(p);
    }
}
CFF_EXPORT(cff_SetLeftFlag, SetLeftFlag_impl);

/* 0x1800be7ec: opens a splice at crossing c (a link record, and a copy of c that replaces
 * it in its row list) */
CFF_API int32_t StartSplice_impl(char *st, char *c) {
    char *l = NEWLINK(st);
    wrp(st, 0x60, l);
    if (!l) return 0;
    SETFL(l, FL(l) | 0x800);
    wrp(st, 0x68, c);
    if (FL(c + 0x18) & 0x1000) return 1;
    char *cur = rdp(st, 0x50);
    char *d = cur != rdp(st, 0x58) ? cur : NULL;
    wrp(st, 0x50, cur + 0x18);
    if (!d) return 0;
    memcpy(d, c, 0x18);
#ifdef CFF_CONTROL_STARTSPLICE
    return 0;
#endif
    if (FL(c) & 0x2000) return 1;
    char *rows = rdp(st, 0x18);
    long r = ROW(c);
    char *p = rdp(rows, r * 8);
    if (p == c) {
        wrp(rows, r * 8, d);
        return 1;
    }
    char *q = NEXT(p);
    while (q != c) {
        p = q;
        q = NEXT(q);
    }
    wrp(p, 0, d);
    return 1;
}
CFF_EXPORT(cff_StartSplice, StartSplice_impl);

/* 0x1800be8a0: sets pixel (x, row of the two edges' midpoint): extends a run next to one of
 * the edges, or splices a new one; 1 done, 0 nothing, 2 out of memory */
CFF_API int32_t SetPixelInDropOut_impl(char *st, int32_t x, DropEdge *e1, DropEdge *e2, int32_t both) {
#ifdef CFF_CONTROL_SETPIXELINDROPOUT
    return 1;
#endif
    int32_t row = add32(e1->x >> 1, e2->x >> 1) >> 16;
    if (EXTENDRUN(st, x, row, e1)) return 1;
    if (both == 0) return 0;
    if (EXTENDRUN(st, x, row, e2)) return 1;
    DropEdge *b = e2;
    if ((int32_t)((uint32_t)shl16(row) | 0x8000) > e1->x) {
        if (e2->x <= e1->x) b = e1;
    } else {
        b = e2->x < e1->x ? e2 : e1;
    }
    return SPLICE(st, x, row, b->cross, b->dir) ? 1 : 2;
}
DROP_EXPORT_STK(cff_SetPixelInDropOut, SetPixelInDropOut_impl);

/* 0x1800be968 */
CFF_API void ExpandCharBBox_impl(char *st, int32_t x, int32_t row) {
    if (x < rd16(st, 0x2d4)) wr16(st, 0x2d4, (uint16_t)x);
#ifndef CFF_CONTROL_EXPANDCHARBBOX
    if (x > rd16(st, 0x2d6)) wr16(st, 0x2d6, (uint16_t)x);
#endif
    if (row < rd16(st, 0x2d0)) wr16(st, 0x2d0, (uint16_t)row);
    if (row > rd16(st, 0x2d2)) wr16(st, 0x2d2, (uint16_t)row);
}
CFF_EXPORT(cff_ExpandCharBBox, ExpandCharBBox_impl);

/* 0x1800be9c8: extends a run of `row` that ends next to pixel x, found from the edge's
 * crossing in either direction; 1 when done */
CFF_API int32_t ExtendRunDropOut_impl(char *st, int32_t x, int32_t row, DropEdge *e) {
    int32_t dir = e->dir;
    char *c = e->cross;
    char *a = dir == 1 ? FORW(c) : BACK(c);
    char *run;
    if (FL(a) & 0x4000) return 0;
    if (ROW(a) == row) {
        run = RUNPAIR(st, a);
        if (!run) return 0;
        if (x == PX(run) - 1) goto set_left;
        char *n = NEXT(run);
        if (x == PX(n)) {
            SETPX(n, x + 1);
            goto done;
        }
    }
    if (FL(c) & 0x2000) c = ((~dir & 3) == 1) ? FORW(c) : BACK(c);
    if (FL(c) & 0x4000) return 0;
    if (ROW(c) != row) return 0;
    run = RUNPAIR(st, c);
    if (!run) return 0;
    if (x == PX(run) - 1) goto set_left;
    {
        char *n = NEXT(run);
        if (x != PX(n)) return 0;
        SETPX(n, x + 1);
        goto done;
    }
set_left:
#ifdef CFF_CONTROL_EXTENDRUNDROPOUT
    SETPX(run, x - 1);
#else
    SETPX(run, x);
#endif
done:
    EXPANDBBOX(st, x, row);
    return 1;
}
CFF_EXPORT(cff_ExtendRunDropOut, ExtendRunDropOut_impl);

/* 0x1800beb08: splices a one-pixel run at (x, row) into the contour of crossing c (dir:
 * which side of c the run goes); 1 done (or nothing to do), 0 out of memory */
CFF_API int32_t SplicePixel_impl(char *st, int32_t x, int32_t row, char *c, int32_t dir) {
    char *xc = XTRA(c, dir);
    char *lb, *rf;
    if (dir == 1) {
        lb = c;
        rf = xc;
    } else {
        lb = xc;
        rf = c;
    }
    if (CX(lb) == CX(rf)) return 1;
    char *a = lb;
    if (FL(lb) & 0x2000) a = BACK(lb);
    if (FL(a) & 0x4000) return 1;
    if (ROW(a) == row) {
        char *run = RUNPAIR(st, a);
        if (!run) return 0;
        if (PX(run) <= x && PX(NEXT(run)) > x) return 1;
    }
    a = rf;
    if (FL(rf) & 0x2000) a = FORW(rf);
    if (FL(a) & 0x4000) return 1;
    if (ROW(a) == row) {
        char *run = RUNPAIR(st, a);
        if (!run) return 0;
        if (PX(run) <= x && PX(NEXT(run)) > x) return 1;
    }
#ifdef CFF_CONTROL_SPLICEPIXEL
    return 1;
#endif
    EXPANDBBOX(st, x, row);
    if (!STARTSPLICE(st, lb)) return 0;
    int32_t xm = (int32_t)((uint32_t)shl16(x) | 0x8000);
    int32_t ym = (int32_t)((uint32_t)shl16(row) | 0x8000);
    int adjacent = 0;
    char *p = ROWHEAD(st, row);
    while (p) {
        int32_t lx = CX(p);
        if (lx > add32(xm, 1)) break;
        p = NEXT(p);
        if (!p) return 0;
        int32_t rx = CX(p);
        if (rx >= sub32(xm, 1)) {
            adjacent = 1;
            if (rx > add32(xm, 1) && lx < sub32(xm, 1)) break;
            for (;;) {
                char *q = NEXT(p);
                if (!q) {
                    xm = add32(rx, 2);
                    adjacent = 0;
                    break;
                }
                int32_t qx = CX(q);
                if (qx > add32(rx, 4)) {
                    xm = add32(rx, 2);
                    adjacent = 0;
                    break;
                }
                p = NEXT(q);
                rx = CX(p);
                if (rx > add32(qx, 4)) {
                    xm = add32(qx, 2);
                    break;
                }
            }
            break;
        }
        p = NEXT(p);
    }
    int32_t sx = CX(lb) <= xm ? -1 : 1;
    int32_t sy = (CY(lb) <= ym && CY(rf) <= ym) ? 1 : -1;
    if (!(FL(lb) & 0x2000) && !NEWXTRA(st, add32(sx, xm), sub32(ym, sy))) return 0;
    char *n1 = NEWCROSS(st, add32(sx, xm), ym);
    if (!n1) return 0;
    ADDY(st, n1);
    if (!NEWXTRA(st, xm, add32(sy, ym))) return 0;
    int32_t xm2 = sub32(xm, sx);
    char *n2 = NEWCROSS(st, xm2, ym);
    if (!n2) return 0;
    ADDY(st, n2);
    if (!(FL(rf) & 0x2000) && !NEWXTRA(st, xm2, sub32(ym, sy))) return 0;
    if (CX(n1) > CX(n2)) {
        char *t = n1;
        n1 = n2;
        n2 = t;
    }
    SETPX(n1, x);
    SETLEFTFLAG(st, n1);
    SETPX(n2, adjacent ? x : x + 1);
    SETLEFTFLAG(st, n2);
    if (!FINISHSPLICE(st)) return 0;
    lb = skiplink(lb);
    rf = skiplink(rf);
    if (FL(rf) & 0x2000) rf = FORW(rf);
    if (FL(lb) & 0x2000) lb = BACK(lb);
    FIXFLAGS(lb, rf);
    return 1;
}
DROP_EXPORT_STK(cff_SplicePixel, SplicePixel_impl);

/* ------------------------------------------------------------------------------------------
 * filling dropped pixels */

/* does the segment from ax to bx reach xm (the original's branch structure) */
static inline int reaches(int32_t ax, int32_t bx, int32_t xm) {
    if (ax < xm) return bx >= xm;
    if (bx <= xm) return 1;
    return ax <= xm;
}

/* y of the segment (a, b) at x = xm: fixmul(xm - ax, fixdiv(by - ay, bx - ax)) + ay */
static inline int32_t cross_y(char *a, char *b, int32_t xm) {
    int32_t q = FIXDIV(sub32(CY(b), CY(a)), sub32(CX(b), CX(a)));
    return add32(FIXMUL(sub32(xm, CX(a)), q), CY(a));
}

/* 0x1800befd4: the pixel column x between the two crossings of the contour after c with
 * the column's centre line; 1 done, 2 out of memory */
CFF_API int32_t FillInInflection_impl(char *st, char *c, char *unused, int32_t x) {
    (void)unused;
#ifdef CFF_CONTROL_FILLININFLECTION
    return 1;
#endif
    DropEdge e[2];
    memset(e, 0, sizeof e);
    int32_t xm = (int32_t)((uint32_t)shl16(x) | 0x8000);
    DropEdge *cur = &e[0];
    for (;;) {
        char *n = XTRAORIG(c, 1);
        int32_t ax = CX(c), bx = CX(n);
        if (reaches(ax, bx, xm) && ax != bx) {
            cur->x = cross_y(c, n, xm);
            cur->cross = c;
            cur->dir = 1;
            if (cur == &e[1]) break;
            if (FL(n) & 0x2000) cur = &e[1];
        }
        c = n;
    }
    int32_t ex1 = e[1].x;
    int32_t row = add32(e[0].x >> 1, ex1 >> 1) >> 16;
    if (EXTENDRUN(st, x, row, &e[0])) return 1;
    if (EXTENDRUN(st, x, row, &e[1])) return 1;
    DropEdge *b = &e[0];
    if ((int32_t)((uint32_t)shl16(row) | 0x8000) > e[0].x) {
        if (ex1 > e[0].x) b = &e[1];
    } else {
        b = ex1 >= e[0].x ? &e[0] : &e[1];
    }
    return SPLICE(st, x, row, b->cross, b->dir) ? 1 : 2;
}
CFF_EXPORT(cff_FillInInflection, FillInInflection_impl);

/* 0x1800bfbdc: the dropout in pixel column x below (up == 0) or above the contour of c0: the
 * nearest crossing of the column's centre line along c0's contour (dir) and the nearest
 * on the other contours of the two rows around it, then SetPixelInDropOut; 1 when there is
 * nothing to fill */
CFF_API int32_t FillInColumn_impl(char *st, char *c0, int32_t dir, int32_t x, int32_t up, int32_t both) {
#ifdef CFF_CONTROL_FILLINCOLUMN
    return 1;
#endif
    int32_t xm = (int32_t)((uint32_t)shl16(x) | 0x8000);
    DropEdge A, B, C;
    memset(&A, 0, sizeof A);
    memset(&B, 0, sizeof B);
    memset(&C, 0, sizeof C);
    A.x = (int32_t)0x80000000;
    B.x = 0x7fffffff;
    C.x = (int32_t)(0x7fffffffu + (up != 0));

    /* along c0's own contour */
    char *cur = c0;
    for (;;) {
        char *n = XTRAORIG(cur, dir);
        int32_t ax = CX(cur), bx = CX(n);
        if (reaches(ax, bx, xm) && ax != bx) {
            int32_t y = cross_y(cur, n, xm);
            if (up == 0) {
                if (y < C.x) {
                    B = C;
                    C.x = y;
                    C.cross = cur;
                    C.dir = dir;
                } else if (y < B.x) {
                    B.x = y;
                    B.cross = cur;
                    B.dir = dir;
                }
            } else {
                if (y > C.x) {
                    A = C;
                    C.x = y;
                    C.cross = cur;
                    C.dir = dir;
                } else if (y > A.x) {
                    A.x = y;
                    A.cross = cur;
                    A.dir = dir;
                }
            }
        }
        cur = n;
        if (!(FL(n) & 0x2000)) break;
    }
    if ((uint32_t)C.x + 0x7fffffffu > 0xfffffffdu) return 1;

    /* the other contours through the rows around c0 */
    int32_t cy0 = CY(c0);
    int32_t r8 = cy0 >> 16;
    char *xc = XTRA(c0, dir);
    int16_t row = CY(xc) <= cy0 ? (int16_t)r8 : (int16_t)(r8 + 1);
    for (int pass = 0; pass < 2; pass++) {
        char *n0 = ROWHEAD(st, (long)row - pass);
        for (; n0; n0 = NEXT(n0)) {
            if (n0 == c0) continue;
            uint16_t f = FL(n0);
            int32_t d;
            if (pass == 0) {
                d = f & 3;
                if (d == 0) {
                    d = (f >> 2) & 3;
                    if (d == 0) continue;
                    if (d == 3) {
                        char *t = XTRAORIG(n0, 1);
                        d = (CY(t) >= CY(n0)) + 1;
                    } else {
                        char *t = XTRAORIG(n0, d);
                        if (CY(t) > CY(n0)) continue;
                    }
                }
            } else {
                d = (f >> 2) & 3;
                if (d == 0) continue;
                if (d == 3) {
                    char *t = XTRAORIG(n0, 1);
                    d = (CY(t) <= CY(n0)) + 1;
                } else {
                    char *t = XTRAORIG(n0, d);
                    if (CY(t) < CY(n0)) continue;
                }
            }
            char *s = n0;
            if (f & 0x800) {
                s = d == 1 ? FORW(n0) : BACK(n0);
                if (FL(s) & 0x4800) continue;
                d = ~d & 3;
            }
            for (;;) {
                char *t = XTRAORIG(s, d);
                if (FL(t) & 0x4000) break;
                int32_t ax = CX(s), bx = CX(t);
                if (reaches(ax, bx, xm) && ax != bx) {
                    int32_t y = cross_y(s, t, xm);
                    if (up == 0) {
                        if (y > A.x) {
                            A.x = y;
                            A.cross = s;
                            A.dir = d;
                        }
                    } else if (y < B.x) {
                        B.x = y;
                        B.cross = s;
                        B.dir = d;
                    }
                }
                s = t;
                if (!(FL(t) & 0x2000)) break;
            }
        }
    }

    DropEdge *pick;
    if (up == 0) {
        if (A.x != (int32_t)0x80000000)
            pick = &A;
        else if (B.x == 0x7fffffff)
            return 1;
        else
            pick = &B;
    } else {
        if (B.x != 0x7fffffff)
            pick = &B;
        else if (A.x != (int32_t)0x80000000)
            pick = &A;
        else
            return 1;
    }
    return SETPIXEL(st, x, &C, pick, both);
}
DROP_EXPORT_STK(cff_FillInColumn, FillInColumn_impl);

/* 0x1800c1ea8: FillInColumn for the columns x0..x1, from the left while it finds
 * something, then from the right; 0 out of memory */
CFF_API int32_t FillInRange_impl(char *st, char *c, int32_t dir, int32_t x0, int32_t x1, int32_t up) {
#ifdef CFF_CONTROL_FILLINRANGE
    return 1;
#endif
    for (;;) {
        int32_t r = FILLCOL(st, c, dir, x0, up, 0);
        if (r == 2) return 0;
        if (r == 0) break;
        c = skiplink(c);
        x0 = add32(x0, 1);
        if (x0 > x1) return 1;
    }
    for (;;) {
        if (x0 > x1) return 1;
        c = skiplink(c);
        if (FILLCOL(st, c, dir, x1, up, 1) == 2) return 0;
        x1 = sub32(x1, 1);
    }
}
DROP_EXPORT_STK(cff_FillInRange, FillInRange_impl);

/* 0x1800c1af8: fills the columns between two runs r1, r2 of adjacent rows */
CFF_API int32_t FillInPairs_impl(char *st, char *r1, char *r2, char *c, int32_t dir, int32_t up) {
#ifdef CFF_CONTROL_FILLINPAIRS
    return 1;
#endif
    if (PX(r1) < PX(r2)) return FILLRANGE(st, c, dir, PX(NEXT(r1)), PX(r2) - 1, up) ? 1 : 0;
    return FILLRANGE(st, c, dir, PX(NEXT(r2)), PX(r1) - 1, up == 0) ? 1 : 0;
}
DROP_EXPORT_STK(cff_FillInPairs, FillInPairs_impl);

/* ------------------------------------------------------------------------------------------
 * black space: runs of adjacent rows that do not touch */

/* 0x1800c10fc: the run on the other side of crossing c (up == 0: the row above through
 * PathUpCross; else along c's contour), if it is not empty */
CFF_API char *ConnectedRunOpposite_impl(char *st, char *c, int32_t dir) {
#ifdef CFF_CONTROL_CONNECTEDRUNOPPOSITE
    return NULL;
#endif
    if (!c) return NULL;
    uint16_t f = FL(c);
    char *a;
    if (dir == 0) {
        if (!(f & 0x100)) return NULL;
        a = PATHUP(c);
    } else {
        int d = f & 3;
        if (!d) return NULL;
        a = d == 1 ? FORW(c) : BACK(c);
    }
    if (!a) return NULL;
    if (FL(a) & 0x4000) return NULL;
    char *run = RUNPAIR(st, a);
    if (!run) return NULL;
    if (PX(run) >= PX(NEXT(run))) return NULL;
    return run;
}
CFF_EXPORT(cff_ConnectedRunOpposite, ConnectedRunOpposite_impl);

/* 0x1800c0bbc: the white gap between runs r1 and r2 of one row, bridged by the runs of the
 * row the contour of c leads to: fills the columns not covered; 0 out of memory */
CFF_API int32_t CheckHorizWhite_impl(char *st, char *r1, char *r2, char *c, int32_t dir) {
#ifdef CFF_CONTROL_CHECKHORIZWHITE
    return 1;
#endif
    char *xc = XTRA(c, dir);
    int32_t ycur = CY(c), yx = CY(xc);
    int32_t last = PX(r2) - 1;
    int32_t flag = yx < ycur;
    int32_t at = PX(NEXT(r1));
    char *ra = CRO(st, r1, flag);
    char *rb = CRO(st, NEXT(r2), flag);
    int32_t up = yx >= ycur;
    if (rb != NULL && ra == NULL) goto right;
    if (rb != NULL && PX(rb) >= PX(ra)) goto left;
    {
        char *t = rb;
        rb = ra;
        ra = t;
        if (ra == NULL) goto check;
    }
left:
    if (PX(ra) > at) {
        int32_t e = PX(ra) - 1;
        if (!FILLRANGE(st, c, dir, at, e < last ? e : last, up)) return 0;
        c = skiplink(c);
        ra = skiplink(ra);
    }
    {
        int32_t v = PX(NEXT(ra));
        if (v > at) at = v;
    }
check:
    if (rb == NULL) goto tail;
right:
    if (at > last) return 1;
    rb = skiplink(rb);
    if (PX(rb) > at) {
        int32_t e = PX(rb) - 1;
        if (!FILLRANGE(st, c, dir, at, e < last ? e : last, up)) return 0;
        c = skiplink(c);
        rb = skiplink(rb);
    }
    {
        int32_t v = PX(NEXT(rb));
        if (v <= at) goto fill;
        at = v;
    }
tail:
    if (at > last) return 1;
fill:
    return FILLRANGE(st, c, dir, at, last, up) ? 1 : 0;
}
DROP_EXPORT_STK(cff_CheckHorizWhite, CheckHorizWhite_impl);

/* 0x1800c118c: two runs of adjacent rows reached from c1 and c2: when they do not overlap,
 * fills the columns between them; 0 out of memory */
CFF_API int32_t CheckBlackDown_impl(char *st, char *c1, char *c2) {
#ifdef CFF_CONTROL_CHECKBLACKDOWN
    return 1;
#endif
    uint16_t f = FL(c1);
    if (f & 0x4000) return 1;
    if (FL(c2) & 0x4000) return 1;
    char *r1 = c1;
    if (!(f & 0x400)) r1 = RUNPAIR(st, c1);
    char *r2 = c2;
    if (!(FL(c2) & 0x400)) r2 = RUNPAIR(st, c2);
    if (!r1 || !r2) return 0;
    if (PX(r1) > PX(NEXT(r2)) || PX(NEXT(r1)) < PX(r2))
        return FILLPAIRS(st, r1, r2, c1, f & 3, c1 == r1) ? 1 : 0;
    return 1;
}
CFF_EXPORT(cff_CheckBlackDown, CheckBlackDown_impl);

/* 0x1800c1244: recomputes the direction flags of the crossings from c to end along the
 * contour; returns the crossing after end (the original's RAX) */
CFF_API char *FixCrossFlags_impl(char *c, char *end) {
    for (;;) {
        uint32_t f = FL(c) & 0xfef0u;
        char *b = BACK(c);
        int32_t y = CY(c);
        if (y > CY(b))
            f |= 2;
        else if (y < CY(b))
            f |= 0x100;
        else if (CX(c) < CX(b) || (CX(c) == CX(b) && (uintptr_t)c < (uintptr_t)b))
            f |= 8;
        char *fw = FORW(c);
        if (y > CY(fw))
            f |= 1;
        else if (y < CY(fw))
            f |= 0x100;
        else if (CX(c) < CX(fw) || (CX(c) == CX(fw) && (uintptr_t)c < (uintptr_t)fw))
            f |= 4;
#ifdef CFF_CONTROL_FIXCROSSFLAGS
        f ^= 0x100;
#endif
        SETFL(c, f);
        if (c == end) return fw;
        c = fw;
    }
}
CFF_EXPORT(cff_FixCrossFlags, FixCrossFlags_impl);

/* 0x1800c12e0: the columns beside the x extrema of the contours (the lists at +0x30, +0x38)
 * that the runs around them do not cover; 0 out of memory */
CFF_API int32_t DoXInflections_impl(char *st) {
#ifdef CFF_CONTROL_DOXINFLECTIONS
    return 1;
#endif
    for (char *n = rdp(st, 0x30); n; n = NEXT(n)) {
        char *c = n;
        if (FL(c) & 0x1000) {
            c = skiplink(c);
            while (!(FL(c) & 0x2000)) {
                c += 0x18;
                if (FL(c) & 0x1000) c += CX(c);
            }
        }
        int32_t cx = CX(c);
        if ((uint16_t)cx == 0x8000) {
            cx = add32(cx, 1);
            wr32(c, 8, cx);
        }
        char *f = FORW(c);
        if (FL(f) & 0x4000) continue;
        if (!(FL(f) & 0x400)) f = RUNPAIR(st, f);
        if (!f) return 0;
        char *b = BACK(c), *bb = b;
        if (FL(b) & 0x4000) continue;
        if (!(FL(b) & 0x400)) b = RUNPAIR(st, b);
        if (!b) return 0;
        if (PX(f) > PX(b)) f = b;
        int32_t lim = add32(cx, 0x8000) >> 16;
        for (int32_t px = PX(f) - 1; px >= lim; px--) {
            if (FILLINFL(st, bb, c, px) == 2) return 0;
            bb = skiplink(bb);
        }
    }
    for (char *n = rdp(st, 0x38); n; n = NEXT(n)) {
        char *c = n;
        if (FL(c) & 0x1000) {
            c = skiplink(c);
            while (!(FL(c) & 0x2000)) c = XTRA(c, 1);
        }
        int32_t cx = CX(c);
        if ((uint16_t)cx == 0x8000) {
            cx = sub32(cx, 1);
            wr32(c, 8, cx);
        }
        char *f = FORW(c);
        if (FL(f) & 0x4000) continue;
        if (!(FL(f) & 0x400)) f = RUNPAIR(st, f);
        if (!f) return 0;
        char *b = BACK(c), *bb = b;
        if (FL(b) & 0x4000) continue;
        if (!(FL(b) & 0x400)) b = RUNPAIR(st, b);
        if (!b) return 0;
        char *e = NEXT(f), *eb = NEXT(b);
        if (PX(e) < PX(eb)) e = eb;
        int32_t lim = (add32(cx, 0x8000) >> 16) - 1;
        for (int32_t px = PX(e); px <= lim; px++) {
            if (FILLINFL(st, bb, c, px) == 2) return 0;
            bb = skiplink(bb);
        }
    }
    return 1;
}
CFF_EXPORT(cff_DoXInflections, DoXInflections_impl);

/* 0x1800c196c: horizontal neighbour of run edge c (dir 1 forward, 2 back, 3 both): when
 * the runs of the two rows leave a white gap, CheckHorizWhite; 0 out of memory */
CFF_API int32_t EditBlackHoriz_impl(char *st, char *c, int32_t dir) {
#ifdef CFF_CONTROL_EDITBLACKHORIZ
    return 1;
#endif
    uint16_t f = FL(c);
    if (f & 0x4000) return 1;
    if (dir == 3) {
        if (!BLACKHORIZ(st, c, 1)) return 0;
        c = skiplink(c);
        return BLACKHORIZ(st, c, 2) ? 1 : 0;
    }
    char *a = dir == 1 ? FORW(c) : BACK(c);
    uint16_t af = FL(a);
    if (af & 0x4000) return 1;
    if ((f & 0x400) && !(af & 0x400)) return 1;
    char *r1 = c;
    if (!(f & 0x400)) r1 = RUNPAIR(st, c);
    if (!(af & 0x400)) a = RUNPAIR(st, a);
    if (!r1 || !a) return 0;
    if (PX(NEXT(r1)) >= PX(a)) return 1;
    return HORIZWHITE(st, r1, a, c, dir) ? 1 : 0;
}
CFF_EXPORT(cff_EditBlackHoriz, EditBlackHoriz_impl);

/* 0x1800c1598: dropout control in black space, from the last row to the first; then
 * DoXInflections. 0 out of memory */
CFF_API int32_t EditBlackSpace_impl(char *st) {
    long top = rd16(st, 0x2d2);
    char *rowp = rdp(st, 0x18) + top * 8;
    int32_t cnt = (int32_t)top - rd16(st, 0x2d0);
#ifdef CFF_CONTROL_EDITBLACKSPACE
    cnt = -1;
#endif
    for (; cnt >= 0; cnt--) {
        char *l = rdp(rowp, 0);
        rowp -= 8;
        while (l) {
            char *r = NEXT(l);
            char *rb = NOCROSS;
            int d = FL(r) & 3;
            if (d) rb = d == 1 ? FORW(r) : BACK(r);
            uint16_t lf = FL(l);
            if ((lf & 3) == 0) {
                if (rb != NOCROSS) {
                    if (!BLACKDOWN(st, r, rb)) return 0;
                    l = skiplink(l);
                    r = skiplink(r);
                }
            } else {
                char *lb;
                if ((lf & 3) == 1) {
                    lb = l + 0x18;
                    while (FL(lb) & 0x2000) lb += (FL(lb) & 0x1000) ? CX(lb) : 0x18;
                } else {
                    lb = BACK(l);
                }
                if (rb != NOCROSS && (FL(lb) & 0x400) && NEXT(lb) == rb) {
                    if (PX(l) > PX(rb) || PX(r) < PX(lb)) {
                        if (!FILLPAIRS(st, l, lb, l, lf & 3, 1)) return 0;
                        l = skiplink(l);
                        r = skiplink(r);
                    }
                } else {
                    if (!BLACKDOWN(st, l, lb)) return 0;
                    l = skiplink(l);
                    r = skiplink(r);
                    if (rb != NOCROSS && (FL8(r) & 3)) {
                        rb = skiplink(rb);
                        if (!BLACKDOWN(st, r, rb)) return 0;
                        l = skiplink(l);
                        r = skiplink(r);
                    }
                }
            }
            uint16_t f2 = FL(l);
            if (f2 & 0xc) {
                if (!BLACKHORIZ(st, l, (f2 >> 2) & 3)) return 0;
                r = skiplink(r);
            }
            f2 = FL(r);
            if (f2 & 0xc) {
                if (!BLACKHORIZ(st, r, (f2 >> 2) & 3)) return 0;
                r = skiplink(r);
            }
            l = NEXT(r);
        }
    }
    return DOXINFL(st);
}
CFF_EXPORT(cff_EditBlackSpace, EditBlackSpace_impl);

/* ------------------------------------------------------------------------------------------
 * white space: gaps between runs, repaired by pixel patterns */

/* 0x1800c1f84 */
CFF_API void ExpandWhiteDropOuts_impl(char *st, char *ctx) {
#ifdef CFF_CONTROL_EXPANDWHITEDROPOUTS
    return;
#endif
    long top = rd16(st, 0x2d2);
    char *rowp = rdp(st, 0x18) + top * 8;
    int32_t cnt = (int32_t)top - rd16(st, 0x2d0);
    if (cnt < 0) return;
    do {
        char *l = rdp(rowp, 0);
        rowp -= 8;
        if (l) {
            char *r = NEXT(l), *n;
            while ((n = NEXT(r)) != NULL) {
                if (PX(r) >= PX(n) && !(FL8(r) & 0xc)) FIXRUNS(st, l, n, ctx);
                r = NEXT(n);
                l = n;
            }
        }
        cnt -= 1;
    } while (cnt >= 0);
}
CFF_EXPORT(cff_ExpandWhiteDropOuts, ExpandWhiteDropOuts_impl);

/* 0x1800c1db0 */
CFF_API void EditWhiteHoriz_impl(char *st, char *c, int32_t dir, char *ctx) {
#ifdef CFF_CONTROL_EDITWHITEHORIZ
    return;
#endif
    char *a;
    if (dir == 3) {
        WHITEHORIZ(st, c, 1, ctx);
        dir = 2;
        a = BACK(c);
    } else if (dir == 1) {
        a = FORW(c);
    } else {
        a = BACK(c);
    }
    if (FL(a) & 0x400) return;
    int32_t lx = PX(c);
    int32_t y = CY(c);
    int32_t rx = PX(NEXT(c));
    char *t = XTRA(c, dir);
    int32_t row = add32(CY(t) > y ? 1 : -1, y >> 16);
    char *p = ROWHEAD(st, row);
    while (p) {
        if (PX(p) > rx) return;
        char *q = NEXT(p);
        if (PX(q) >= lx) {
            FIXRUNS(st, c, p, ctx);
            lx = PX(c);
            rx = PX(NEXT(c));
        }
        p = NEXT(q);
    }
}
CFF_EXPORT(cff_EditWhiteHoriz, EditWhiteHoriz_impl);

/* 0x1800c1b9c: dropout control in white space (CScan calls it at small sizes) */
CFF_API void EditWhiteSpace_impl(char *st, char *ctx) {
#ifdef CFF_CONTROL_EDITWHITESPACE
    return;
#endif
    EXPANDWHITE(st, ctx);
    long top = rd16(st, 0x2d2);
    char *rowp = rdp(st, 0x18) + top * 8;
    int32_t cnt = (int32_t)top - rd16(st, 0x2d0);
    if (cnt < 0) return;
    do {
        char *l = rdp(rowp, 0);
        rowp -= 8;
        if (l) {
            for (;;) {
                uint16_t f = FL(l);
                if (f & 0xc) WHITEHORIZ(st, l, (f >> 2) & 3, ctx);
                char *r = NEXT(l);
                l = NEXT(r);
                if (!l) break;
                char *lp = NOCROSS;
                int d = FL(l) & 3;
                if (d) lp = d == 1 ? FORW(l) : BACK(l);
                d = FL(r) & 3;
                if (d) {
                    char *rp = d == 1 ? FORW(r) : BACK(r);
                    if (lp != NOCROSS && (FL(rp) & 0x400) && NEXT(rp) == lp) {
                        if (PX(r) > PX(lp) || PX(l) < PX(rp)) CONNECTWHITE(st, r, rp, ctx);
                        continue;
                    }
                    if (!(FL(rp) & 0x400)) {
                        char *rn = NEXT(rp);
                        if (rn && (PX(r) >= PX(rn) || PX(l) <= PX(rp))) CONNECTWHITE(st, r, rp, ctx);
                    }
                }
                if (lp == NOCROSS || !(FL(lp) & 0x400)) continue;
                char *q = rdp(rowp, 0);
                if (q == lp) continue;
                for (;;) {
                    char *qn = NEXT(q);
                    if (qn == lp) break;
                    q = qn;
                    if (!q) return;
                }
                if (PX(r) >= PX(lp) || PX(l) <= PX(q)) CONNECTWHITE(st, r, q, ctx);
            }
        }
        cnt -= 1;
    } while (cnt >= 0);
}
CFF_EXPORT(cff_EditWhiteSpace, EditWhiteSpace_impl);

/* 0x180184568: two runs of adjacent rows that touch at a corner only */
CFF_API void ConnectWhitePairs_impl(char *st, char *r1, char *r2, char *ctx) {
#ifdef CFF_CONTROL_CONNECTWHITEPAIRS
    return;
#endif
    char *e = NEXT(r1);
    int16_t p1 = PX(r1);
    if (p1 >= PX(e)) return;
    char *e2 = NEXT(r2);
    int16_t p2 = PX(r2);
    if (p2 >= PX(e2)) return;
    char *s = r2;
    if (p1 > p2) {
        s = r1;
        e = e2;
    }
    if (PX(e) != PX(s)) return;
    FIXRUNS(st, RUNPAIR(st, s), e, ctx);
}
CFF_EXPORT(cff_ConnectWhitePairs, ConnectWhitePairs_impl);

/* 0x1800c2028: runs a and b of adjacent rows meeting at a pixel corner: FixPixelProblem at
 * the pixels concerned */
CFF_API void FixRuns_impl(char *st, char *a, char *b, char *ctx) {
#ifdef CFF_CONTROL_FIXRUNS
    return;
#endif
    if (!a) return;
    char *an = NEXT(a);
    int16_t ap = PX(a);
    if (ap >= PX(an)) return;
    char *bn = NEXT(b);
    int16_t bp = PX(b);
    if (bp >= PX(bn)) return;
    char *lo, *loend, *hi;
    if (ap <= bp) {
        lo = a;
        loend = an;
        hi = b;
    } else {
        lo = b;
        loend = bn;
        hi = a;
    }
    int32_t hp = PX(hi);
    if (PX(loend) == (int16_t)hp) {
        FIXPIXEL(st, lo, PX(loend) - 1, hi, hp, ctx);
        return;
    }
    if (CY(lo) == CY(hi)) return;
    int32_t e1 = PX(loend) - 1;
    if (e1 == hp) {
        FIXPIXEL(st, lo, hp, hi, hp, ctx);
        return;
    }
    int32_t lp = PX(lo);
    if (lp == e1) {
        FIXPIXEL(st, lo, lp, hi, lp, ctx);
        return;
    }
    if (hp == PX(NEXT(hi)) - 1) FIXPIXEL(st, lo, hp, hi, hp, ctx);
}
CFF_EXPORT(cff_FixRuns, FixRuns_impl);

/* 0x1800c2c58: the neighbourhood bits of the runs from cl to cr around column x (mode -1,
 * 0, 1: the row below, the row, the row above) */
CFF_API uint8_t ScanLineCxtn_impl(char *cl, char *cr, int32_t x, int32_t mode, int32_t adj) {
#ifdef CFF_CONTROL_SCANLINECXTN
    return 0;
#endif
    if (CX(cr) < CX(cl)) return 0;
    int done = 0, seen = 0;
    uint8_t acc = 0;
    char *p = cl;
    for (;;) {
        char *n = NEXT(p);
        int32_t lo = sub32(PX(p), x);
        int32_t hi = sub32(PX(n), x);
        int32_t e = sub32(hi, 1);
        if (adj != 0) {
            if (!(FL(n) & 0x200)) hi = e;
            e = hi;
            if (FL(p) & 0x200) lo = sub32(lo, 1);
        }
        if (lo < -1) lo = -1;
        if (e > 1) e = 1;
        if (!(FL8(n) & 0xc)) {
            if (e == -1) {
                seen = 1;
                if (mode == 0) goto next;
            } else if (e == 0) {
                if (seen || mode == 0) done = 1;
            }
        }
        for (long d = lo; d <= e; d++) acc |= rdata_20a300[0x2c + 3 * (long)mode + d];
        if (done) return acc;
    next:
        if (n == cr) return acc;
        p = NEXT(n);
        if (!p) return acc;
    }
}
DROP_EXPORT_STK(cff_ScanLineCxtn, ScanLineCxtn_impl);

/* 0x1800c2250: the context byte of the pixel corner at crossing c, column x: the
 * neighbourhood bits of the three rows around it, 0xff when it is not a simple corner */
CFF_API uint8_t BuildCxtn_impl(char *st, char *c, int32_t x, int32_t adj) {
#ifdef CFF_CONTROL_BUILDCXTN
    return 0xff;
#endif
    char *cn = NEXT(c);
    int32_t cx = CX(c), cy = CY(c);
    uint8_t acc = 0;
    char *maxl = NULL, *minr = NULL, *left = NULL;
    if (cx == CX(cn)) return 0xff;
    int below = 1, above = 1;
    char *head = ROWHEAD(st, cy >> 16);
    for (char *p = head; p != cn; p = NEXT(p)) {
        uint16_t f = FL(p);
        if (f & 4) {
            char *q = FORW(p);
            if (CX(q) > cx) {
                if (!((FL(q) ^ f) & 0x400)) return 0xff;
                char *t = XTRA(p, 1);
                if (CY(t) <= cy)
                    below = 0;
                else
                    above = 0;
            }
        }
        if (f & 8) {
            char *q = BACK(p);
            if (CX(q) > cx) {
                if (!((FL(q) ^ f) & 0x400)) return 0xff;
                char *t = p - 0x18;
                if (FL(t) & 0x1000) t += CY(t);
                if (CY(t) <= cy)
                    below = 0;
                else
                    above = 0;
            }
        }
    }
    int32_t m;
    if (below) {
        if (head == cn) return 0xff;
        m = (int32_t)0x80000000;
        for (char *p = head; p != cn; p = NEXT(p)) {
            int d = FL(p) & 3;
            if (d) {
                char *q = d == 1 ? FORW(p) : BACK(p);
                if (CX(q) > m) {
                    m = CX(q);
                    maxl = p;
                }
            }
        }
        if (!maxl) return 0xff;
        char *p = NEXT(maxl);
        if (!p) return 0xff;
        m = 0x7fffffff;
        for (; p; p = NEXT(p)) {
            int d = FL(p) & 3;
            if (d) {
                char *q = d == 1 ? FORW(p) : BACK(p);
                if (CX(q) < m) {
                    m = CX(q);
                    minr = p;
                }
            }
        }
        cn = NEXT(c);
        if (!minr) return 0xff;
        left = maxl;
        char *a = (FL8(maxl) & 3) == 1 ? FORW(maxl) : BACK(maxl);
        char *b = (FL8(minr) & 3) == 1 ? FORW(minr) : BACK(minr);
        if (!(FL(a) & 0x400)) return 0xff;
        if (FL(b) & 0x400) return 0xff;
        if (CX(a) >= CX(b)) return 0xff;
        acc = SCANLINE(a, b, x, -1, adj);
        if (acc == 0xff) return 0xff;
    } else {
        if (FL8(c) & 3) return 0xff;
        if (FL8(cn) & 3) return 0xff;
    }
    if (above) {
        char *bl = NULL, *br = NULL;
        if (head == cn) return 0xff;
        m = (int32_t)0x80000000;
        for (char *p = head; p != cn; p = NEXT(p)) {
            if (FL(p) & 0x100) {
                char *t = p + 0x18;
                if (FL(t) & 0x1000) t += CX(t);
                char *q = CY(t) > CY(p) ? FORW(p) : BACK(p);
                if (CX(q) > m) {
                    m = CX(q);
                    bl = p;
                }
            }
        }
        if (!bl) return 0xff;
        char *p = NEXT(bl);
        if (!p) return 0xff;
        m = 0x7fffffff;
        for (; p; p = NEXT(p)) {
            if (FL(p) & 0x100) {
                char *t = p + 0x18;
                if (FL(t) & 0x1000) t += CX(t);
                char *q = CY(t) > CY(p) ? FORW(p) : BACK(p);
                if (CX(q) < m) {
                    m = CX(q);
                    br = p;
                }
            }
        }
        if (!br) return 0xff;
        if (below) {
            left = CX(bl) >= CX(left) ? left : bl;
            minr = CX(br) <= CX(minr) ? minr : br;
        } else {
            left = bl;
            minr = br;
        }
        char *t = bl + 0x18;
        if (FL(t) & 0x1000) t += CX(t);
        char *a = CY(t) > CY(bl) ? FORW(bl) : BACK(bl);
        t = br + 0x18;
        if (FL(t) & 0x1000) t += CX(t);
        char *b = CY(t) > CY(br) ? FORW(br) : BACK(br);
        if (!(FL(a) & 0x400)) return 0xff;
        if (FL(b) & 0x400) return 0xff;
        if (CX(a) >= CX(b)) return 0xff;
        acc |= SCANLINE(a, b, x, 1, adj);
        if (acc == 0xff) return 0xff;
    } else {
        if (FL(c) & 0x100) return 0xff;
        if (FL(cn) & 0x100) return 0xff;
        if (!below) {
            left = head;
            minr = head;
            for (char *p = NEXT(head); p; p = NEXT(p)) minr = p;
        }
    }
    if (!(FL(left) & 0x400)) return 0xff;
    if (FL(minr) & 0x400) return 0xff;
    acc |= SCANLINE(left, minr, x, 0, adj);
    return acc;
}
CFF_EXPORT(cff_BuildCxtn, BuildCxtn_impl);

/* 0x1800c2d74: writes the actions of pattern `entry` rotated back by rot */
CFF_API void PatternToActions_impl(uint8_t *desc, const uint8_t *entry, int32_t rot) {
    uint8_t r = (uint8_t)((0u - (uint32_t)rot) & 7);
    uint8_t *o = desc + 1;
    if (entry[2] != 0xd) {
        uint8_t code = entry[2];
        o[0] = data_231cc0[code * 2];
        o[1] = data_231cc0[code * 2 + 1];
        o[2] = (uint8_t)((r + entry[3]) & 7);
        o += 3;
    } else {
        const uint8_t *l = data_231bb8 + entry[3] * 8;
        while (*l != 0xd) {
            uint8_t code = *l;
            l += 2;
            o[0] = data_231cc0[code * 2];
            o[1] = data_231cc0[code * 2 + 1];
            o[2] = (uint8_t)((r + l[-1]) & 7);
            o += 3;
        }
    }
#ifdef CFF_CONTROL_PATTERNTOACTIONS
    desc[2] = 0;
#endif
    o[1] = 0;
}
CFF_EXPORT(cff_PatternToActions, PatternToActions_impl);

/* 0x1800c20f4: the rule description of pixel (x, row of c) against the pixel (xo, row of
 * co): the context byte rotated to its smallest form, looked up in the pattern table */
CFF_API void GetRuleDesc_impl(char *st, uint8_t *desc, char *c, int32_t x, char *co, int32_t xo) {
    uint8_t s = BUILDCXTN(st, c, x, 1);
    desc[0] = s;
    if (s == 0xff) {
        desc[2] = 0;
        return;
    }
    int32_t idx = add32(add32(xo, 4), sub32((ROW(co) - ROW(c)) * 3, x));
    int32_t t = rd32((const char *)rdata_20a300, (long)idx * 4);
    int32_t rot;
    if (s == 0) goto zero;
    rot = 0;
    {
        uint8_t dl = s;
        for (int k = 2; k < 8; k += 2) {
            dl = (uint8_t)((dl >> 6) | (uint8_t)(dl << 2));
            if (dl < s && (dl & 3)) {
                s = dl;
                rot = k;
            }
        }
    }
    if ((uint8_t)(s - 1) <= 1 && BUILDCXTN(st, c, x, 0) == 0) {
        s = 0;
        goto zero;
    }
    goto key;
zero:
    rot = (int32_t)((0xfffffffeu - (uint32_t)t) & 6);
key:;
    uint16_t k = (uint16_t)((((uint32_t)t + (uint32_t)rot) & 7) | ((uint32_t)s << 8));
#ifdef CFF_CONTROL_GETRULEDESC
    k ^= 1;
#endif
    int lo = 0, hi = 0x2e;
    for (;;) {
        int mid = (hi + lo) >> 1;
        const uint8_t *e = data_231c00 + mid * 4;
        uint16_t ek = (uint16_t)(e[0] | e[1] << 8);
        if (ek < k) {
            if (mid == hi) break;
            lo = mid + 1;
        } else if (ek == k) {
            PATTERN(desc, e, rot);
            return;
        } else {
            if (mid == lo) break;
            hi = mid - 1;
        }
    }
    desc[2] = 0;
}
DROP_EXPORT_STK(cff_GetRuleDesc, GetRuleDesc_impl);

/* 0x1800c2c04: is pixel (x, row) black */
CFF_API int32_t PixelIsBlack_impl(char *st, int32_t x, int32_t row) {
#ifdef CFF_CONTROL_PIXELISBLACK
    return 0;
#endif
    if (row < rd16(st, 0x2d0) || row > rd16(st, 0x2d2)) return 0;
    char *p = ROWHEAD(st, row);
    while (p) {
        if (PX(p) > x) return 0;
        p = NEXT(p);
        if (PX(p) > x) return 1;
        p = NEXT(p);
    }
    return 0;
}
CFF_EXPORT(cff_PixelIsBlack, PixelIsBlack_impl);

/* 0x1800c2e18: may the end pixel x of run c be deleted (not adjusted before) */
CFF_API int32_t CheckDeletePixel_impl(char *c, int32_t x) {
#ifdef CFF_CONTROL_CHECKDELETEPIXEL
    return 0;
#endif
    if (x == PX(c)) return (FL(c) & 0x200) ? 0 : 1;
    char *n = NEXT(c);
    if (x != PX(n) - 1) return 0;
    return ((uint16_t)~FL(n) >> 9) & 1;
}
CFF_EXPORT(cff_CheckDeletePixel, CheckDeletePixel_impl);

/* 0x18010c47c: extends a run of `row` to the left onto pixel x */
CFF_API int32_t FixupExtendLeft_impl(char *st, int32_t x, int32_t row) {
#ifdef CFF_CONTROL_FIXUPEXTENDLEFT
    return 0;
#endif
    char *p = ROWHEAD(st, row);
    while (p) {
        int32_t px = PX(p);
        if (px == x + 1) {
            SETPX(p, x);
            return 1;
        }
        char *n = NEXT(p);
        if ((FL(p) & 0x200) && px == x + 2 && (int16_t)px >= PX(n)) {
            SETPX(n, x + 1);
            SETFL(p, FL(p) & 0xfdff);
            SETFL(n, FL(n) | 0x200);
            SETPX(p, x);
            return 1;
        }
        p = NEXT(n);
    }
    return 0;
}
CFF_EXPORT(cff_FixupExtendLeft, FixupExtendLeft_impl);

/* 0x18018465c: extends a run of `row` to the right onto pixel x */
CFF_API int32_t FixupExtendRight_impl(char *st, int32_t x, int32_t row) {
#ifdef CFF_CONTROL_FIXUPEXTENDRIGHT
    return 0;
#endif
    char *p = ROWHEAD(st, row);
    while (p) {
        char *n = NEXT(p);
        int32_t nx = PX(n);
        if (nx == x) {
            SETPX(n, x + 1);
            return 1;
        }
        if ((FL(n) & 0x200) && nx == x - 1 && PX(p) >= (int16_t)nx) {
            SETFL(p, FL(p) | 0x200);
            SETPX(p, x);
            SETFL(n, FL(n) & 0xfdff);
            SETPX(n, x + 1);
            return 1;
        }
        p = NEXT(n);
    }
    return 0;
}
CFF_EXPORT(cff_FixupExtendRight, FixupExtendRight_impl);

/* 0x1800c28e0: applies action `entry` of rule description desc to the end pixel x of run
 * c: delete it, or move it one pixel (rotation 1, 3, 5, 7: down, right, up, left) when the
 * pixels around the new place are white; 1 when applied */
CFF_API int32_t DoFixupAction_impl(char *st, uint8_t *desc, uint8_t *entry, char *c, int32_t x) {
#ifdef CFF_CONTROL_DOFIXUPACTION
    return 0;
#endif
    int32_t row = ROW(c);
    if (entry[0] == 1) {
        if (!CHECKDELETE(c, x)) return 0;
        goto apply;
    }
    switch (entry[2]) {
    case 3:
        if (PX(c) != PX(NEXT(c)) - 1) return 0;
        if (x == rd16(st, 0x2d6)) return 0;
        if (PIXELBLACK(st, x + 2, row - 1) || PIXELBLACK(st, x + 2, row) || PIXELBLACK(st, x + 2, row + 1) ||
            PIXELBLACK(st, x + 1, row))
            return 0;
        {
            char *n = NEXT(c);
            SETPX(c, PX(c) + 1);
            SETPX(n, PX(n) + 1);
        }
        return 1;
    case 7:
        if (PX(c) != PX(NEXT(c)) - 1) return 0;
        if (x == rd16(st, 0x2d4)) return 0;
        if (PIXELBLACK(st, x - 2, row - 1) || PIXELBLACK(st, x - 2, row) || PIXELBLACK(st, x - 2, row + 1) ||
            PIXELBLACK(st, x - 1, row))
            return 0;
        {
            char *n = NEXT(c);
            SETPX(c, PX(c) - 1);
            SETPX(n, PX(n) - 1);
        }
        return 1;
    case 1:
        if (row == rd16(st, 0x2d2)) return 0;
        if (PIXELBLACK(st, x - 1, row + 2) || PIXELBLACK(st, x, row + 2) || PIXELBLACK(st, x + 1, row + 2) ||
            PIXELBLACK(st, x, row + 1))
            return 0;
        if (!CHECKDELETE(c, x)) return 0;
        if ((desc[0] & 1) && EXTENDRIGHT(st, x, row + 1)) goto apply;
        if (!(desc[0] & 4)) return 0;
        if (!EXTENDLEFT(st, x, row + 1)) return 0;
        goto apply;
    case 5:
        if (row == rd16(st, 0x2d0)) return 0;
        if (PIXELBLACK(st, x - 1, row - 2) || PIXELBLACK(st, x, row - 2) || PIXELBLACK(st, x + 1, row - 2) ||
            PIXELBLACK(st, x, row - 1))
            return 0;
        if (!CHECKDELETE(c, x)) return 0;
        if ((desc[0] & 0x40) && EXTENDRIGHT(st, x, row - 1)) goto apply;
        if (!(desc[0] & 0x10)) return 0;
        if (!EXTENDLEFT(st, x, row - 1)) return 0;
        goto apply;
    default:
        return 0;
    }
apply:
    if (x == PX(c)) {
        SETFL(c, FL(c) | 0x200);
        SETPX(c, PX(c) + 1);
    } else {
        char *n = NEXT(c);
        SETPX(n, PX(n) - 1);
        SETFL(n, FL(n) | 0x200);
    }
    return 1;
}
DROP_EXPORT_STK(cff_DoFixupAction, DoFixupAction_impl);

/* 0x1801845cc: which of the two pixels comes first in the original (untransformed) space:
 * the transform at ctx + 0x588 maps each pixel centre in place */
CFF_API int32_t DetermineTieBreaker_impl(char *c1, int32_t x1, char *c2, int32_t x2, char *ctx) {
    int32_t a[2], b[2];
    a[1] = CY(c1);
    b[1] = CY(c2);
    a[0] = (int32_t)((uint32_t)shl16(x1) | 0x8000);
    b[0] = (int32_t)((uint32_t)shl16(x2) | 0x8000);
    uint64_t pa = (uint64_t)(uint32_t)a[0] | (uint64_t)(uint32_t)a[1] << 32;
    ((tfm_fn)rdp(ctx, 0x588))(pa, a, ctx);
    uint64_t pb = (uint64_t)(uint32_t)b[0] | (uint64_t)(uint32_t)b[1] << 32;
    ((tfm_fn)rdp(ctx, 0x588))(pb, b, ctx);
    int32_t r = a[1] == b[1] ? a[0] > b[0] : a[1] < b[1];
#ifdef CFF_CONTROL_DETERMINETIEBREAKER
    r = !r;
#endif
    return r;
}
DROP_EXPORT_STK(cff_DetermineTieBreaker, DetermineTieBreaker_impl);

/* 0x1800c279c: two runs meeting at a pixel corner: the rule descriptions of both pixels,
 * then their actions by priority until one applies */
CFF_API void FixPixelProblem_impl(char *st, char *c1, int32_t x1, char *c2, int32_t x2, char *ctx) {
#ifdef CFF_CONTROL_FIXPIXELPROBLEM
    return;
#endif
    uint8_t d[0x20];
    memset(d, 0, sizeof d);
    GETRULE(st, d, c1, x1, c2, x2);
    GETRULE(st, d + 0x10, c2, x2, c1, x1);
    uint8_t *p1 = d + 1, *p2 = d + 0x11;
    for (;;) {
        if (p1[1] == p2[1]) {
            if (p1[1] == 0) return;
            uint8_t *q = TIEBREAK(c1, x1, c2, x2, ctx) ? p1 : p2;
            for (q++; *q; q += 3) (*q)++;
        }
        if (p1[1] > p2[1]) {
            if (DOFIXUP(st, d, p1, c1, x1)) return;
            p1 += 3;
        } else {
            if (DOFIXUP(st, d + 0x10, p2, c2, x2)) return;
            p2 += 3;
        }
    }
}
DROP_EXPORT_STK(cff_FixPixelProblem, FixPixelProblem_impl);
