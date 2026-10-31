/* CFF rasterizer, group "hint" (notes/cff.md, group 6): Adobe's stem hinting.
 *
 * Stem locking against the blue zones (BlueLock, BlueBoost), the per-character stem list
 * ("global coloring": GCNew, GCEnter, GCFind, GCCalcLocs, GCFix1Loc, ...), the lock arrays the
 * transform walks (InsertLock through SetXLock/SetYLock), the hint operators the charstring
 * interpreter calls (RXLock, RBLock, RMLock, RVLock), and the global counters (BuildGlbCounters,
 * GlobalColoring, FixBands, GCFixOnePath, ...). Ported from the assembly of DWrite.dll
 * 10.0.26100.9711; every function keeps its original x64 signature, and calls inside the group
 * go through the original entry points (CFF_ORIG), so each port swaps alone.
 *
 * The ATM font state `s` (CFFFontInst + 0x9a0, the first or last argument of everything here),
 * fields used by this group:
 *   +0x000 u16  overscale of x (UseStdWidth rounding)    +0x002 u16  x rounding multiple
 *   +0x1a8 int  BlueShift-like scale  +0x1ac int  blue fuzz  +0x1b0 int  minimum overshoot
 *   +0x1b4 int  blue zone count       +0x1b8 zones, 0x14 bytes each: top, bottom, top/bottom
 *               device position (+8 / +0xc), +0x10
 *   +0x2dc u32  bit i set: zone i is a bottom zone
 *   +0x2ec/+0x2f0 int  x / y device scale (16.16)
 *   +0x330 u16  y rounding multiple (vertical overscale)  +0x332..+0x338 u16  Adjust parameters
 *   +0x33c int  rounding mode (2..7)
 *   +0x360..+0x36c int  stem limits for GCAlignIsolatedStem_Postpass
 *   +0x3d8 int, +0x3e4 u16  BlueBoost: overshoot, last boost
 *   +0x3e8 int  error flag (1: give up hinting)  +0x3ec / +0x3f0 int  stem / counter count
 *   +0x410/+0x418 int *  StdVW/StdHW tables, +0x420/+0x424 their counts
 *   +0x428 ptr  cleared by InsertLock on first use  +0x430/+0x434/+0x44c int  flags, ppem
 *   +0x444/+0x448 int  the current hint's edge and width (-0x150000/-0x140000: ghost hints)
 *   +0x450 ptr  ->+0x20: the allocator object (vtable +0 alloc, +0x10 free, +0x28 realloc)
 *   +0x460 int  hinting pass (0 collect, 1 lock, 2 direct, 3 lock + align)
 *   +0x468/+0x470 lock arrays (x, y): u32 count at +0, entries of 0x20 bytes from +0x38
 *   +0x478/+0x480/+0x488  stem records: end, next free, grow buffer {ptr, u32 size}
 *   +0x490/+0x498/+0x4a0  counter records: same
 *   +0x4a8 stem list, +0x4b0 counter list
 *   +0x580/+0x588/+0x590/+0x598  point procs: void f(Fixed x | Fixed y << 32, Fixed out[2], s)
 *
 * The stem record (0x50 bytes, GCNew):
 *   +0x00 next  +0x08 int width (+0x0a its integer part as s16)  +0x0c/+0x10 edges (character
 *   space)  +0x14/+0x18 edges (device)  +0x1c/+0x20 counter band (+0x1e/+0x22 as s16)
 *   +0x24/+0x28 locked device edges  +0x2c device width  +0x30 half width
 *   +0x38 counters list  +0x40 path link  +0x48 u32 flags: 1 horizontal stem (y), 2 fixed,
 *   4 seen in this character, 8 std width applied, 0x10 isolated, 0x20 / 0x40 ghost top / bottom
 *   +0x4c s16 band depth  +0x4e s16 index after GCSortStems
 * The counter record (0x30 bytes, NewGlbCounter):
 *   +0x00 next  +0x08 int gap  +0x10 / +0x18 the two stems  +0x20/+0x24/+0x28 clump band
 *   (+0x2a as s16)  +0x2c u8 group  +0x2d u8 clumped
 *
 * Register hazards: GCFixLocs keeps RCX across GCFix1Loc, GCDoLock keeps RDX and R8 across
 * GCFind, GCAlignIsolatedStem_Postpass keeps R10 and R11 across GCCalcLocs, and the interpreter
 * keeps RCX across GCFind and GCCalcLocs: all exports are CFF_EXPORT thunks.
 *
 * Negative controls: -DCFF_CONTROL_HINT_<NAME> (cff_check.py --control HINT_<NAME>) breaks the
 * port of <NAME>: an early return, or a one-pixel shift of its input (GCFindLocs, Adjust,
 * PreXLock, PreYLock), or no sorting (GCSortStems), or a wrong stem count (GCNew). */
#include "cff_port.h"

/* CFF_EXPORT's thunk moves the stack pointer before calling the port, so a port with more
 * than four arguments would read its stack arguments from the thunk's frame. HINT_THUNK is
 * the same register-preserving thunk (RCX, RDX, R8-R11, XMM1-XMM5) that also copies six stack
 * argument slots (arguments 5 to 10; Adjust has ten) into its own outgoing area. */
#ifdef IDWR_STANDALONE
#define HEXPORT(n)
#define IMPL(n) cff_##n
#else
#define HINT_THUNK(name, impl)                                                         \
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
            "subq $0xb8, %rsp\n.seh_stackalloc 0xb8\n"                                 \
            "movaps %xmm1, 0x50(%rsp)\n.seh_savexmm %xmm1, 0x50\n"                     \
            "movaps %xmm2, 0x60(%rsp)\n.seh_savexmm %xmm2, 0x60\n"                     \
            "movaps %xmm3, 0x70(%rsp)\n.seh_savexmm %xmm3, 0x70\n"                     \
            "movaps %xmm4, 0x80(%rsp)\n.seh_savexmm %xmm4, 0x80\n"                     \
            "movaps %xmm5, 0x90(%rsp)\n.seh_savexmm %xmm5, 0x90\n"                     \
            ".seh_endprologue\n"                                                       \
            "movq 0x110(%rsp), %rax\nmovq %rax, 0x20(%rsp)\n"                          \
            "movq 0x118(%rsp), %rax\nmovq %rax, 0x28(%rsp)\n"                          \
            "movq 0x120(%rsp), %rax\nmovq %rax, 0x30(%rsp)\n"                          \
            "movq 0x128(%rsp), %rax\nmovq %rax, 0x38(%rsp)\n"                          \
            "movq 0x130(%rsp), %rax\nmovq %rax, 0x40(%rsp)\n"                          \
            "movq 0x138(%rsp), %rax\nmovq %rax, 0x48(%rsp)\n"                          \
            "callq " #impl "\n"                                                        \
            "movaps 0x50(%rsp), %xmm1\n"                                               \
            "movaps 0x60(%rsp), %xmm2\n"                                               \
            "movaps 0x70(%rsp), %xmm3\n"                                               \
            "movaps 0x80(%rsp), %xmm4\n"                                               \
            "movaps 0x90(%rsp), %xmm5\n"                                               \
            "addq $0xb8, %rsp\n"                                                       \
            "popq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n"         \
            "retq\n"                                                                   \
            ".seh_endproc\n"                                                           \
            ".section .drectve,\"yn\"\n"                                               \
            ".ascii \" -export:" #name "\"\n"                                          \
            ".text\n")
#define HEXPORT(n) HINT_THUNK(cff_##n, cff_##n##_impl);
#define IMPL(n) cff_##n##_impl
#endif

typedef int32_t __attribute__((may_alias)) a_i32;
typedef uint32_t __attribute__((may_alias)) a_u32;
typedef int16_t __attribute__((may_alias)) a_i16;
typedef uint16_t __attribute__((may_alias)) a_u16;
typedef uint8_t __attribute__((may_alias)) a_u8;
typedef uint8_t *__attribute__((may_alias)) a_ptr;

#define B(p) ((uint8_t *)(p))
#define I32(p, o) (*(a_i32 *)(B(p) + (o)))
#define U32(p, o) (*(a_u32 *)(B(p) + (o)))
#define I16(p, o) (*(a_i16 *)(B(p) + (o)))
#define U16(p, o) (*(a_u16 *)(B(p) + (o)))
#define U8(p, o) (*(a_u8 *)(B(p) + (o)))
#define PTR(p, o) (*(a_ptr *)(B(p) + (o)))

/* wrapping 32-bit arithmetic, as the x64 code computes it */
#define ADD(a, b) ((int32_t)((uint32_t)(a) + (uint32_t)(b)))
#define SUB(a, b) ((int32_t)((uint32_t)(a) - (uint32_t)(b)))
#define MUL(a, b) ((int32_t)((uint32_t)(a) * (uint32_t)(b)))
#define NEG(a) ((int32_t)(0u - (uint32_t)(a)))
#define SHL(a, n) ((int32_t)((uint32_t)(a) << (n)))

/* neg eax; cmovs eax, x: |x|, with INT_MIN unchanged */
static inline int32_t iabs32(int32_t x) { return x < 0 ? NEG(x) : x; }

/* stem record fields */
#define S_NEXT 0x00
#define S_W 0x08
#define S_LO 0x0c
#define S_HI 0x10
#define S_DLO 0x14
#define S_DHI 0x18
#define S_BLO 0x1c
#define S_BHI 0x20
#define S_LLO 0x24
#define S_LHI 0x28
#define S_DW 0x2c
#define S_HW 0x30
#define S_CNT 0x38
#define S_PATH 0x40
#define S_FL 0x48
#define S_LEVEL 0x4c
#define S_IDX 0x4e

#define ERR(s) I32(s, 0x3e8)
#define STEMS(s) PTR(s, 0x4a8)
#define COUNTERS(s) PTR(s, 0x4b0)

/* the point procs of the state: (x, y) packed in one register, the result in out[2] */
typedef void (*pt_fn)(uint64_t pt, int32_t *out, uint8_t *s);
static inline uint64_t pack(int32_t x, int32_t y) { return (uint32_t)x | ((uint64_t)(uint32_t)y << 32); }
/* the original passes the point by value from a stack slot and the slot itself as `out` */
static inline int32_t pt_call(uint8_t *s, int off, int32_t *b) {
    ((pt_fn)PTR(s, off))(pack(b[0], b[1]), b, s);
    return 0;
}
/* transform (v, 0) or (0, v) by proc `off` and return the same component */
static inline int32_t tfm1(uint8_t *s, int off, int32_t v, int y) {
    int32_t b[2];
    b[0] = y ? 0 : v;
    b[1] = y ? v : 0;
    pt_call(s, off, b);
    return b[y ? 1 : 0];
}

/* the allocator object of the state */
#define ALLOCATOR(s) PTR(PTR(s, 0x450), 0x20)
#define VFN(obj, off) (*(void **)(*(uint8_t **)(obj) + (off)))
typedef void *(*alloc_fn)(void *obj, int64_t size);
typedef void (*free_fn)(void *obj, void *p);
typedef void *(*realloc_fn)(void *obj, void *p, uint64_t size);

/* ------------------------------------------------------------------------------------------ */
/* the group's entry points, called through the original addresses */

typedef void (*BuildGlbCounters_t)(uint8_t *s);
typedef void (*GCProcess_t)(uint8_t *s, void *buf, int32_t lvl, void *p4);
typedef int32_t (*GlobalColoringGC_t)(uint8_t *stems, void *buf, int32_t n1, int32_t n2, uint8_t *s);
typedef void (*GCSortStems_t)(uint8_t *stems, uint8_t **arr, int32_t n);
typedef void (*SelectionSort_t)(uint8_t **arr, int32_t lo, int32_t hi);
typedef int32_t (*GCEnter_t)(int32_t lo, int32_t hi, int32_t y, int32_t edge, uint8_t *s);
typedef uint8_t *(*GCNew_t)(uint8_t **prev, uint8_t **cur, uint8_t *s);
typedef void (*AlignIsolatedStems_t)(uint8_t **arr, int32_t n, uint8_t *s);
typedef void (*GCAlignIsolatedStem_t)(int32_t lo, int32_t hi, int32_t y, uint8_t *s);
typedef void (*GCFix1Loc_t)(uint8_t *stem, uint8_t *s);
typedef int32_t (*GCDoLock_t)(int32_t lo, int32_t hi, int32_t y, uint8_t *s, void *p5);
typedef uint8_t *(*GCFind_t)(int32_t lo, int32_t hi, int32_t y, uint8_t *s);
typedef void (*GCFindLocs_t)(uint8_t *stem, int32_t delta, uint8_t *s);
typedef void (*GCCalcLocs_t)(uint8_t *stem, uint8_t *s);
typedef void (*BlueBoost_t)(uint8_t *s);
typedef void (*RaiseTops_t)(uint8_t *s);
typedef int32_t (*Lock_t)(int32_t a, int32_t b, int32_t flags, int32_t c, int32_t d, uint8_t *s, void *p7);
typedef void (*RXLock_t)(int32_t *h, int32_t off, uint8_t *s, void *p4);
typedef void (*MatchStem_t)(int32_t *h, int32_t y, uint8_t *s);
typedef void (*BlueLock_t)(int32_t lo, int32_t hi, int32_t edge, uint8_t *stem, uint8_t *s, void *p6);
typedef int32_t (*PreLock_t)(int32_t lo, int32_t hi, int32_t *loc, int32_t *mid, uint8_t *s, void *p6);
typedef int32_t (*InsertLock_t)(uint8_t *arr, int32_t *e, uint8_t *s);
typedef int32_t (*CalcHW2_t)(int32_t hw, int32_t dw, int32_t w, int32_t y, uint8_t *s);
typedef int32_t (*Adjust_t)(int32_t edge, int32_t ghostb, int32_t w, int32_t loc, uint32_t n, uint32_t p6, uint32_t p7,
                            int32_t p8, int32_t p9, uint8_t *s);
typedef int32_t (*UseStdWidth_t)(int32_t w, int32_t *out, int32_t *tbl, int32_t n, int64_t unused, uint32_t ov, uint8_t *s);
typedef void (*BoostBotLocs_t)(uint8_t *s);
typedef int32_t (*GCFixOnePath_t)(uint8_t *stem, uint8_t **arr, int32_t pix, void *p4, int32_t max);
typedef void (*ClumpCounters_t)(uint8_t **arr, int32_t n, int32_t tol);
typedef void (*SortGroupsByFrac_t)(uint8_t **arr, int32_t n);
typedef int32_t (*CounterGt_t)(uint8_t *a, uint8_t *b);
typedef uint8_t *(*NewGlbCounter_t)(uint8_t *s);
typedef int32_t (*FixBands_t)(uint8_t **stems, int32_t n, uint8_t **ctrs, int32_t pix, uint8_t *s);
typedef void (*GCStartLock_t)(uint8_t *s);
typedef void (*TriLock_t)(int32_t *h, int32_t off, void *pre, void *set, int32_t ov, uint8_t *s, void *p7);
typedef void (*EnterGlbCounter_t)(uint8_t *a, uint8_t *b, uint8_t *s);
typedef void (*GCCalcAnchorLocs_t)(uint8_t *stem, int32_t loc, uint8_t *s);
typedef uint8_t *(*ExtendToAnchor_t)(uint8_t *stem);
typedef int32_t (*GlobalColoring_t)(uint8_t *stems, uint8_t *ctrs, void *buf, int32_t pix, int32_t nctr, int32_t nstem,
                                    uint8_t *s);
typedef int32_t (*SimpleCounter_t)(uint8_t **arr, uint8_t *ctr);

#define O_BuildGlbCounters CFF_ORIG(BuildGlbCounters_t, 0xae5f0)
#define O_GCSortStems CFF_ORIG(GCSortStems_t, 0xae85c)
#define O_SelectionSort CFF_ORIG(SelectionSort_t, 0xae918)
#define O_GCEnter CFF_ORIG(GCEnter_t, 0xae9c0)
#define O_GCNew CFF_ORIG(GCNew_t, 0xaec78)
#define O_AlignIsolatedStems CFF_ORIG(AlignIsolatedStems_t, 0xaeea4)
#define O_GCAlignIsolatedStem CFF_ORIG(GCAlignIsolatedStem_t, 0xaefa0)
#define O_GCFix1Loc CFF_ORIG(GCFix1Loc_t, 0xaf3d4)
#define O_GCAlignIsolatedStem_Postpass CFF_ORIG(GCAlignIsolatedStem_t, 0xaf5e4)
#define O_GCDoLock CFF_ORIG(GCDoLock_t, 0xaf904)
#define O_GCFind CFF_ORIG(GCFind_t, 0xafaa0)
#define O_GCFindLocs CFF_ORIG(GCFindLocs_t, 0xafafc)
#define O_GCCalcLocs CFF_ORIG(GCCalcLocs_t, 0xafba4)
#define O_RaiseTops CFF_ORIG(RaiseTops_t, 0xb00d8)
#define O_SetXLock CFF_ORIG(Lock_t, 0xb0220)
#define O_MatchStem CFF_ORIG(MatchStem_t, 0xb06d0)
#define O_BlueLock CFF_ORIG(BlueLock_t, 0xb07dc)
#define O_PreXLock CFF_ORIG(PreLock_t, 0xb1090)
#define O_SetYLock CFF_ORIG(Lock_t, 0xb1200)
#define O_InsertLock CFF_ORIG(InsertLock_t, 0xb125c)
#define O_CalcHW2 CFF_ORIG(CalcHW2_t, 0xb14c8)
#define O_Adjust CFF_ORIG(Adjust_t, 0xb15f0)
#define O_UseStdWidth CFF_ORIG(UseStdWidth_t, 0xb1948)
#define O_BoostBotLocs CFF_ORIG(BoostBotLocs_t, 0xe2870)
#define O_GCFixOnePath CFF_ORIG(GCFixOnePath_t, 0xe9870)
#define O_ClumpCounters CFF_ORIG(ClumpCounters_t, 0xe9c34)
#define O_SortGroupsByFrac CFF_ORIG(SortGroupsByFrac_t, 0xe9e30)
#define O_CounterGt CFF_ORIG(CounterGt_t, 0xe9f30)
#define O_NewGlbCounter CFF_ORIG(NewGlbCounter_t, 0xf1230)
#define O_FixBands CFF_ORIG(FixBands_t, 0xf2e90)
#define O_TriLock CFF_ORIG(TriLock_t, 0xf326c)
#define O_EnterGlbCounter CFF_ORIG(EnterGlbCounter_t, 0xf3590)
#define O_GCCalcAnchorLocs CFF_ORIG(GCCalcAnchorLocs_t, 0x10c064)
#define O_GCFixLocs CFF_ORIG(void (*)(uint8_t *, uint8_t *), 0x10c3fc)
#define O_PreYLock CFF_ORIG(PreLock_t, 0x184870)
#define O_ExtendToAnchor CFF_ORIG(ExtendToAnchor_t, 0x184b24)
#define O_GlobalColoring CFF_ORIG(GlobalColoring_t, 0x184b6c)
#define O_SimpleCounter CFF_ORIG(SimpleCounter_t, 0x184d1c)

/* the math (setup group) */
#define O_fixmul CFF_ORIG(int32_t (*)(int32_t, int32_t), 0xb3340)
#define O_fixdiv CFF_ORIG(int32_t (*)(int32_t, int32_t), 0xb316c)
#define O_fracmul CFF_ORIG(int32_t (*)(int32_t, int32_t), 0xb34d8)
#define O_fixratio CFF_ORIG(int32_t (*)(int32_t, int32_t), 0xb3544)
/* FFloorFModN(x, n): x floored to a multiple of n pixels; FFloorFModNR / FRoundFModNR(x, n, r)
 * floor / round to a multiple of n with a phase of r pixels (the callees zero- or sign-extend
 * the 16-bit arguments themselves) */
#define O_FFloorFModN CFF_ORIG(int32_t (*)(int32_t, uint16_t), 0xb1c1c)
#define O_FFloorFModNR CFF_ORIG(int32_t (*)(int32_t, uint16_t, int16_t), 0xb1bdc)
#define O_FRoundFModNR CFF_ORIG(int32_t (*)(int32_t, uint16_t, int16_t), 0xafc98)

/* ------------------------------------------------------------------------------------------ */
/* the stem list */

/* GCNew 0x1800aec78: the next free stem record; grows (and relocates) the record buffer */
CFF_API uint8_t *IMPL(GCNew)(uint8_t **prev, uint8_t **cur, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCNEW
    I32(s, 0x3ec) -= 1;
#endif
    if ((uintptr_t)PTR(s, 0x480) >= (uintptr_t)PTR(s, 0x478)) {
        uint8_t *gb = PTR(s, 0x488);
        uint8_t *old = PTR(gb, 0);
        void *obj = ALLOCATOR(s);
        uint8_t *nw = ((realloc_fn)VFN(obj, 0x28))(obj, old, (uint32_t)(U32(gb, 8) + 0x50));
        if (!nw) {
            obj = ALLOCATOR(s);
            ((free_fn)VFN(obj, 0x10))(obj, PTR(gb, 0));
            U32(gb, 8) = 0;
            PTR(gb, 0) = 0;
            STEMS(s) = 0;
            PTR(s, 0x478) = 0;
            PTR(s, 0x480) = 0;
            I32(s, 0x3f0) = 0;
            I32(s, 0x3ec) = 0;
            return 0;
        }
        U32(gb, 8) += 0x50;
        PTR(gb, 0) = nw;
        if (nw != old) {
            /* p - old, rounded down to 16 bytes (the original divides by 0x50 with a
             * multiply-by-inverse that keeps the multiple of 16) */
#define RELOC(p) ((uint8_t *)((uint64_t)((int64_t)((uint64_t)(p) - (uint64_t)old) >> 4) * 0xcccccccccccccccdull * 5 * 16 + (uint64_t)nw))
            if (*prev)
                *prev = RELOC(*prev);
            if (*cur)
                *cur = RELOC(*cur);
            PTR(s, 0x480) = RELOC(PTR(s, 0x480));
            if (STEMS(s)) {
                uint8_t *p = RELOC(STEMS(s));
                STEMS(s) = p;
                while (p && PTR(p, S_NEXT)) {
                    uint8_t *q = RELOC(PTR(p, S_NEXT));
                    PTR(p, S_NEXT) = q;
                    p = q;
                }
                for (uint8_t *c = COUNTERS(s); c; c = PTR(c, 0)) {
                    PTR(c, 0x18) = RELOC(PTR(c, 0x18));
                    PTR(c, 0x10) = RELOC(PTR(c, 0x10));
                }
            }
#undef RELOC
        }
        PTR(s, 0x478) = nw + (uint64_t)(U32(gb, 8) / 0x50) * 0x50;
    }
    uint8_t *r = PTR(s, 0x480);
    I32(s, 0x3ec) += 1;
    PTR(s, 0x480) = r + 0x50;
    return r;
}
HEXPORT(GCNew)

/* GCEnter 0x1800ae9c0: enters the stem [lo, hi] (y: horizontal stem) unless one within two
 * units is already there; edge is the hint's width (-0x150000 / -0x140000: ghost hints) */
CFF_API int32_t IMPL(GCEnter)(int32_t lo, int32_t hi, int32_t y, int32_t edge, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCENTER
    return 1;
#endif
    int32_t b[2], dlo, dhi, dw;
    if (I32(s, 0x460) == 0 && (((uint32_t)edge + 0x150000u) & 0xfffeffffu) == 0)
        return 0;
    if (y == 0) {
        b[0] = lo, b[1] = 0;
        pt_call(s, 0x590, b);
        dlo = b[0];
        b[0] = hi, b[1] = 0;
        pt_call(s, 0x590, b);
        dhi = b[0];
    } else {
        b[0] = 0, b[1] = lo;
        pt_call(s, 0x590, b);
        dlo = b[1];
        b[0] = 0, b[1] = hi;
        pt_call(s, 0x590, b);
        dhi = b[1];
    }
    dw = SUB(dhi, dlo);
    uint8_t *prev = 0, *cur = STEMS(s);
    for (; cur; prev = cur, cur = PTR(cur, S_NEXT)) {
        if (iabs32(SUB(I32(cur, S_LO), lo)) < 0x20000 && iabs32(SUB(I32(cur, S_HI), hi)) < 0x20000 &&
            (int32_t)(U32(cur, S_FL) & 1) == y) {
            U32(cur, S_FL) |= 4;
            return 1;
        }
    }
    uint8_t *r = O_GCNew(&prev, &cur, s);
    if (!r) {
        ERR(s) = 1;
        return 0;
    }
    uint32_t fl = U32(r, S_FL);
    int32_t d14, d18;
    if (edge == -0x150000) {
        fl = (fl & ~0x20u) | 0x40;
        d14 = dlo, d18 = dlo;
    } else {
        if (edge == -0x140000) {
            fl |= 0x20;
            d14 = dhi;
        } else {
            d14 = dlo;
            fl &= ~0x20u;
        }
        fl &= ~0x40u;
        d18 = dhi;
    }
    I32(r, S_DLO) = d14;
    I32(r, S_DHI) = d18;
    I32(r, S_LO) = lo;
    I32(r, S_HI) = hi;
    U32(r, S_BLO) = 0x3e800000;
    fl = ((fl ^ (uint32_t)y) & 1) ^ fl;
    U32(r, S_BHI) = 0xc1800000;
    I32(r, S_DW) = dw;
    I32(r, S_W) = dw;
    fl &= ~8u & ~2u;
    U32(r, S_FL) = fl;
    PTR(r, S_PATH) = 0;
    PTR(r, S_CNT) = 0;
    PTR(r, S_NEXT) = 0;
    if ((fl & 0x60) == 0) {
        int32_t n = y ? I32(s, 0x424) : I32(s, 0x420);
        if (n)
            O_UseStdWidth(SUB(hi, lo), (int32_t *)(B(r) + S_W), (int32_t *)PTR(s, y ? 0x418 : 0x410), n, 0, U16(s, 0), s);
    }
    U32(r, S_FL) |= 4;
    if (prev) {
        PTR(prev, S_NEXT) = r;
        PTR(r, S_NEXT) = cur;
    } else {
        PTR(r, S_NEXT) = STEMS(s);
        STEMS(s) = r;
    }
    return 1;
}
HEXPORT(GCEnter)

/* GCFind 0x1800afaa0: the stem within two units of [lo, hi] in direction y, or NULL */
CFF_API uint8_t *IMPL(GCFind)(int32_t lo, int32_t hi, int32_t y, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCFIND
    return 0;
#endif
    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT))
        if (iabs32(SUB(I32(p, S_LO), lo)) < 0x20000 && iabs32(SUB(I32(p, S_HI), hi)) < 0x20000 &&
            (int32_t)(U32(p, S_FL) & 1) == y)
            return p;
    return 0;
}
HEXPORT(GCFind)

/* GCStartLock 0x1800f3154: clears the "seen" flag of every stem */
CFF_API void IMPL(GCStartLock)(uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCSTARTLOCK
    return;
#endif
    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT))
        U32(p, S_FL) &= ~4u;
}
HEXPORT(GCStartLock)

/* GCFixLocs 0x18010c3fc */
CFF_API void IMPL(GCFixLocs)(uint8_t *p, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCFIXLOCS
    return;
#endif
    for (; p; p = PTR(p, S_NEXT))
        O_GCFix1Loc(p, s);
}
HEXPORT(GCFixLocs)

/* SelectionSort 0x1800ae918: sorts arr[lo, hi) by the stems' low edge */
CFF_API void IMPL(SelectionSort)(uint8_t **arr, int32_t lo, int32_t hi) {
    if (lo >= hi - 1)
        return;
    uint32_t count = (uint32_t)(hi - 1 - lo);
    int64_t i = lo;
    int32_t i1 = lo + 1;
    do {
        uint8_t *first = arr[i];
        int64_t best = i;
        int32_t key = I32(first, S_LO);
        if (i1 < hi) {
            int64_t j = i + 1;
            for (uint32_t k = (uint32_t)(hi - i1); k; k--, j++) {
                int32_t v = I32(arr[j], S_LO);
#ifdef CFF_CONTROL_HINT_SELECTIONSORT
                if (key < v) /* descending */
#else
                if (key > v)
#endif
                    best = j, key = v;
            }
            if (best != i) {
                arr[i] = arr[best];
                arr[best] = first;
            }
        }
        i1++;
        i++;
    } while (--count);
}
HEXPORT(SelectionSort)

/* GCSortStems 0x1800ae85c: the vertical stems, then the horizontal ones, each sorted */
CFF_API void IMPL(GCSortStems)(uint8_t *stems, uint8_t **arr, int32_t n) {
    int32_t nx = 0, nall;
    int64_t k = 0;
    for (uint8_t *p = stems; p && k < n; p = PTR(p, S_NEXT))
        if (!(U8(p, S_FL) & 1))
            arr[k++] = p, nx++;
    nall = nx;
    k = nx;
    for (uint8_t *p = stems; p && k < n; p = PTR(p, S_NEXT))
        if (U8(p, S_FL) & 1)
            arr[k++] = p, nall++;
    O_SelectionSort(arr, 0, nx);
    O_SelectionSort(arr, nx, nall);
    for (int32_t i = 0; i < nall; i++)
#ifdef CFF_CONTROL_HINT_GCSORTSTEMS
        I16(arr[i], S_IDX) = (int16_t)(nall - 1 - i);
#else
        I16(arr[i], S_IDX) = (int16_t)i;
#endif
}
HEXPORT(GCSortStems)

/* grows the pointer buffer buf {ptr, u32 size} to `size` bytes; 0 when out of memory */
static int grow_ptrs(uint8_t *buf, uint32_t size, uint8_t *s) {
    if (U32(buf, 8) < size) {
        if (PTR(buf, 0)) {
            void *obj = ALLOCATOR(s);
            ((free_fn)VFN(obj, 0x10))(obj, PTR(buf, 0));
        }
        void *obj = ALLOCATOR(s);
        uint8_t *p = ((alloc_fn)VFN(obj, 0))(obj, (int64_t)(int32_t)size);
        PTR(buf, 0) = p;
        if (!p) {
            U32(buf, 8) = 0;
            return 0;
        }
        U32(buf, 8) = size;
    }
    return 1;
}

/* GlobalColoringGC 0x1800ae794 */
CFF_API int32_t IMPL(GlobalColoringGC)(uint8_t *stems, void *buf, int32_t n1, int32_t n2, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GLOBALCOLORINGGC
    return 1;
#endif
    if (n2 == 0)
        return 1;
    if (!grow_ptrs(B(buf), (uint32_t)(n1 + n2) << 3, s))
        return 0;
    uint8_t **arr = (uint8_t **)PTR(buf, 0);
    O_GCSortStems(stems, arr, n2);
    O_AlignIsolatedStems(arr, n2, s);
    return 1;
}
HEXPORT(GlobalColoringGC)

/* BuildGlbCounters 0x1800ae5f0: a counter between every two disjoint stems of the same
 * direction */
CFF_API void IMPL(BuildGlbCounters)(uint8_t *s) {
#ifdef CFF_CONTROL_HINT_BUILDGLBCOUNTERS
    return;
#endif
    for (uint8_t *a = STEMS(s); a; a = PTR(a, S_NEXT)) {
        uint32_t dir = U32(a, S_FL) & 1;
        int32_t lo = I32(a, S_LO), hi = I32(a, S_HI);
        for (uint8_t *b = PTR(a, S_NEXT); b; b = PTR(b, S_NEXT)) {
            if ((U32(b, S_FL) & 1) != dir)
                continue;
            if (I32(b, S_LO) > hi)
                O_EnterGlbCounter(b, a, s);
            else if (lo > I32(b, S_HI))
                O_EnterGlbCounter(a, b, s);
            if (ERR(s) == 1)
                return;
        }
    }
}
HEXPORT(BuildGlbCounters)

/* GCProcess 0x1800ae68c: locks every stem of the character, then the global coloring */
CFF_API void IMPL(GCProcess)(uint8_t *s, void *buf, int32_t pix, void *p4) {
#ifdef CFF_CONTROL_HINT_GCPROCESS
    return;
#endif
    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT)) {
        if (!(U8(p, S_FL) & 1))
            O_GCCalcLocs(p, s);
        else
            O_BlueLock(I32(p, S_LO), I32(p, S_HI), 0, p, s, p4);
    }
    O_GCFixLocs(STEMS(s), s);
    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT))
        I32(p, S_W) = iabs32(SUB(I32(p, S_LHI), I32(p, S_LLO)));
    O_BuildGlbCounters(s);
    if (ERR(s) == 1)
        return;
    if (!O_GlobalColoring(STEMS(s), COUNTERS(s), buf, pix, I32(s, 0x3f0), I32(s, 0x3ec), s))
        ERR(s) = 1;
}
HEXPORT(GCProcess)

/* AlignIsolatedStems 0x1800aeea4 */
CFF_API void IMPL(AlignIsolatedStems)(uint8_t **arr, int32_t n, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_ALIGNISOLATEDSTEMS
    return;
#endif
    if (n <= 0)
        return;
    for (uint32_t k = (uint32_t)n; k; k--, arr++) {
        uint8_t *p = *arr;
        uint32_t fl = U32(p, S_FL);
        if (fl & 0x10) {
            O_GCAlignIsolatedStem(I32(p, S_LO), I32(p, S_HI), fl & 1, s);
            continue;
        }
        if (fl & 0x60)
            continue;
        int32_t w = tfm1(s, 0x590, ADD(I32(p, S_HW), I32(p, S_HW)), fl & 1);
        p = *arr;
        int32_t d = ADD(SUB((int32_t)((uint32_t)w & 0xffff0000u), I32(p, S_W)), 0x10000) >> 1;
        U32(p, S_LLO) = (uint32_t)SUB(I32(p, S_LLO), d) & 0xffff0000u;
        p = *arr;
        U32(p, S_LHI) = (uint32_t)ADD(I32(p, S_LHI), d) & 0xffff0000u;
    }
}
HEXPORT(AlignIsolatedStems)

/* ------------------------------------------------------------------------------------------ */
/* rounding of one stem */

/* CalcHW2 0x1800b14c8: the device half width of a stem (y: horizontal stem) */
CFF_API int32_t IMPL(CalcHW2)(int32_t hw, int32_t dw, int32_t w, int32_t y, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_CALCHW2
    return 0;
#endif
    int32_t adw = iabs32(dw), aw = iabs32(w), r = hw >= 0 ? hw : NEG(hw);
    if (I32(s, 0x44c) < 0x8000 || I32(s, 0x434) != 0)
        return r;
    int32_t rw = (int32_t)((uint32_t)ADD(aw, 0x8000) & 0xffff0000u);
    int32_t k = MUL((int16_t)(SUB(y ? I32(s, 0x2f0) : I32(s, 0x2ec), 0x10000) >> 2), 0x4d41) >> 14;
    int32_t v = SUB(0x10000, k), lim = ADD(k, 0x3000);
    if (rw > 0x10000) {
        v = SUB(rw, k);
        if (adw <= v && adw >= SUB(rw, lim))
            return r;
        if (aw <= v) {
            int32_t lo = SUB(rw, lim);
            v = aw < lo ? lo : aw;
        }
    }
    return tfm1(s, 0x598, v, y) >> 1;
}
HEXPORT(CalcHW2)

/* Adjust 0x1800b15f0: the rounded device position of a stem's centre `loc` (width w, edge
 * the hint's character-space edge, n/p6/p7 the rounding multiple and phases, p8/p9 ghost top
 * and bottom) */
CFF_API int32_t IMPL(Adjust)(int32_t edge, int32_t unused, int32_t w, int32_t loc, uint16_t n_, uint16_t p6_,
                             uint16_t p7_, int32_t p8, int32_t p9, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_ADJUST
    loc = ADD(loc, 0x10000);
#endif
    (void)unused;
    uint32_t n = n_;
    int32_t mode = I32(s, 0x33c);
    int32_t fl = O_FFloorFModN(ADD(SHL(n, 15), w), (uint16_t)n);
    int32_t n2 = SHL(n, 17), n1 = n2 >> 1, nh = n1 >> 1, hw = w >> 1;
    int32_t base = SHL((uint32_t)p6_, 16);
    int32_t r;
    if (edge == 0 || p9 != 0)
        goto floor_side;
    if (fl < n2 && edge < 0xc80000 && ((uint32_t)(mode - 1) & 0xfffffffbu) == 0)
        goto floor_side;
    if (p8 != 0)
        goto ceil_side;
    if (fl < n2 && ((uint32_t)(mode - 1) & 0xfffffffbu) == 0)
        goto ceil_side;
    if (mode <= 2 || mode >= 8 || mode == 5) {
        int32_t c = ADD(base, loc), t;
        int32_t q = (fl >> 16) / (int32_t)n;
        if (q & 1)
            t = ADD(O_FFloorFModNR(c, (uint16_t)n, (int16_t)p7_), nh);
        else
            t = O_FRoundFModNR(c, (uint16_t)n, (int16_t)p7_);
        if (edge >= 0xc80000) {
            int32_t u = ADD(hw, t);
            r = ADD(t, SUB(O_FRoundFModNR(u, (uint16_t)n, (int16_t)p7_), u));
        } else {
            r = ADD(hw, O_FRoundFModNR(SUB(t, hw), (uint16_t)n, (int16_t)p7_));
        }
    } else {
        int32_t top = ADD(ADD(hw, base), loc);   /* edi */
        int32_t bot = ADD(SUB(base, hw), loc);   /* r13d */
        int32_t d5 = SUB(bot, O_FFloorFModNR(bot, (uint16_t)n, (int16_t)p7_));
        int32_t nn = SHL(n, 16);
        int32_t d7 = SUB(nn, d5);
        int32_t c2 = O_FFloorFModN(ADD(SUB(nn, 1), top), (uint16_t)n);
        int32_t d1 = SUB(c2, SHL((int32_t)(int16_t)p7_, 16));
        if (d1 < top)
            d1 = ADD(d1, nn);
        d1 = SUB(d1, top);
        int32_t d12 = SUB(nn, d1);
        if (mode != 3 || (ADD(w, 0x8000) >> 16) < (int32_t)n) {
            int32_t a = O_FFloorFModN(ADD(SUB((int32_t)((uint32_t)ADD(w, 0x8000) & 0xffff0000u), 1), nn), (uint16_t)n);
            int32_t b = O_FFloorFModN(bot, (uint16_t)n);
            if (!(a < SUB(c2, b))) {
                r = loc;
                goto final;
            }
        }
        if (d5 < d7 && d5 < d12 && d5 < d1)
            r = ADD(SUB(hw, d5), bot);
        else if (d7 < d12 && d7 < d1)
            r = ADD(ADD(hw, bot), d7);
        else if (d12 < d1)
            r = SUB(SUB(top, hw), d12);
        else
            r = ADD(SUB(top, hw), d1);
    }
final : {
    uint32_t rw = (uint32_t)ADD(w, 0x8000) & 0xffff0000u;
    if ((int32_t)rw <= 0x10000 || (rw & 0x10000))
        return ADD((int32_t)((uint32_t)r & 0xffff0000u), 0x8000);
    return (int32_t)((uint32_t)ADD(r, 0x8000) & 0xffff0000u);
}
floor_side : {
    int32_t c = ADD(SUB(base, hw), loc);
    int32_t d = SUB(c, O_FFloorFModNR(c, (uint16_t)n, (int16_t)p7_));
    int32_t e = SUB(n1, d);
    return d >= e ? ADD(ADD(hw, c), e) : SUB(ADD(hw, c), d);
}
ceil_side : {
    int32_t nn = SHL(n, 16);
    int32_t c = ADD(ADD(base, hw), loc);
    int32_t a = SUB(O_FFloorFModN(ADD(SUB(nn, 1), c), (uint16_t)n), SHL((int32_t)(int16_t)p7_, 16));
    if (a < c)
        a = ADD(a, nn);
    a = SUB(a, c);
    int32_t e = SUB(n1, a);
    int32_t m = SUB(c, hw);
    return e >= a ? ADD(m, a) : SUB(m, e);
}
}
HEXPORT(Adjust)

/* GCFindLocs 0x1800afafc: the locked device edges of a stem around its centre + delta */
CFF_API void IMPL(GCFindLocs)(uint8_t *p, int32_t delta, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCFINDLOCS
    delta = ADD(delta, 0x10000);
#endif
    int32_t w = I32(p, S_W);
    int32_t h = (int32_t)((uint32_t)(ADD(w, 0x8000) >> 1) & 0xffff8000u);
    if (h < 0x8000)
        h = 0x8000;
    int32_t c = ADD(ADD(I32(p, S_DHI), I32(p, S_DLO)) >> 1, delta);
    int32_t lo = SUB(c, h), hi = ADD(c, h);
    if (I32(s, 0x434) == 0 && I32(s, 0x44c) >= 0x8000) {
        lo = ADD(lo, 0x8000);
        hi = SUB(hi, 0x8000);
    }
    hi = ADD((int32_t)((uint32_t)hi & 0xffff0000u), 0x10000);
    lo = (int32_t)((uint32_t)lo & 0xffff0000u);
    I32(p, S_LLO) = lo;
    I32(p, S_LHI) = hi;
    if (hi <= lo)
        I32(p, S_LHI) = ADD(lo, 0x10000);
    I32(p, S_HW) = O_CalcHW2(SUB(I32(p, S_HI), I32(p, S_LO)) >> 1, I32(p, S_DW), w, U32(p, S_FL) & 1, s);
}
HEXPORT(GCFindLocs)

/* GCCalcLocs 0x1800afba4 */
CFF_API void IMPL(GCCalcLocs)(uint8_t *p, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCCALCLOCS
    return;
#endif
    uint32_t fl = U32(p, S_FL);
    if (fl & 0x40) {
        uint32_t v = (uint32_t)ADD(I32(p, S_DLO), 0x8000) & 0xffff0000u;
        I32(p, S_HW) = 0;
        U32(p, S_LHI) = v;
        U32(p, S_LLO) = v;
        return;
    }
    if ((fl >> 5) & 1) {
        uint32_t v = (uint32_t)ADD(I32(p, S_DHI), 0x8000) & 0xffff0000u;
        I32(p, S_HW) = 0;
        U32(p, S_LHI) = v;
        U32(p, S_LLO) = v;
        return;
    }
    int32_t mid = ADD(I32(p, S_DLO) >> 1, I32(p, S_DHI) >> 1);
    uint16_t a, b, c;
    if (!(fl & 1))
        a = U16(s, 2), b = U16(s, 0x336), c = U16(s, 0x332);
    else
        a = U16(s, 0x330), b = U16(s, 0x338), c = U16(s, 0x334);
    int32_t r = O_Adjust(I32(p, S_LO), b, I32(p, S_W), mid, a, b, c, 0, 0, s);
    O_GCFindLocs(p, SUB(r, mid), s);
}
HEXPORT(GCCalcLocs)

/* GCCalcAnchorLocs 0x18010c064 */
CFF_API void IMPL(GCCalcAnchorLocs)(uint8_t *p, int32_t loc, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCCALCANCHORLOCS
    return;
#endif
    O_GCFindLocs(p, SUB(SUB(loc, I32(p, S_DHI) >> 1), I32(p, S_DLO) >> 1), s);
    U32(p, S_FL) |= 2;
}
HEXPORT(GCCalcAnchorLocs)

/* GCFix1Loc 0x1800af3d4: keeps a stem's locked edges in the order of the fixed stems around it */
CFF_API void IMPL(GCFix1Loc)(uint8_t *a, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCFIX1LOC
    return;
#endif
    if (U8(a, S_FL) & 2)
        return;
    int any = 0;
    int32_t maxlo = INT32_MIN, maxhi = INT32_MIN, minlo = INT32_MAX, minhi = INT32_MAX;
    uint8_t *other = 0;
    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT)) {
        uint32_t pf = U32(p, S_FL);
        if ((pf ^ U32(a, S_FL)) & 1)
            continue;
        if (a == p)
            continue;
        if (pf & 2) {
            int hit = 0;
            if (I32(p, S_DLO) <= I32(a, S_DLO) && I32(p, S_LLO) > maxlo)
                maxlo = I32(p, S_LLO), hit = 1;
            if (I32(p, S_DHI) <= I32(a, S_DHI) && I32(p, S_LHI) > maxhi)
                maxhi = I32(p, S_LHI), hit = 1;
            if (I32(p, S_DLO) >= I32(a, S_DLO) && I32(p, S_LLO) < minlo)
                minlo = I32(p, S_LLO), hit = 1;
            if (I32(p, S_DHI) >= I32(a, S_DHI) && I32(p, S_LHI) < minhi)
                minhi = I32(p, S_LHI);
            else if (!hit)
                continue;
            any = 1;
        }
        if ((U32(a, S_FL) & pf) & 2)
            continue;
        int32_t adlo = I32(a, S_DLO), pdlo = I32(p, S_DLO);
        uint8_t *in = 0;
        if (pdlo <= adlo && I32(a, S_DHI) <= I32(p, S_DHI))
            in = a, other = p;
        else if (!(pdlo < adlo) && I32(p, S_DHI) <= I32(a, S_DHI))
            in = p, other = a;
        if (in) {
            int32_t l = I32(in, S_LLO), ol = I32(other, S_LLO);
            if (l < ol) {
                if (!(U8(other, S_FL) & 2)) {
                    I32(other, S_LLO) = l;
                    I32(other, S_LHI) = ADD(I32(other, S_LHI), SUB(l, ol));
                } else {
                    I32(in, S_LLO) = ol;
                    I32(in, S_LHI) = ADD(I32(in, S_LHI), SUB(ol, l));
                }
            } else {
                int32_t h = I32(in, S_LHI), oh = I32(other, S_LHI);
                if (h > oh) {
                    if (!(U8(other, S_FL) & 2)) {
                        I32(other, S_LHI) = h;
                        I32(other, S_LLO) = ADD(SUB(ol, oh), h);
                    } else {
                        I32(in, S_LHI) = oh;
                        I32(in, S_LLO) = ADD(SUB(l, h), oh);
                    }
                }
            }
        } else {
            if (I32(a, S_DHI) <= pdlo) {
                int32_t h = I32(a, S_LHI), v = I32(p, S_LLO);
                if (h > v) {
                    I32(a, S_LHI) = v;
                    I32(a, S_LLO) = ADD(I32(a, S_LLO), SUB(v, h));
                }
            }
            if (adlo <= I32(p, S_DHI)) {
                int32_t l = I32(a, S_LLO), v = I32(p, S_LHI);
                if (l > v) {
                    I32(a, S_LLO) = v;
                    I32(a, S_LHI) = ADD(I32(a, S_LHI), SUB(v, l));
                }
            }
        }
    }
    if (any) {
        int32_t v = I32(a, S_LLO);
        if (maxlo > v)
            I32(a, S_LLO) = maxlo, v = maxlo;
        if (minlo < v)
            I32(a, S_LLO) = minlo;
        v = I32(a, S_LHI);
        if (maxhi > v)
            I32(a, S_LHI) = maxhi, v = maxhi;
        if (minhi < v)
            I32(a, S_LHI) = minhi;
    }
}
HEXPORT(GCFix1Loc)

/* the edge-order tests of GCAlignIsolatedStem: when the order of the character-space edges
 * ea, ep disagrees with the order of the device edges *la, lp, *la moves to lp (and *other
 * moves along unless `keep`) */
static inline void order_fix(int32_t ea, int32_t ep, a_i32 *la, int32_t lp, a_i32 *other, int keep) {
    int32_t l = *la;
    int fix;
    if (ea < ep)
        fix = l > lp;
    else if (l < lp)
        fix = 1;
    else if (ea > ep)
        fix = 0;
    else
        fix = l > lp;
    if (fix) {
        if (!keep)
            *other = ADD(*other, SUB(lp, l));
        *la = lp;
    }
}
static inline void order_clamp(int32_t ea, int32_t ep, a_i32 *la, int32_t lp) {
    int32_t l = *la;
    if (ea < ep) {
        if (l > lp)
            *la = lp;
    } else if (l < lp)
        *la = lp;
    else if (ea == ep && l > lp)
        *la = lp;
}

/* finds the stem [lo, hi] of direction y (it must exist) and marks it isolated */
static uint8_t *find_isolated(int32_t lo, int32_t hi, int32_t y, uint8_t *s) {
    uint8_t *a = STEMS(s);
    while (I32(a, S_LO) != lo || I32(a, S_HI) != hi || (int32_t)(U32(a, S_FL) & 1) != y)
        a = PTR(a, S_NEXT);
    uint32_t fl = U32(a, S_FL);
    PTR(a, S_PATH) = 0;
    PTR(a, S_CNT) = 0;
    U32(a, S_FL) = fl | 0x10;
    return a;
}

/* GCAlignIsolatedStem 0x1800aefa0 */
CFF_API void IMPL(GCAlignIsolatedStem)(int32_t lo, int32_t hi, int32_t y, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCALIGNISOLATEDSTEM
    return;
#endif
    uint8_t *a = find_isolated(lo, hi, y, s);
    O_GCCalcLocs(a, s);
    O_GCFix1Loc(a, s);
    int lockLo = 0, lockHi = 0;
    a_i32 *alo = (a_i32 *)(B(a) + S_LLO), *ahi = (a_i32 *)(B(a) + S_LHI);
    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT)) {
        if (y != (int32_t)(U32(p, S_FL) & 1) || a == p)
            continue;
        int32_t adl = I32(a, S_DLO), pdl = I32(p, S_DLO);
        int prop;
        if (adl > pdl)
            prop = I32(a, S_DHI) <= I32(p, S_DHI);
        else if (adl == pdl)
            prop = 1;
        else
            prop = I32(a, S_DHI) >= I32(p, S_DHI);
        if (prop) {
            int32_t pl = I32(p, S_LLO);
            int32_t num = SUB(pl, *alo);
            int32_t den = ADD(SUB(*ahi, I32(p, S_LHI)), num);
            int32_t span = ADD(SUB(SUB(*ahi, *alo), I32(p, S_LHI)), pl);
            int32_t f = O_fixratio(num, den);
            int32_t d = (int32_t)((uint32_t)ADD(O_fracmul(span, f), 0x8000) & 0xffff0000u);
            if (!lockLo)
                *alo = SUB(pl, d);
            if (!lockHi)
                *ahi = ADD(SUB(I32(p, S_LHI), d), span);
        }
        if (!lockLo) {
            order_fix(I32(a, S_LO), I32(p, S_LO), alo, I32(p, S_LLO), ahi, lockHi);
            order_fix(I32(a, S_LO), I32(p, S_HI), alo, I32(p, S_LHI), ahi, lockHi);
        }
        if (!lockHi) {
            order_fix(I32(a, S_HI), I32(p, S_LO), ahi, I32(p, S_LLO), alo, lockLo);
            order_fix(I32(a, S_HI), I32(p, S_HI), ahi, I32(p, S_LHI), alo, lockLo);
        }
        if (!lockLo) {
            order_clamp(I32(a, S_LO), I32(p, S_LO), alo, I32(p, S_LLO));
            order_clamp(I32(a, S_LO), I32(p, S_HI), alo, I32(p, S_LHI));
        }
        if (!lockHi) {
            order_clamp(I32(a, S_HI), I32(p, S_LO), ahi, I32(p, S_LLO));
            order_clamp(I32(a, S_HI), I32(p, S_HI), ahi, I32(p, S_LHI));
        }
        if (I32(a, S_HI) == I32(p, S_HI) && !lockHi) {
            *ahi = I32(p, S_LHI);
            lockHi = 1;
        }
        if (I32(a, S_LO) == I32(p, S_LO) && !lockLo) {
            *alo = I32(p, S_LLO);
            lockLo = 1;
        }
        if (lockHi == 1 && lockLo == 1)
            break;
    }
    U32(a, S_FL) |= 2;
    I32(a, S_HW) = tfm1(s, 0x598, SUB(*ahi, *alo), y) >> 1;
}
HEXPORT(GCAlignIsolatedStem)

/* GCAlignIsolatedStem_Postpass 0x1800af5e4 */
CFF_API void IMPL(GCAlignIsolatedStem_Postpass)(int32_t lo, int32_t hi, int32_t y, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GCALIGNISOLATEDSTEM_POSTPASS
    return;
#endif
    int32_t loMin, loMax;
    if (y == 0)
        loMin = I32(s, 0x360), loMax = I32(s, 0x368);
    else
        loMin = I32(s, 0x364), loMax = I32(s, 0x36c);
    int32_t hiMin = loMin, hiMax = loMax;
    uint8_t *a = find_isolated(lo, hi, y, s);
    O_GCCalcLocs(a, s);
    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT)) {
        uint32_t pf = U32(p, S_FL);
        if (y != (int32_t)(pf & 1) || a == p)
            continue;
        if ((U8(a, S_FL) & 0x60) && (pf & 0x60))
            continue;
        if (!(U8(a, S_FL) & 0x20)) {
            int32_t al = I32(a, S_LO);
            int skip_hi = 0;
            if (al > I32(p, S_LO)) {
                if (!(pf & 0x20) && loMin <= I32(p, S_LLO))
                    loMin = I32(p, S_LLO);
            } else if (al < I32(p, S_LO) && !(pf & 0x20)) {
                if (loMax >= I32(p, S_LLO))
                    loMax = I32(p, S_LLO);
                skip_hi = 1;
            }
            if (!skip_hi && al < I32(p, S_HI) && !(pf & 0x40)) {
                if (loMax >= I32(p, S_LHI))
                    loMax = I32(p, S_LHI);
            }
        }
        if (!(U8(a, S_FL) & 0x40)) {
            int32_t ah = I32(a, S_HI);
            if (ah > I32(p, S_HI) && !(U8(p, S_FL) & 0x40)) {
                if (hiMin <= I32(p, S_LHI))
                    hiMin = I32(p, S_LHI);
            } else {
                if (ah > I32(p, S_LO) && !(U8(p, S_FL) & 0x20)) {
                    if (hiMin <= I32(p, S_LLO))
                        hiMin = I32(p, S_LLO);
                }
                if (ah < I32(p, S_HI) && !(U8(p, S_FL) & 0x40)) {
                    if (hiMax >= I32(p, S_LHI))
                        hiMax = I32(p, S_LHI);
                }
            }
        }
        if (I32(a, S_LO) == I32(p, S_LO) && I32(p, S_LLO) >= loMin && I32(p, S_LLO) <= loMax)
            loMax = loMin = I32(p, S_LLO);
        if (I32(a, S_HI) == I32(p, S_HI) && I32(p, S_LHI) >= hiMin && I32(p, S_LHI) <= hiMax)
            hiMin = hiMax = I32(p, S_LHI);
        if (loMin >= loMax && hiMin >= hiMax)
            break;
    }
    int32_t l = I32(a, S_LLO), h, nl;
    if (l < loMin) {
        I32(a, S_LLO) = loMin;
        I32(a, S_LHI) = ADD(I32(a, S_LHI), SUB(loMin, l));
        l = loMin;
    } else if (l > loMax) {
        I32(a, S_LLO) = loMax;
        I32(a, S_LHI) = ADD(I32(a, S_LHI), SUB(loMax, l));
        l = loMax;
    }
    h = I32(a, S_LHI);
    if (h < hiMin) {
        nl = ADD(SUB(l, h), hiMin);
        I32(a, S_LHI) = hiMin;
        h = hiMin;
        l = nl < loMax ? nl : loMax;
        I32(a, S_LLO) = l;
    } else if (h > hiMax) {
        nl = ADD(SUB(l, h), hiMax);
        I32(a, S_LHI) = hiMax;
        h = hiMax;
        l = nl > loMin ? nl : loMin;
        I32(a, S_LLO) = l;
    }
    uint32_t fl = U32(a, S_FL);
    if (!(fl & 0x60) && SUB(h, l) < 0x10000) {
        int32_t d = ADD(SUB(l, h), 0x10000) >> 1;
        l = SUB(l, d);
        I32(a, S_LLO) = l;
        h = ADD(h, d);
        I32(a, S_LHI) = h;
    }
    U32(a, S_FL) = fl | 2;
    I32(a, S_HW) = tfm1(s, 0x598, SUB(h, l), y) >> 1;
}
HEXPORT(GCAlignIsolatedStem_Postpass)

/* ------------------------------------------------------------------------------------------ */
/* the lock arrays */

/* InsertLock 0x1800b125c: inserts the lock pair e (two 0x20-byte entries: +8 from, +0xc to,
 * +0x10 flags) into the sorted lock array arr */
CFF_API int32_t IMPL(InsertLock)(uint8_t *arr, int32_t *e_, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_INSERTLOCK
    return 0;
#endif
    uint8_t *e = B(e_);
    int first = 1;
    uint8_t *r9 = arr + 0x38, *cur, *end, *r8;
    int32_t mx, my, cx, cy;
    if ((uint32_t)I32(e, 8) + 0x27cffffeu > 0x4f9ffffcu)
        return 0;
    if ((uint32_t)I32(e, 0x28) + 0x27cffffeu > 0x4f9ffffcu)
        return 0;
    if (PTR(s, 0x428)) {
        PTR(s, 0x428) = 0;
        U32(PTR(s, 0x468), 0) = 1;
        U32(PTR(s, 0x470), 0) = 1;
        U32(PTR(s, 0x468), 0x40) = 0xd8300001u;
        U32(PTR(s, 0x470), 0x40) = 0xd8300001u;
    }
    if (U32(arr, 0) > 0x7c)
        return (int32_t)0xfe140169u;
    end = arr + 0x38 + ((int64_t)I32(arr, 0) << 5);
    mx = ADD(I32(e, 0x28), I32(e, 8)) >> 1;
    my = ADD(I32(e, 0x2c), I32(e, 0xc)) >> 1;
    r8 = end + 0x20;
    for (;;) {
        cur = r9;
        uint8_t *pa = PTR(cur, 0x18);
        cy = ADD(I32(pa, 0xc), I32(cur, 0xc)) >> 1;
        cx = ADD(I32(pa, 8), I32(cur, 8)) >> 1;
        if (mx == cx)
            return 0;
        if (mx < cx)
            goto before;
        r9 += 0x20;
        if (r9 == end)
            break;
        if (!(U32(r9, 0x10) & 0x100))
            continue;
        r9 += 0x20;
        first = 0;
        if (r9 == end)
            break;
    }
    if (my < cy)
        return 0;
    if (my == cy) {
        r9 -= 0x40;
        goto same;
    }
    memcpy(r8 + 8, e + 0x28, 8);
    U32(r8, 0x10) = U32(e, 0x30);
    memcpy(r8 - 0x18, e + 8, 8);
    U32(r8, -0x10) = U32(e, 0x10);
    PTR(r8, 0x18) = end;
    PTR(r8, -8) = r8;
    U32(arr, 0) += 2;
    return 0;
before:
    if (my > cy)
        return 0;
    if (my == cy)
        goto same;
    if (!first) {
        uint8_t *pa = PTR(r9, -0x28);
        r9 -= 0x40;
        int32_t py = ADD(I32(pa, 0xc), I32(r9, 0xc)) >> 1;
        if (my < py)
            return 0;
        if (my == py) {
            cx = ADD(I32(pa, 8), I32(r9, 8)) >> 1;
            goto same;
        }
    }
    {
        uint8_t *d = r8, *stop = cur + 0x20;
        do {
            memcpy(d + 8, d - 0x38, 8);
            U32(d, 0x10) = U32(d, -0x30);
            PTR(d, 0x18) = PTR(d, -0x28) + 0x40;
            d -= 0x20;
        } while (d != stop);
        memcpy(cur + 8, e + 8, 8);
        U32(cur, 0x10) = U32(e, 0x10);
        memcpy(cur + 0x28, e + 0x28, 8);
        U32(cur, 0x30) = U32(e, 0x30);
        PTR(cur, 0x18) = cur + 0x20;
        PTR(cur, 0x38) = cur;
        U32(arr, 0) += 2;
        return 0;
    }
same:
    if (!(U32(r9, 0x10) & 1)) {
        if (!(U32(e, 0x10) & 1) && mx <= cx)
            return 0;
    } else {
        if (!(U32(e, 0x10) & 1))
            return 0;
        if (mx <= cx)
            return 0;
    }
    memcpy(r9 + 8, e + 8, 8);
    U32(r9, 0x10) = U32(e, 0x10);
    memcpy(r9 + 0x28, e + 0x28, 8);
    U32(r9, 0x30) = U32(e, 0x30);
    PTR(r9, 0x18) = r9 + 0x20;
    PTR(r9, 0x38) = r9;
    return 0;
}
HEXPORT(InsertLock)

/* SetXLock 0x1800b0220 / SetYLock 0x1800b1200: the lock pair (a, b) -> (c, d) */
static int32_t set_lock(int32_t a, int32_t b, int32_t flags, int32_t c, int32_t d, uint8_t *s, int off) {
    int32_t e[0x40 / 4];
    e[2] = a;
    e[3] = b;
    e[4] = (int32_t)((uint32_t)flags | 0x200);
    e[10] = c;
    e[11] = d;
    e[12] = (int32_t)((uint32_t)flags | 0x100);
    return O_InsertLock(PTR(s, off), e, s);
}
CFF_API int32_t IMPL(SetXLock)(int32_t a, int32_t b, int32_t flags, int32_t c, int32_t d, uint8_t *s, void *p7) {
#ifdef CFF_CONTROL_HINT_SETXLOCK
    return 0;
#endif
    (void)p7;
    return set_lock(a, b, flags, c, d, s, 0x468);
}
HEXPORT(SetXLock)
CFF_API int32_t IMPL(SetYLock)(int32_t a, int32_t b, int32_t flags, int32_t c, int32_t d, uint8_t *s, void *p7) {
#ifdef CFF_CONTROL_HINT_SETYLOCK
    return 0;
#endif
    (void)p7;
    return set_lock(a, b, flags, c, d, s, 0x470);
}
HEXPORT(SetYLock)

/* GCDoLock 0x1800af904: the lock of the stem [lo, hi] found in the stem list */
CFF_API int32_t IMPL(GCDoLock)(int32_t lo, int32_t hi, int32_t y, uint8_t *s, void *p5) {
#ifdef CFF_CONTROL_HINT_GCDOLOCK
    return 0;
#endif
    uint8_t *p = O_GCFind(lo, hi, y, s);
    if (!p)
        return 1;
    int32_t mn = lo <= hi ? lo : hi, mx = lo <= hi ? hi : lo;
    Lock_t set = y ? O_SetYLock : O_SetXLock;
    int32_t r;
    if (!(U8(p, S_FL) & 0x60)) {
        int32_t v = tfm1(s, 0x598, ADD(I32(p, S_LHI) >> 1, I32(p, S_LLO) >> 1), y);
        int32_t hw = I32(p, S_HW);
        r = set(mn, SUB(v, hw), 1, mx, ADD(hw, v), s, p5);
    } else {
        int32_t v = tfm1(s, 0x598, (U8(p, S_FL) & 0x40) ? I32(p, S_LLO) : I32(p, S_LHI), y);
        int32_t m = (U8(p, S_FL) & 0x40) ? mn : mx;
        r = set(m, v, 0x11, m, v, s, p5);
    }
    if (r) {
        ERR(s) = 1;
        return 1;
    }
    return 0;
}
HEXPORT(GCDoLock)

/* MatchStem 0x1800b06d0: snaps the hint h = {edge, width} to the edges of a known stem */
CFF_API void IMPL(MatchStem)(int32_t *h, int32_t y, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_MATCHSTEM
    return;
#endif
    int32_t a = h[0], b = ADD(h[1], h[0]);
    int fa = 0, fb = 0;
    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT)) {
        if ((int32_t)(U32(p, S_FL) & 1) != y)
            continue;
        if (!fa) {
            if (iabs32(SUB(a, I32(p, S_LO))) < 0x20000) {
                a = I32(p, S_LO), fa = 1;
                if (fb)
                    break;
            }
            if (iabs32(SUB(a, I32(p, S_HI))) < 0x20000) {
                a = I32(p, S_HI), fa = 1;
                if (fb)
                    break;
            }
        }
        if (!fb) {
            if (iabs32(SUB(b, I32(p, S_LO))) < 0x20000) {
                b = I32(p, S_LO), fb = 1;
                if (fa)
                    break;
            }
            if (iabs32(SUB(b, I32(p, S_HI))) < 0x20000) {
                b = I32(p, S_HI), fb = 1;
                if (fa)
                    break;
            }
        }
    }
    h[1] = SUB(b, a);
    h[0] = a;
}
HEXPORT(MatchStem)

/* the ghost-hint offset of RXLock and BlueLock: -(((s16)((scale - 1) >> 2) * 0x4d41) >> 15) */
static inline int32_t ghost_off(int32_t scale) {
    return NEG(MUL((int16_t)(SUB(scale, 0x10000) >> 2), 0x4d41) >> 15);
}

/* PreXLock 0x1800b1090 / PreYLock 0x180184870: the rounded position (*loc) and centre (*mid)
 * of the stem [lo, hi]; returns its device half width */
static int32_t pre_lock(int32_t lo, int32_t hi, int32_t *loc, int32_t *mid, uint8_t *s, int y) {
    int32_t w = SUB(hi, lo), b[2];
    b[0] = y ? 0 : w;
    b[1] = y ? w : 0;
    pt_call(s, 0x590, b);
    int32_t t = b[y];
    int32_t dw = t >= 0 ? t : NEG(t), sw = dw;
    int32_t n = I32(s, y ? 0x424 : 0x420);
    if (n) {
        int32_t v = dw;
        O_UseStdWidth(w, &v, (int32_t *)PTR(s, y ? 0x418 : 0x410), n, 0, U16(s, 0), s);
        sw = v;
    }
    int32_t hw = w >> 1;
    b[0] = y ? 0 : ADD(hw, lo);
    b[1] = y ? ADD(hw, lo) : 0;
    *mid = ADD(hw, lo);
    pt_call(s, 0x580, b);
    int32_t e = I32(s, 0x448);
    int32_t r = O_Adjust(I32(s, 0x444), 0, sw, b[y], y ? U16(s, 0x330) : U16(s, 2), y ? U16(s, 0x338) : U16(s, 0x336),
                         y ? U16(s, 0x334) : U16(s, 0x332), e == -0x140000, e == -0x150000, s);
    b[y] = r;
    pt_call(s, 0x588, b);
    *loc = b[y];
    return O_CalcHW2(hw, dw, sw, y, s);
}
CFF_API int32_t IMPL(PreXLock)(int32_t lo, int32_t hi, int32_t *loc, int32_t *mid, uint8_t *s, void *p6) {
#ifdef CFF_CONTROL_HINT_PREXLOCK
    lo = ADD(lo, 0x10000);
#endif
    (void)p6;
    return pre_lock(lo, hi, loc, mid, s, 0);
}
HEXPORT(PreXLock)
CFF_API int32_t IMPL(PreYLock)(int32_t lo, int32_t hi, int32_t *loc, int32_t *mid, uint8_t *s, void *p6) {
#ifdef CFF_CONTROL_HINT_PREYLOCK
    lo = ADD(lo, 0x10000);
#endif
    (void)p6;
    return pre_lock(lo, hi, loc, mid, s, 1);
}
HEXPORT(PreYLock)

/* RXLock 0x1800b027c: the vstem hint h = {edge, width} (off: the current x offset) */
CFF_API void IMPL(RXLock)(int32_t *h, int32_t off, uint8_t *s, void *p4) {
#ifdef CFF_CONTROL_HINT_RXLOCK
    return;
#endif
    int32_t loc = 0, mid[2];
    if (iabs32(h[1]) < 0x40000)
        return;
    h[0] = ADD(h[0], off);
    I32(s, 0x444) = h[0];
    I32(s, 0x448) = h[1];
    O_MatchStem(h, 0, s);
    int32_t lo = h[0], hi = ADD(h[1], lo);
    if (hi < lo) {
        int32_t t = hi;
        hi = lo;
        lo = t;
    }
    int32_t r;
    switch (I32(s, 0x460)) {
    case 0:
        O_GCEnter(lo, hi, 0, I32(s, 0x448), s);
        return;
    case 1:
        r = O_GCDoLock(lo, hi, 0, s, p4);
        break;
    case 2: {
        int32_t hw = O_PreXLock(lo, hi, &loc, mid, s, p4);
        int32_t e = I32(s, 0x448);
        if (e == -0x150000) {
            int32_t t1 = tfm1(s, 0x598, ghost_off(I32(s, 0x2ec)), 0);
            int32_t t2 = tfm1(s, 0x598, ghost_off(I32(s, 0x2ec)), 0);
            r = O_SetXLock(lo, ADD(SUB(t2, hw), loc), 0x10, lo, ADD(SUB(t1, hw), loc), s, p4);
        } else if (e == -0x140000) {
            int32_t t1 = tfm1(s, 0x598, ghost_off(I32(s, 0x2ec)), 0);
            int32_t t2 = tfm1(s, 0x598, ghost_off(I32(s, 0x2ec)), 0);
            r = O_SetXLock(hi, ADD(SUB(loc, t2), hw), 0x10, hi, ADD(SUB(loc, t1), hw), s, p4);
        } else {
            int32_t el[0x40 / 4];
            el[2] = lo;
            el[3] = SUB(loc, hw);
            el[4] = 0x200;
            el[10] = hi;
            el[11] = ADD(loc, hw);
            el[12] = 0x100;
            r = O_InsertLock(PTR(s, 0x468), el, s);
        }
        break;
    }
    case 3:
        if (!O_GCDoLock(lo, hi, 0, s, p4))
            return;
        if (!O_GCEnter(lo, hi, 0, I32(s, 0x448), s))
            return;
        O_GCAlignIsolatedStem_Postpass(lo, hi, 0, s);
        r = O_GCDoLock(lo, hi, 0, s, p4);
        break;
    default:
        return;
    }
    if (r)
        ERR(s) = 1;
}
HEXPORT(RXLock)

/* RBLock 0x1800b0580: the hstem hint h = {edge, width} (off: the current y offset) */
CFF_API void IMPL(RBLock)(int32_t *h, int32_t off, uint8_t *s, void *p4) {
#ifdef CFF_CONTROL_HINT_RBLOCK
    return;
#endif
    if (iabs32(h[1]) < 0x40000)
        return;
    h[0] = ADD(h[0], off);
    I32(s, 0x444) = h[0];
    I32(s, 0x448) = h[1];
    O_MatchStem(h, 1, s);
    int32_t w = h[1], lo = h[0], hi;
    if (w > 0)
        hi = ADD(w, lo);
    else
        hi = lo, lo = ADD(lo, w);
    switch (I32(s, 0x460)) {
    case 0:
        O_GCEnter(lo, hi, 1, I32(s, 0x448), s);
        return;
    case 2:
        O_BlueLock(lo, hi, I32(s, 0x448), 0, s, p4);
        return;
    case 3:
        if (!O_GCDoLock(lo, hi, 1, s, p4))
            return;
        if (!O_GCEnter(lo, hi, 1, I32(s, 0x448), s))
            return;
        O_GCAlignIsolatedStem_Postpass(lo, hi, 1, s);
        /* fall through */
    case 1:
        if (O_GCDoLock(lo, hi, 1, s, p4))
            ERR(s) = 1;
        return;
    default:
        return;
    }
}
HEXPORT(RBLock)

/* TriLock 0x1800f326c: three equally spaced stems h = {edge, width} x 3 (hstem3 / vstem3) */
CFF_API void IMPL(TriLock)(int32_t *h, int32_t off, void *pre_, void *set_, uint16_t ov, uint8_t *s, void *p7) {
#ifdef CFF_CONTROL_HINT_TRILOCK
    return;
#endif
    PreLock_t pre = (PreLock_t)pre_;
    Lock_t set = (Lock_t)set_;
    int32_t loc[3] = {0, 0, 0}, mid[3] = {0, 0, 0}, w[3], hw[3], t;
    h[0] = ADD(h[0], off);
    h[4] = ADD(h[4], off);
    w[0] = h[1];
    w[1] = h[3];
    w[2] = h[5];
    h[2] = ADD(h[2], off);
    for (int k = 0; k < 3; k++) {
        I32(s, 0x444) = h[2 * k];
        I32(s, 0x448) = h[2 * k + 1];
        hw[k] = pre(h[2 * k], ADD(h[2 * k + 1], h[2 * k]), &loc[k], &mid[k], s, p7);
    }
    if (!set)
        return;
#define SWAP(i, j)                                                                                 \
    (t = mid[i], mid[i] = mid[j], mid[j] = t, t = loc[i], loc[i] = loc[j], loc[j] = t, t = w[i],   \
     w[i] = w[j], w[j] = t, t = hw[i], hw[i] = hw[j], hw[j] = t)
    if (mid[1] < mid[0])
        SWAP(0, 1);
    if (mid[2] < mid[0]) {
        SWAP(0, 2); /* (2, 1, 0) */
        SWAP(1, 2); /* (2, 0, 1) */
    } else if (mid[2] < mid[1])
        SWAP(1, 2);
#undef SWAP
    int32_t d1 = SUB(loc[1], loc[0]), d2 = SUB(loc[2], loc[1]), d;
    if (SUB(loc[2], loc[0]) > SUB(mid[2], mid[0]))
        d = d1 >= d2 ? d2 : d1;
    else
        d = d1 <= d2 ? d2 : d1;
    if (ov == 1) {
        loc[1] = ADD(d, loc[0]);
        loc[2] = ADD(loc[1], d);
    }
    int32_t flag = ov == 1;
    for (int k = 0; k < 3; k++) {
        int32_t half = iabs32(w[k]) >> 1;
        int32_t hh = hw[k] ? hw[k] : half;
        set(SUB(mid[k], half), SUB(loc[k], hh), flag, ADD(half, mid[k]), ADD(hh, loc[k]), s, p7);
    }
}
HEXPORT(TriLock)

/* RMLock 0x18010c260 / RVLock 0x18010c2b0: vstem3 / hstem3 */
CFF_API void IMPL(RMLock)(int32_t *h, int32_t off, uint8_t *s, void *p4) {
#ifdef CFF_CONTROL_HINT_RMLOCK
    return;
#endif
    if (I32(s, 0x460) != 2) {
        ERR(s) = 1;
        return;
    }
    O_TriLock(h, off, (void *)O_PreXLock, (void *)O_SetXLock, U16(s, 2), s, p4);
}
HEXPORT(RMLock)
CFF_API void IMPL(RVLock)(int32_t *h, int32_t off, uint8_t *s, void *p4) {
#ifdef CFF_CONTROL_HINT_RVLOCK
    return;
#endif
    if (I32(s, 0x460) != 2) {
        ERR(s) = 1;
        return;
    }
    O_TriLock(h, off, (void *)O_PreYLock, (void *)O_SetYLock, U16(s, 0x330), s, p4);
}
HEXPORT(RVLock)

/* ------------------------------------------------------------------------------------------ */
/* std widths and blue zones */

/* UseStdWidth 0x1800b1948: snaps the device width *out of the stem width w to the std width
 * table tbl (n entries: widths +0, classes +0x30, targets +0x60, costs +0x90); 1 if changed */
CFF_API int32_t IMPL(UseStdWidth)(int32_t w, int32_t *out, int32_t *tbl, int32_t n, int64_t unused, uint16_t ov,
                                  uint8_t *s) {
#ifdef CFF_CONTROL_HINT_USESTDWIDTH
    return 0;
#endif
    (void)unused;
    int32_t orig = *out, idx = -1, prev = 0, dist = 0, i;
    int32_t *cls = tbl + 12, *tgt = tbl + 24, *cost = tbl + 36;
    for (i = 0; i < n; i++) {
        if (cls[i] < 0)
            continue;
        int32_t v = tbl[i];
        if (v < w) {
            prev = v;
            dist = SUB(w, v);
            idx = i;
            continue;
        }
        if (v == w) {
            orig = -1;
            goto done;
        }
        if (prev == 0 || !(MUL(SUB(v, w), 2) >= SUB(w, prev))) {
            dist = SUB(v, w);
            idx = i;
        }
        break;
    }
    {
        int32_t best = INT32_MAX, r8 = INT32_MAX;
        int32_t c = cls[idx];
        if (orig > c) {
            if (SUB(orig, c) <= 0xae00) {
                *out = c;
                goto done;
            }
            int32_t edi = 0;
            for (int32_t j = idx + 1; j < n; j++) {
                if (cls[j] > 0)
                    break;
                if (iabs32(SUB(tbl[j], w)) > dist)
                    break;
                if (cost[j] < best) {
                    edi = NEG(cls[j]);
                    best = cost[j];
                    r8 = tgt[j];
                }
            }
            if (r8 < orig && !(SUB(orig, edi) > 0xae00))
                *out = r8;
            goto done;
        }
        for (int64_t j = (int64_t)idx - 1; j >= 0; j--) {
            if (cls[j] > 0)
                break;
            if (iabs32(SUB(tbl[j], w)) > dist)
                break;
            if (cost[j] < best) {
                best = cost[j];
                r8 = tgt[j];
            }
        }
        if (SUB(c, orig) <= 0x5700) {
            *out = r8 < c ? r8 : c;
            goto done;
        }
        int32_t f = O_fixmul(0x5700, ADD(O_fixdiv(orig, SUB(c, orig)), 0x10000));
        if (f > orig)
            *out = r8 < f ? r8 : f;
    }
done : {
    int32_t v = *out;
    if (v <= 0xbffe) {
        v = 0xbfff;
        *out = v;
    }
    int32_t mode = I32(s, 0x33c);
    if (((((uint32_t)mode - 2) & 0xfffffffau) == 0 && mode != 3) || mode == 5) {
        uint32_t m = mode == 2 ? ov : 1;
        int32_t f = O_FFloorFModN(v, (uint16_t)m);
        if (SUB(v, f) > SUB(SHL(m, 15), 1))
            f = ADD(f, SHL(m, 16));
        v = SHL(m, 16);
        if (f >= v)
            v = f;
        *out = v;
    }
    return v != orig;
}
}
HEXPORT(UseStdWidth)

/* AdjustToStdWidths 0x18010c144 */
CFF_API void IMPL(AdjustToStdWidths)(uint8_t *s) {
#ifdef CFF_CONTROL_HINT_ADJUSTTOSTDWIDTHS
    return;
#endif
    if (I32(s, 0x424) == 0 && I32(s, 0x420) == 0)
        return;
    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT)) {
        int hit = 0;
        if (U8(p, S_FL) & 1) {
            if (I32(s, 0x424))
                hit = O_UseStdWidth(SUB(I32(p, S_HI), I32(p, S_LO)), (int32_t *)(B(p) + S_W),
                                    (int32_t *)PTR(s, 0x418), I32(s, 0x424), 0, U16(s, 0), s) != 0;
            if (!hit && (U8(p, S_FL) & 1))
                continue;
        }
        if (!hit) {
            if (!I32(s, 0x420))
                continue;
            hit = O_UseStdWidth(SUB(I32(p, S_HI), I32(p, S_LO)), (int32_t *)(B(p) + S_W), (int32_t *)PTR(s, 0x410),
                                I32(s, 0x420), 0, U16(s, 0), s) != 0;
        }
        if (hit)
            U32(p, S_FL) |= 8;
    }
}
HEXPORT(AdjustToStdWidths)

/* RaiseTops 0x1800b00d8 */
CFF_API void IMPL(RaiseTops)(uint8_t *s) {
#ifdef CFF_CONTROL_HINT_RAISETOPS
    return;
#endif
    for (uint32_t i = 1; i < U32(s, 0x1b4); i++) {
        if ((U32(s, 0x2dc) >> (i & 31)) & 1)
            return;
        uint16_t ov = U16(s, 0x330);
        int32_t z = ADD(SHL((uint32_t)U16(s, 0x338), 16), I32(s, 0x1c8 + i * 0x14));
        int32_t a = O_FFloorFModN(z, ov);
        int32_t b = O_FFloorFModN(ADD(SUB(z, I32(s, 0x3d8)) >> 1, z >> 1), ov);
        if (b == a) {
            int32_t c = O_FFloorFModN(ADD(SHL((uint32_t)ov, 15), z), ov);
            if (iabs32(SUB(z, c)) > SHL((uint32_t)ov, 13))
                I32(s, 0x1c0 + i * 0x14) = ADD(SHL((uint32_t)ov, 16), a);
        }
    }
}
HEXPORT(RaiseTops)

/* BoostBotLocs 0x1800e2870 */
CFF_API void IMPL(BoostBotLocs)(uint8_t *s) {
#ifdef CFF_CONTROL_HINT_BOOSTBOTLOCS
    return;
#endif
    if (U32(s, 0x1b4) <= 2 || I32(s, 0x1c8) != 0)
        return;
    for (uint32_t i = 1; i < U32(s, 0x1b4); i++) {
        if (!((U32(s, 0x2dc) >> (i & 31)) & 1))
            continue;
        uint32_t ov = U16(s, 0x330);
        int32_t z = ADD(SHL((uint32_t)U16(s, 0x338), 16), I32(s, 0x1c8 + i * 0x14));
        int32_t az = iabs32(z);
        int32_t one = SHL(ov, 16);
        if (az <= one || az >= SHL(ov * 3, 15))
            continue;
        int32_t v = SUB((int32_t)~(uint32_t)one, SHL(ov, 15));
        I32(s, 0x1c0 + i * 0x14) = v;
        if (z > 0)
            I32(s, 0x1c0 + i * 0x14) = NEG(v);
    }
}
HEXPORT(BoostBotLocs)

/* BlueBoost 0x1800b0098 */
CFF_API void IMPL(BlueBoost)(uint8_t *s) {
#ifdef CFF_CONTROL_HINT_BLUEBOOST
    return;
#endif
    uint16_t v = U16(s, 0x338);
    if (v == U16(s, 0x3e4))
        return;
    U16(s, 0x3e4) = v;
    O_RaiseTops(s);
    O_BoostBotLocs(s);
}
HEXPORT(BlueBoost)

/* BlueLock 0x1800b07dc: locks the hstem [lo, hi] (edge: the hint's width, -0x150000 /
 * -0x140000 for ghost hints) against the blue zones; with a stem record (GCProcess), only
 * computes the stem's locked edges */
CFF_API void IMPL(BlueLock)(int32_t lo, int32_t hi, int32_t edge, uint8_t *stem, uint8_t *s, void *p6) {
#ifdef CFF_CONTROL_HINT_BLUELOCK
    return;
#endif
    int32_t A[2], Bp[2] = {0, 0}, w = SUB(hi, lo), hw = w >> 1, dw, sw, pos, E, r;
    int ghost = (((uint32_t)edge + 0x150000u) & 0xfffeffffu) == 0;
    A[0] = 0;
    A[1] = w;
    pt_call(s, 0x590, A);
    dw = A[1];
    sw = dw;
    if (stem) {
        sw = I32(stem, S_W);
    } else if (I32(s, 0x424) != 0 && !ghost) {
        int32_t v = dw;
        if (O_UseStdWidth(w, &v, (int32_t *)PTR(s, 0x418), I32(s, 0x424), 0, U16(s, 0), s))
            sw = v;
    }
    int32_t mid = ADD(hw, lo);
    uint32_t mask = U32(s, 0x2dc), m;
    uint8_t *z = 0, *zp;
    int32_t i;
    for (i = 0, m = mask, zp = s + 0x1b8; i < I32(s, 0x1b4); i++, m >>= 1, zp += 0x14) {
        if (!(m & 1)) {
            if (edge != -0x150000 && hi >= I32(zp, 4) && hi <= I32(zp, 0)) {
                z = zp;
                break;
            }
        } else if (edge != -0x140000 && lo >= I32(zp, 4) && lo <= I32(zp, 0)) {
            z = zp;
            break;
        }
    }
    int32_t fuzz = I32(s, 0x1ac);
    if (!z && fuzz != 0) {
        for (i = 0, m = mask, zp = s + 0x1b8; i < I32(s, 0x1b4); i++, m >>= 1, zp += 0x14) {
            if (!(m & 1)) {
                if (edge != -0x150000 && hi >= SUB(I32(zp, 4), fuzz) && hi <= ADD(fuzz, I32(zp, 0))) {
                    z = zp;
                    break;
                }
            } else if (edge != -0x140000 && lo >= SUB(I32(zp, 4), fuzz) && lo <= ADD(fuzz, I32(zp, 0))) {
                z = zp;
                break;
            }
        }
    }
    if (!z) {
        if (stem) {
            O_GCCalcLocs(stem, s);
            return;
        }
        Bp[1] = mid;
        pt_call(s, 0x580, Bp);
        Bp[1] = O_Adjust(I32(s, 0x444), 0, sw, Bp[1], U16(s, 0x330), U16(s, 0x338), U16(s, 0x334), edge == -0x140000,
                         edge == -0x150000, s);
        pt_call(s, 0x588, Bp);
        int32_t h2 = O_CalcHW2(hw, dw, sw, 1, s);
        if (edge == -0x150000) {
            int32_t t = tfm1(s, 0x598, ghost_off(I32(s, 0x2f0)), 1);
            int32_t v = ADD(Bp[1], SUB((int32_t)0xfff58000u, t));
            r = O_SetYLock(lo, v, 0x10, lo, v, s, p6);
        } else if (edge == -0x140000) {
            int32_t t = tfm1(s, 0x598, ghost_off(I32(s, 0x2f0)), 1);
            int32_t v = ADD(t, ADD(Bp[1], 0xa0000));
            r = O_SetYLock(hi, v, 0x10, hi, v, s, p6);
        } else {
            r = O_SetYLock(lo, SUB(Bp[1], h2), 0, hi, ADD(Bp[1], h2), s, p6);
        }
        if (r)
            ERR(s) = 1;
        return;
    }
    int32_t s1a8 = I32(s, 0x1a8), zbot = I32(z, 4), zt = I32(z, 0);
    int64_t idx = (z - (s + 0x1b8)) / 0x14;
    int32_t isb = (int32_t)((1u << (idx & 31)) & mask);
    uint16_t ov = U16(s, 0x330);
    int alt = 0;
    if (!isb) {
        E = iabs32(SUB(hi, zbot)) <= fuzz ? zbot : hi;
        int32_t f = O_fixmul(MUL((uint32_t)ov, s1a8), zt);
        int32_t bb = (int32_t)((uint32_t)ADD(f, 0x8000) & 0xffff0000u);
        A[0] = 0;
        A[1] = zbot;
        pt_call(s, 0x590, A);
        if (!((int32_t)((uint32_t)ADD(A[1], 0x8000) & 0xffff0000u) >= iabs32(bb))) {
            Bp[0] = 0;
            Bp[1] = zt;
            pt_call(s, 0x580, Bp);
            pos = O_FFloorFModN(ADD(SHL((uint32_t)ov, 15), I32(z, 8)), ov);
            alt = 1;
        }
    } else {
        E = iabs32(SUB(lo, zt)) <= fuzz ? zt : lo;
    }
    if (!alt) {
        int32_t fx;
        Bp[0] = 0;
        if (!isb)
            Bp[1] = zbot, fx = I32(z, 0xc);
        else
            Bp[1] = zt, fx = I32(z, 8);
        pos = O_FFloorFModN(ADD(SHL((uint32_t)ov, 15), fx), ov);
        pt_call(s, 0x580, Bp);
        int32_t d = isb ? SUB(zt, E) : SUB(E, zbot);
        if (d != 0) {
            int32_t v;
            A[0] = 0;
            A[1] = d;
            pt_call(s, 0x590, A);
            if (d < I32(s, 0x1b0)) {
                v = A[1];
            } else {
                int32_t h15 = SHL((uint32_t)ov, 15), r9 = SUB(h15, 1);
                int32_t e = SUB(O_fixmul(d, MUL((uint32_t)ov, s1a8)), h15);
                int32_t c = e <= r9 ? e : r9;
                int32_t lim = NEG(r9);
                if (c >= lim)
                    lim = c;
                v = SUB(A[1], lim);
            }
            int32_t f = O_FFloorFModN(ADD(SHL((uint32_t)ov, 15), v), ov);
            pos = isb ? SUB(pos, f) : ADD(pos, f);
        }
    }
    if (ghost) {
        if (stem) {
            O_GCCalcAnchorLocs(stem, pos, s);
            return;
        }
        Bp[0] = 0;
        int32_t h = sw >> 1;
        int32_t loc = ADD(edge != -0x140000 ? h : NEG(h), pos);
        Bp[1] = O_Adjust(I32(s, 0x444), 0, sw, loc, U16(s, 0x330), U16(s, 0x338), U16(s, 0x334), edge == -0x140000,
                         edge == -0x150000, s);
        pt_call(s, 0x588, Bp);
        if (edge == -0x150000) {
            int32_t t = tfm1(s, 0x598, ghost_off(I32(s, 0x2f0)), 1);
            int32_t v = ADD(Bp[1], SUB((int32_t)0xfff58000u, t));
            r = O_SetYLock(lo, v, 0x11, lo, v, s, p6);
        } else if (edge == -0x140000) {
            int32_t t = tfm1(s, 0x598, ghost_off(I32(s, 0x2f0)), 1);
            int32_t v = ADD(t, ADD(Bp[1], 0xa0000));
            r = O_SetYLock(hi, v, 0x11, hi, v, s, p6);
        } else {
            return;
        }
        if (r)
            ERR(s) = 1;
        return;
    }
    uint32_t rw = (uint32_t)ADD(sw, 0x8000) & 0xffff0000u;
    if (rw == 0 && (I32(s, 0x434) != 0 || I32(s, 0x44c) >= 0x8000))
        rw = 0x10000;
    int32_t half = (int32_t)rw >> 1;
    if (I32(s, 0x434) != 0) {
        if (I32(s, 0x430) != 0)
            half = ADD(half, 0x8000);
    } else if (I32(s, 0x44c) < 0x8000) {
        half = ADD(half, 0x8000);
    }
    pos = isb ? ADD(pos, half) : SUB(pos, half);
    if (stem) {
        O_GCCalcAnchorLocs(stem, pos, s);
        return;
    }
    Bp[1] = O_Adjust(I32(s, 0x444), 0, sw, pos, U16(s, 0x330), U16(s, 0x338), U16(s, 0x334), isb == 0, isb, s);
    pt_call(s, 0x588, Bp);
    int32_t h2 = O_CalcHW2(hw, dw, sw, 1, s);
    r = O_SetYLock(lo, SUB(Bp[1], h2), 1, hi, ADD(Bp[1], h2), s, p6);
    if (r)
        ERR(s) = 1;
}
HEXPORT(BlueLock)

/* ------------------------------------------------------------------------------------------ */
/* global counters */

/* NewGlbCounter 0x1800f1230: the next free counter record; grows (and relocates) the buffer */
CFF_API uint8_t *IMPL(NewGlbCounter)(uint8_t *s) {
#ifdef CFF_CONTROL_HINT_NEWGLBCOUNTER
    return 0;
#endif
    if ((uintptr_t)PTR(s, 0x498) >= (uintptr_t)PTR(s, 0x490)) {
        uint8_t *gb = PTR(s, 0x4a0);
        uint8_t *old = PTR(gb, 0);
        void *obj = ALLOCATOR(s);
        uint8_t *nw = ((realloc_fn)VFN(obj, 0x28))(obj, old, (uint32_t)(U32(gb, 8) + 0x30));
        if (!nw) {
            obj = ALLOCATOR(s);
            ((free_fn)VFN(obj, 0x10))(obj, PTR(gb, 0));
            U32(gb, 8) = 0;
            PTR(gb, 0) = 0;
            return 0;
        }
        U32(gb, 8) += 0x30;
        PTR(gb, 0) = nw;
        if (nw != old) {
            if (!old) {
                PTR(s, 0x498) = nw;
            } else {
#define RELOC(p) ((uint8_t *)((uint64_t)((int64_t)((uint64_t)(p) - (uint64_t)old) >> 4) * 0xaaaaaaaaaaaaaaabull * 3 * 16 + (uint64_t)nw))
                PTR(s, 0x498) = RELOC(PTR(s, 0x498));
                if (COUNTERS(s)) {
                    uint8_t *c = RELOC(COUNTERS(s));
                    COUNTERS(s) = c;
                    while (PTR(c, 0)) {
                        uint8_t *q = RELOC(PTR(c, 0));
                        PTR(c, 0) = q;
                        c = q;
                    }
                    for (uint8_t *p = STEMS(s); p; p = PTR(p, S_NEXT)) {
                        PTR(p, S_CNT) = RELOC(PTR(p, S_CNT));
                        if (PTR(p, S_PATH))
                            PTR(p, S_PATH) = RELOC(PTR(p, S_PATH));
                    }
                }
#undef RELOC
            }
        }
        PTR(s, 0x490) = nw + (uint64_t)(U32(gb, 8) / 0x30) * 0x30;
    }
    uint8_t *r = PTR(s, 0x498);
    I32(s, 0x3f0) += 1;
    PTR(s, 0x498) = r + 0x30;
    return r;
}
HEXPORT(NewGlbCounter)

/* EnterGlbCounter 0x1800f3590: a counter between stems a (above) and b when their counter
 * bands overlap enough */
CFF_API void IMPL(EnterGlbCounter)(uint8_t *a, uint8_t *b, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_ENTERGLBCOUNTER
    return;
#endif
    int32_t gap = SUB(I32(a, S_DLO), I32(b, S_DHI));
    int32_t bhi = I32(b, S_BHI), blo = I32(b, S_BLO), ahi = I32(a, S_BHI);
    int32_t mn = ahi >= bhi ? bhi : ahi;
    int32_t mx = I32(a, S_BLO) <= blo ? blo : I32(a, S_BLO);
    int32_t ov = SUB(mn, mx);
    if (ov <= 0)
        return;
    int32_t wa = SUB(ahi, I32(a, S_BLO)), wb = SUB(bhi, blo);
    if (wa >= wb)
        wa = wb;
    if (ADD(ov, ov) < wa)
        return;
    uint8_t *c = O_NewGlbCounter(s);
    if (!c) {
        ERR(s) = 1;
        return;
    }
    I32(c, 8) = gap;
    PTR(c, 0x10) = a;
    PTR(c, 0x18) = b;
    PTR(c, 0) = COUNTERS(s);
    COUNTERS(s) = c;
}
HEXPORT(EnterGlbCounter)

/* ExtendToAnchor 0x180184b24: follows the stem's smallest counter to the stem above */
CFF_API uint8_t *IMPL(ExtendToAnchor)(uint8_t *p) {
#ifdef CFF_CONTROL_HINT_EXTENDTOANCHOR
    return p;
#endif
    uint8_t *best = 0;
    int32_t m = 0x27100000;
    for (uint8_t *c = PTR(p, S_CNT); c; c = PTR(c, 0))
        if (I32(c, 8) < m)
            best = c, m = I32(c, 8);
    if (best) {
        PTR(PTR(best, 0x10), S_PATH) = best;
        p = PTR(best, 0x10);
    }
    return p;
}
HEXPORT(ExtendToAnchor)

/* SimpleCounter 0x180184d1c: whether the counter's band is mostly free of the stems between
 * its two stems (arr: the stems sorted by GCSortStems) */
CFF_API int32_t IMPL(SimpleCounter)(uint8_t **arr, uint8_t *c) {
#ifdef CFF_CONTROL_HINT_SIMPLECOUNTER
    return 0;
#endif
    int16_t L[24];
    uint8_t *b = PTR(c, 0x18), *a = PTR(c, 0x10);
    int32_t cnt = 2;
    int32_t lo = I32(a, S_BLO) <= I32(b, S_BLO) ? I32(b, S_BLO) : I32(a, S_BLO);
    int16_t si = (int16_t)(lo >> 16);
    L[0] = si;
    int32_t hi = I32(a, S_BHI) >= I32(b, S_BHI) ? I32(b, S_BHI) : I32(a, S_BHI);
    L[1] = (int16_t)(hi >> 16);
    int32_t span = (int32_t)L[1] - (int32_t)si;
    int64_t end = I16(a, S_IDX);
    for (int64_t k = (int64_t)(I16(b, S_IDX) + 1); k < end; k++) {
        uint8_t *p = arr[k];
        if (U8(p, S_FL) & 0x60)
            continue;
        int16_t di = I16(p, 0x1e), ph = I16(p, 0x22);
        int64_t rdx = (int64_t)cnt - 1;
        int16_t r11 = L[rdx];
        if (di >= r11 || ph <= si)
            continue;
        if (di < si)
            di = si;
        if (ph <= r11)
            r11 = ph;
        int64_t ix = 0;
        int32_t r8 = 0;
        if (di > si)
            do {
                ix++, r8++;
            } while (di > L[ix]);
        int16_t cx = L[ix];
        int32_t r10 = r8;
        if (r11 > cx)
            do {
                ix++, r10++;
            } while (r11 > L[ix]);
        if ((r8 & 1) && di == cx)
            r8++;
        if ((r10 & 1) && r11 == L[ix])
            r10++;
        int32_t odd10 = r10 & 1, odd8 = r8 & 1;
        int32_t d = odd10 - r10 + odd8 + r8;
        if (d == -cnt)
            return 0;
        int32_t ncnt = d + cnt;
        if (ncnt > 0x18)
            continue;
        int64_t dst = ncnt - 1, i8 = r8, i10 = r10;
        for (; rdx >= 0 && rdx >= i8; rdx--) {
            if (rdx > i10) {
                if (dst < 0)
                    return 0;
                L[dst--] = L[rdx];
                continue;
            }
            if (rdx == i10) {
                if (dst < 0)
                    return 0;
                L[dst--] = L[rdx];
                if (odd10) {
                    if (dst < 0)
                        return 0;
                    L[dst--] = r11;
                }
            }
            if (rdx == i8 && odd8) {
                if (dst < 0)
                    return 0;
                L[dst--] = di;
            }
        }
        si = L[0];
        cnt = ncnt;
    }
    int32_t sum = 0;
    for (int32_t k = 0; k < cnt; k++)
        sum += (k & 1) ? L[k] : -L[k];
    return ADD(sum, sum) >= span;
}
HEXPORT(SimpleCounter)

/* ClumpCounters 0x1800e9c34: merges counters of similar size (within tol) into groups */
CFF_API void IMPL(ClumpCounters)(uint8_t **arr, int32_t n, int32_t tol) {
#ifdef CFF_CONTROL_HINT_CLUMPCOUNTERS
    return;
#endif
    int32_t bestd = 0;
    for (int32_t i = 0; i < n; i++) {
        uint8_t *c = arr[i];
        U8(c, 0x2c) = (uint8_t)i;
        I32(c, 0x28) = I32(c, 8);
        I32(c, 0x24) = I32(c, 0x28);
        I32(c, 0x20) = I32(c, 0x24);
        U8(c, 0x2d) = 0;
    }
    for (;;) {
        int32_t best = -1;
        if (n <= 1)
            break;
        for (int32_t j = 1; j < n; j++) {
            uint8_t *d = arr[j];
            if (U8(d, 0x2d) != 0)
                continue;
            int32_t x = SUB(I32(arr[j - 1], 8), I32(d, 8));
            int32_t ad = x >= 0 ? x : SUB(I32(d, 8), I32(arr[j - 1], 8));
            if (best < 0 || bestd > ad)
                best = j, bestd = ad;
        }
        if (best < 0)
            break;
        uint8_t *cb = arr[best], *cp = arr[best - 1];
        U8(cb, 0x2d) = 1;
        int32_t lo = I32(cp, 0x20) >= I32(cb, 0x20) ? I32(cb, 0x20) : I32(cp, 0x20);
        int32_t hi = I32(cp, 0x24) <= I32(cb, 0x24) ? I32(cb, 0x24) : I32(cp, 0x24);
        if (SUB(hi, lo) > tol)
            continue;
        int32_t g = best - 1, e = g;
        int64_t r10 = (int64_t)best - 1;
        while (r10 > 0 && (int32_t)U8(arr[r10 - 1], 0x2c) == g)
            e--, r10--;
        for (int32_t k = e; k < best; k++)
            U8(arr[k], 0x2c) = U8(arr[best], 0x2c);
        int32_t last = U8(arr[e], 0x2c);
        if (e <= last)
            for (int32_t k = 0; k < last - e + 1; k++) {
                I32(arr[e + k], 0x20) = lo;
                I32(arr[e + k], 0x24) = hi;
            }
        if (e < 0)
            break;
    }
    if (n > 0) {
        uint32_t i = 0;
        do {
            uint8_t *d = arr[(int32_t)i];
            if (U8(d, 0x2c) > i) {
                int32_t mid = ADD(I32(d, 0x24) >> 1, I32(d, 0x20) >> 1);
                uint8_t **q = &arr[(int32_t)i];
                do {
                    I32(*q, 0x28) = mid;
                    q++;
                    i++;
                } while (i <= U8(d, 0x2c));
                i = U8(d, 0x2c);
            }
            i++;
        } while ((int32_t)i < n);
    }
}
HEXPORT(ClumpCounters)

/* CounterGt 0x1800e9f30: the order of SortGroupsByFrac */
CFF_API int32_t IMPL(CounterGt)(uint8_t *a, uint8_t *b) {
#ifdef CFF_CONTROL_HINT_COUNTERGT
    return 0;
#endif
    if ((uint8_t)(U8(a, 0x2c) ^ U8(b, 0x2c)) != 0) {
        int32_t va = I32(a, 0x28);
        int32_t fa = O_FFloorFModN(va, 1);
        int32_t vb = I32(b, 0x28);
        int32_t fb = O_FFloorFModN(vb, 1);
        int32_t rb = SUB(vb, fb), ra = SUB(va, fa);
        int32_t ia = fa == 0 && ra >= 0x8000;
        int32_t ib = fb == 0 && rb >= 0x8000;
        if (ia != ib)
            return ia;
        if (ra > rb)
            return 1;
        if (ra < rb)
            return 0;
    }
    uint8_t *sa = PTR(a, 0x18), *sb = PTR(b, 0x18);
    if (U8(sa, S_FL) & 1)
        return I32(sa, S_HI) < I32(sb, S_HI);
    return I32(sa, S_HI) > I32(sb, S_HI);
}
HEXPORT(CounterGt)

/* SortGroupsByFrac 0x1800e9e30 */
CFF_API void IMPL(SortGroupsByFrac)(uint8_t **arr, int32_t n) {
#ifdef CFF_CONTROL_HINT_SORTGROUPSBYFRAC
    return;
#endif
    int32_t last = n - 1;
    for (int32_t i = 0; i < last; i++) {
        int32_t best = i;
        if (i + 1 < n) {
            for (int32_t j = i + 1; j < n; j++)
                if (O_CounterGt(arr[best], arr[j]))
                    best = j;
            if (best != i) {
                uint8_t *t = arr[i];
                arr[i] = arr[best];
                arr[best] = t;
            }
        }
    }
    if (n > 0) {
        int32_t r8 = 0;
        int64_t w = 0;
        do {
            int32_t e = r8;
            while (e < last && (uint8_t)(U8(arr[e + 1], 0x2c) ^ U8(arr[e], 0x2c)) == 0)
                e++;
            if (r8 <= e) {
                int32_t k = e - r8 + 1;
                r8 += k;
                for (; k; k--)
                    U8(arr[w++], 0x2c) = (uint8_t)e;
            }
        } while (r8 < n);
    }
}
HEXPORT(SortGroupsByFrac)

/* GCFixOnePath 0x1800e9870: distributes the pixels of a chain of counters from stem top down
 * (arr: room for the chain's counters, max: its size) */
CFF_API int32_t IMPL(GCFixOnePath)(uint8_t *top, uint8_t **arr, int32_t pix, void *p4, int32_t max) {
#ifdef CFF_CONTROL_HINT_GCFIXONEPATH
    return 0;
#endif
    (void)p4;
    int32_t cnt = 0, sumw = 0;
    int64_t n = 0;
    if (!PTR(top, S_PATH))
        return 0;
    uint8_t *p = top;
    for (;;) {
        uint8_t *c = PTR(p, S_PATH);
        if (!c)
            break;
        if (p == PTR(c, 0x18))
            return 0;
        if (p != top && (U8(p, S_FL) & 2))
            break;
        sumw += I16(p, 0x0a);
        if (n >= (int64_t)max || n >= 300)
            return 0;
        arr[n] = c;
        cnt++;
        n++;
        p = PTR(PTR(p, S_PATH), 0x18);
    }
    if (cnt > 2 && n > 0)
        for (int64_t k = 0; k < n; k++) {
            U32(PTR(arr[k], 0x10), S_FL) &= ~0x10u;
            U32(PTR(arr[k], 0x18), S_FL) &= ~0x10u;
        }
    sumw += I16(p, 0x0a);
    int32_t f = O_fixdiv(MUL(SUB(I32(top, S_DHI), I32(top, S_DLO)), 12), SUB(I32(top, S_HI), I32(top, S_LO)));
    if (f >= 0x999a)
        f = 0x999a;
    O_ClumpCounters(arr, cnt, f);
    O_SortGroupsByFrac(arr, cnt);
    int32_t r8 = 0;
    for (int64_t k = 0; k < n; k++)
        r8 += I16(arr[k], 0x2a);
    int32_t base = I32(p, S_DLO), span = I32(top, S_DHI), half = pix >> 1;
    if (U8(p, S_FL) & 2) {
        base = I32(p, S_LLO);
        pix = SUB(pix, half);
    }
    if (U8(top, S_FL) & 2) {
        span = I32(top, S_LHI);
        pix = SUB(pix, half);
    }
    span = SUB(span, base);
    r8 = ADD(SUB(r8, ADD(span, 0x8000) >> 16), sumw);
    for (;;) {
        r8 = ADD(r8, cnt);
        if (r8 >= 0)
            break;
        for (int64_t k = 0; k < n; k++)
            I32(arr[k], 0x28) = ADD(I32(arr[k], 0x28), 0x10000);
    }
    while (r8 > cnt) {
        for (int64_t k = 0; k < n; k++)
            I32(arr[k], 0x28) = ADD(I32(arr[k], 0x28), (int32_t)0xffff0000);
        r8 = SUB(r8, cnt);
    }
    int32_t t = ADD(O_fixmul(span, pix), 0x8000) >> 16;
    if (t > 0 && r8 > 0) {
        uint32_t g = U8(arr[r8 - 1], 0x2c);
        if (g != (uint32_t)(r8 - 1)) {
            int32_t r10 = 0;
            while ((uint32_t)U8(arr[r10], 0x2c) < g)
                r10++;
            if (SUB(r8, r10) <= t)
                r8 = r10;
            else if (SUB((int32_t)g, r8) < t)
                r8 = (int32_t)g + 1;
        }
    }
    int32_t sumf = 0, i9 = 0;
    for (int64_t k = 0; k < n; k++) {
        uint8_t *c = arr[k];
        uint32_t v = U32(c, 0x28);
        if (v == 0xffff) {
            U32(c, 0x28) = 0x10000;
            r8++;
        } else if (i9 < r8) {
            U16(c, 0x28) = 0;
        } else {
            U32(c, 0x28) = (v & 0xffff0000u) + 0x10000;
        }
        i9++;
        sumf += I16(arr[k], 0x2a);
    }
    uint32_t fl = U32(top, S_FL);
    int32_t sw = SHL(sumw, 16), sf = SHL(sumf, 16);
    int32_t r11 = SUB(SUB(span, sw), sf);
    if (!(fl & 2)) {
        int32_t sum = ADD(sf, sw);
        int32_t w = iabs32(SUB(I32(top, S_LHI), I32(top, S_LLO)));
        if (U8(p, S_FL) & 2) {
            r11 = ADD(I32(p, S_LLO), sum);
        } else {
            int32_t r10 = ADD(I32(top, S_DHI), I32(top, S_DLO));
            int32_t q8 = ADD(I32(p, S_DHI), I32(p, S_DLO));
            int32_t a9 = (int32_t)((uint32_t)ADD(ADD(SUB(r10, r11), w) / 2, 0x8000) & 0xffff0000u);
            int32_t pl = SUB(I32(p, S_LLO), I32(p, S_LHI));
            int32_t ea = (int32_t)((uint32_t)ADD(ADD(ADD(pl, q8), r11) / 2, 0x8000) & 0xffff0000u);
            int32_t cand = ADD(sum, ea);
            int32_t e1 = ADD(ADD(ADD(SUB(pl, MUL(ADD(ea, cand), 2)), q8), r10), w);
            int32_t e2 = ADD(ADD(ADD(ADD(SUB(MUL(ADD(SUB(sw, MUL(a9, 2)), sf), 2), I32(p, S_LHI)), I32(p, S_LLO)), q8), r10), w);
            r11 = iabs32(e2) < iabs32(e1) ? a9 : cand;
        }
        I32(top, S_LHI) = r11;
        I32(top, S_LLO) = SUB(r11, w);
        U32(top, S_FL) = fl | 2;
        I16(top, S_LEVEL) = 0;
    }
    if (!PTR(top, S_PATH))
        return 0;
    uint8_t *q = top;
    do {
        uint8_t *c = PTR(q, S_PATH);
        int32_t v = SUB(I32(q, S_LLO), I32(c, 0x28));
        q = PTR(c, 0x18);
        uint32_t qf = U32(q, S_FL);
        if (qf & 2)
            return 1;
        int32_t w = SUB(I32(q, S_LHI), I32(q, S_LLO));
        I32(q, S_LHI) = v;
        I32(q, S_LLO) = SUB(v, w);
        U32(q, S_FL) = qf | 2;
        I16(q, S_LEVEL) = 0;
    } while (PTR(q, S_PATH));
    return 1;
}
HEXPORT(GCFixOnePath)

/* FixBands 0x1800f2e90: walks the counter graph by depth, fixing one path at a time */
CFF_API int32_t IMPL(FixBands)(uint8_t **stems, int32_t n, uint8_t **ctrs, int32_t pix, uint8_t *s) {
#ifdef CFF_CONTROL_HINT_FIXBANDS
    return 0;
#endif
    for (int64_t i = 0; i < n; i++) {
        I16(stems[i], S_LEVEL) = 0;
        PTR(stems[i], S_PATH) = 0;
    }
    int32_t maxl = 0;
    uint8_t *best = 0;
    if (n <= 0)
        return 0;
    for (int32_t i = 0; i < n; i++) {
        uint8_t *a = stems[i];
        int16_t lv = (int16_t)(I16(a, S_LEVEL) + 1);
        for (uint8_t *c = PTR(a, S_CNT); c; c = PTR(c, 0)) {
            uint8_t *o = PTR(c, 0x10);
            if (U8(o, S_FL) & 2)
                continue;
            int take = 0;
            if (I16(o, S_LEVEL) < lv)
                take = 1;
            else if (I16(o, S_LEVEL) == lv) {
                int32_t ohi = I32(o, S_BHI), olo = I32(o, S_BLO);
                uint8_t *pc = PTR(o, S_PATH);
                a = stems[i];
                int32_t m1 = I32(a, S_BHI) >= ohi ? ohi : I32(a, S_BHI);
                int32_t x1 = I32(a, S_BLO) <= olo ? olo : I32(a, S_BLO);
                int32_t ov1 = SUB(m1, x1);
                uint8_t *ps = PTR(pc, 0x18);
                int32_t m2 = I32(ps, S_BHI) >= ohi ? ohi : I32(ps, S_BHI);
                int32_t x2 = I32(ps, S_BLO) <= olo ? olo : I32(ps, S_BLO);
                int32_t ov2 = SUB(m2, x2);
                if (ov1 > ov2)
                    take = 1;
                else if (ov1 == ov2 && I32(c, 8) < I32(pc, 8))
                    take = 1;
            }
            if (take) {
                I16(o, S_LEVEL) = lv;
                PTR(PTR(c, 0x10), S_PATH) = c;
                if ((int32_t)lv > maxl) {
                    best = PTR(c, 0x10);
                    maxl = lv;
                }
            }
        }
    }
    if (!best)
        return 0;
    uint8_t *top = O_ExtendToAnchor(best);
    for (;;) {
        if (!O_GCFixOnePath(top, ctrs, pix, 0, I32(s, 0x3f0)))
            return 0;
        int32_t m = 0;
        uint8_t *nb = 0;
        for (int32_t i = 0; i < n; i++) {
            uint8_t *a = stems[i];
            if (!(U8(a, S_FL) & 2) && PTR(a, S_PATH)) {
                int16_t lv = (int16_t)(I16(PTR(PTR(a, S_PATH), 0x18), S_LEVEL) + 1);
                I16(a, S_LEVEL) = lv;
                if ((int32_t)lv > m) {
                    nb = stems[i];
                    m = lv;
                }
            }
        }
        if (!nb)
            return 1;
        top = O_ExtendToAnchor(nb);
    }
}
HEXPORT(FixBands)

/* GlobalColoring 0x180184b6c */
CFF_API int32_t IMPL(GlobalColoring)(uint8_t *stems, uint8_t *ctrs, void *buf, int32_t pix, int32_t nctr, int32_t nstem,
                                     uint8_t *s) {
#ifdef CFF_CONTROL_HINT_GLOBALCOLORING
    return 1;
#endif
    if (nstem == 0)
        return 1;
    if ((uint32_t)nstem > 12 || (uint32_t)nctr > 11)
        return 0;
    if (!grow_ptrs(B(buf), (uint32_t)(nstem + nctr) << 3, s))
        return 0;
    uint8_t **arr = (uint8_t **)PTR(buf, 0);
    uint8_t **carr = arr + (int64_t)(int32_t)((uint32_t)nstem * 8) / 8;
    O_GCSortStems(stems, arr, nstem);
    for (int32_t i = 0; i < nstem; i++)
        U32(arr[i], S_FL) |= 0x10;
    int32_t k = 0;
    if (ctrs) {
        int64_t j = 0;
        for (uint8_t *c = ctrs; c; c = PTR(c, 0)) {
            if (j >= (int64_t)nctr)
                break;
            if (O_SimpleCounter(arr, c)) {
                k++;
                carr[j++] = c;
            }
        }
    }
    for (uint8_t *p = stems; p; p = PTR(p, S_NEXT))
        PTR(p, S_CNT) = 0;
    for (int64_t j = 0; j < k; j++) {
        uint8_t *c = carr[j];
        PTR(c, 0) = PTR(PTR(c, 0x18), S_CNT);
        c = carr[j];
        PTR(PTR(c, 0x18), S_CNT) = c;
    }
    if (!O_FixBands(arr, nstem, carr, pix, s))
        return 0;
    O_AlignIsolatedStems(arr, nstem, s);
    return 1;
}
HEXPORT(GlobalColoring)
