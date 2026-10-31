/*
 * Opening a font and setting up a transformation in DWrite.dll 10.0.26100.9711's TrueType
 * scaler: fs_NewSfnt with the work-space layout (fsg_PrivateFontSpaceSize,
 * fsg_WorkSpaceSetOffsets), and fs__NewTransformation with what it calls in this group
 * (font program, pre-program, the ClearType sub-pixel copy of the global state, the matrix
 * helpers). Callees of other groups (sfac_*, scl_*, itrp_Execute) are called at their RVA.
 *
 * Within the group, every call the original makes goes through the original's entry
 * (ORIG), so each port can be swapped in alone (reimpl/oracle.py --swap) and, with all
 * swapped, the hooks route the calls to the ports.
 *
 * Structures are reached by offset:
 *   in   fs_GlyphInputType      (in + 8 + 8 i = memory base i)
 *   info fs_GlyphInfoType       (info + 4 i = memory size i)
 *   c    sfac_ClientRec         (memory base 0)
 *   G    fnt_GlobalGraphicStateType, in the private font space (memory base 4)
 */
#include <stdint.h>
#include <string.h>
#include "tt_state.h"
#include "orig.h"

/* RVAs of the callees (DWrite.dll 10.0.26100.9711) */
#define RVA_SafeIntOnOverflow             0x1090b0
#define RVA_sfac_DoOffsetTableMap         0x091e38
#define RVA_sfac_LoadCriticalSfntMetrics  0x091854
#define RVA_sfac_ComputeMapping           0x0927b4
#define RVA_fsg_PrivateFontSpaceSize      0x0924c8
#define RVA_fsg_WorkSpaceSetOffsets       0x0921b8
#define RVA_sfac_CopyFontAndPrePrograms   0x00bc38
#define RVA_sfac_CopyCVT                  0x00c0f0
#define RVA_scl_InitializeScaling         0x00ac08
#define RVA_scl_Scale                     0x00c270
#define RVA_itrp_Execute                  0x00b7dc
#define RVA_itrp_RoundToGrid              0x0ad020
#define RVA_fsg_QueryGlobalGSSubPixel     0x003050
#define RVA_fsg_QueryTwilightElementSubPixel 0x0030b0
#define RVA_fsg_CopyFontProgramResults    0x008b94
#define RVA_fsg_InitInterpreterTrans      0x008c70
#define RVA_fsg_RunPreProgram             0x00be70
#define RVA_mth_90degRotationFactor       0x00be00
#define RVA_mth_PositiveSquare            0x00abe4
#define RVA_mth_Max45Trick                0x0e4320
#define RVA_IsGrayScale                   0x0d3ea8
#define RVA_nop_release                   0x0e5100 /* an empty function (folded destructor) */


/* SafeIntExceptionHandler<SafeIntRasterizerException>::SafeIntOnOverflow throws; callers
 * return a dummy value after it for the compiler's sake. */
static void safeint_overflow(void) { ORIG(void (*)(void), RVA_SafeIntOnOverflow)(); }

/* base + offset with SafeInt's pointer-overflow check (add, then compare with base) */
static uint8_t *ptr_add(uintptr_t base, uint32_t off) {
    uintptr_t p = base + off;
    if (p < base) safeint_overflow();
    return (uint8_t *)p;
}

/*
 * Leaf functions: the original's callers (built with whole-program optimization) keep values
 * in volatile registers across calls to them (fs__NewTransformation keeps R8 across
 * mth_PositiveSquare, R9/R10 across mth_Max45Trick, RDX/R9 across IsGrayScale), so a port
 * swapped in for one of them must preserve every register except RAX. The export is a thunk
 * that saves RCX, RDX, R8-R11 and XMM0-XMM5 around the C implementation.
 *
 * Standalone mode (-DIDWR_STANDALONE): every caller is a C port, so there is no thunk. The
 * defines below rename each implementation to its export, which is then the plain C function
 * on any ABI; PRESERVING_EXPORT only checks that the rename is there.
 */
#ifdef IDWR_STANDALONE
#define scaler_init_Max45Trick                   tt_mth_Max45Trick
#define scaler_init_90degRotationFactor          tt_mth_90degRotationFactor
#define scaler_init_IsGrayScale                  tt_IsGrayScale
#define scaler_init_PrivateFontSpaceSize         tt_fsg_PrivateFontSpaceSize
#define scaler_init_WorkSpaceSetOffsets          tt_fsg_WorkSpaceSetOffsets
#define scaler_init_QueryGlobalGSSubPixel        tt_fsg_QueryGlobalGSSubPixel
#define scaler_init_QueryTwilightElementSubPixel tt_fsg_QueryTwilightElementSubPixel
#define scaler_init_CopyFontProgramResults       tt_fsg_CopyFontProgramResults
#define PRESERVING_EXPORT(name, impl) _Static_assert(sizeof(&name) == sizeof(&impl), #name)
#else
#define PRESERVING_EXPORT(name, impl)                                                  \
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
            "movaps %xmm0, 0x20(%rsp)\n.seh_savexmm %xmm0, 0x20\n"                     \
            "movaps %xmm1, 0x30(%rsp)\n.seh_savexmm %xmm1, 0x30\n"                     \
            "movaps %xmm2, 0x40(%rsp)\n.seh_savexmm %xmm2, 0x40\n"                     \
            "movaps %xmm3, 0x50(%rsp)\n.seh_savexmm %xmm3, 0x50\n"                     \
            "movaps %xmm4, 0x60(%rsp)\n.seh_savexmm %xmm4, 0x60\n"                     \
            "movaps %xmm5, 0x70(%rsp)\n.seh_savexmm %xmm5, 0x70\n"                     \
            ".seh_endprologue\n"                                                       \
            "callq " #impl "\n"                                                        \
            "movaps 0x20(%rsp), %xmm0\n"                                               \
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

/* ======================================================================================
 * mth_* and IsGrayScale
 * ====================================================================================== */

/* mth_PositiveSquare (0x18000abe4) belongs to the scl group; it is called at its RVA. */

/* mth_Max45Trick: |a| and |b| (|INT_MIN| stays INT_MIN) differ by at most 0x21. The original
 * returns SETLE in AL over the smaller magnitude, which stays in the upper bits of EAX. */
uint32_t scaler_init_Max45Trick(uint64_t a_, uint64_t b_) {
    int32_t a = (int32_t)a_, b = (int32_t)b_;
    int32_t na = (int32_t)(0u - (uint32_t)a), nb = (int32_t)(0u - (uint32_t)b);
    int32_t ma = na < 0 ? a : na, mb = nb < 0 ? b : nb;
    int32_t hi = ma, lo = mb;
    if (ma < mb) { hi = mb; lo = ma; }
    int32_t diff = (int32_t)((uint32_t)hi - (uint32_t)lo);
    return ((uint32_t)lo & 0xffffff00u) | (diff <= 0x21);
}
PRESERVING_EXPORT(tt_mth_Max45Trick, scaler_init_Max45Trick);

/* mth_90degRotationFactor: 0 identity-like (a, d > 0), 1/3 for +-90 degrees, 2 for 180,
 * 4 for anything else. */
static uint32_t rotation_factor(const uint8_t *m) {
    int32_t a = AT(m, 0, int32_t), b = AT(m, 4, int32_t), c = AT(m, 0xc, int32_t), d = AT(m, 0x10, int32_t);
    if (c == 0 && b == 0) {
        if (a > 0) return d > 0 ? 0 : 4;
        if (a == 0) return 4;
        return d < 0 ? 2 : 4;
    }
    if (a != 0 || d != 0) return 4;
    if (c < 0) return b > 0 ? 1 : 4;
    if (c == 0) return 4;
    return (b >= 0) + 3;
}
uint32_t scaler_init_90degRotationFactor(const uint8_t *m) { return rotation_factor(m); }
PRESERVING_EXPORT(tt_mth_90degRotationFactor, scaler_init_90degRotationFactor);

/* IsGrayScale(input, client): the transformation is grayscale (client + 0x488 = input + 0xc0,
 * the overscale-related word) unless input + 0x8c says 2. EAX keeps the word's high byte. */
uint32_t scaler_init_IsGrayScale(const uint8_t *in, const uint8_t *c) {
    uint32_t v = AT(in, 0x8c, int32_t) == 2 ? 0 : AT(c, 0x488, uint16_t);
    return (v & 0xff00) | (v != 0);
}
PRESERVING_EXPORT(tt_IsGrayScale, scaler_init_IsGrayScale);

#define CALL_PositiveSquare(m) ((uint8_t)ORIG(uint8_t (*)(const uint8_t *), RVA_mth_PositiveSquare)(m))
#define CALL_Max45Trick(a, b) ((uint8_t)ORIG(uint8_t (*)(int32_t, int32_t), RVA_mth_Max45Trick)(a, b))
#define CALL_90deg(m) ORIG(uint16_t (*)(const uint8_t *), RVA_mth_90degRotationFactor)(m)
#define CALL_IsGrayScale(in, c) ((uint8_t)ORIG(uint8_t (*)(const uint8_t *, const uint8_t *), RVA_IsGrayScale)(in, c))

/* ======================================================================================
 * fs_NewSfnt and the work-space layout
 * ====================================================================================== */

/* fsg_PrivateFontSpaceSize(client, maxp, offsets): lays out the private font space (memory
 * base 4: storage, CVT, the global graphics state and its sub-pixel copy, twilight zones,
 * font and pre-program copies) and returns its size. Every addition has SafeInt's unsigned
 * overflow check. maxp fields: +0x10 twilight points, +0x12 storage, +0x14 FDEFs,
 * +0x16 IDEFs. */
uint32_t scaler_init_PrivateFontSpaceSize(const uint8_t *c, const uint8_t *mp, uint8_t *o) {
#define O(off) AT(o, off, uint32_t)
#define ADD(dst, a, b) do { (dst) = (uint32_t)(a) + (uint32_t)(b); } while (0)
#define FAIL_IF(cond) do { if (cond) { safeint_overflow(); return 0; } } while (0)
    uint32_t a, x, y, z, cv;
    O(0) = 0;
    a = (uint32_t)AT(mp, 0x12, uint16_t) << 2;
    O(4) = a;
    x = (uint32_t)AT(mp, 0x14, uint16_t) << 3;
    ADD(y, x, a); FAIL_IF(y < x);
    O(8) = y;
    x = (uint32_t)AT(mp, 0x16, uint16_t) << 3;
    ADD(z, x, y); FAIL_IF(z < x);
    O(0xc) = z;
    /* CVT: (client + 0x44 bytes) / 2 entries of 4 bytes, at least 0x400 bytes */
    uint32_t cvt_bytes = AT(c, 0x44, uint32_t);
    uint64_t cv64 = (uint64_t)(cvt_bytes ? cvt_bytes >> 1 : 0) << 2;
    FAIL_IF(cv64 > 0xffffffffu);
    cv = (uint32_t)cv64;
    if (cv < 0x400) cv = 0x400;
    z += cv; FAIL_IF(z < cv);
    O(0x10) = z;
    x = z + 7; FAIL_IF(x < z);
    x &= ~7u; O(0x10) = x;
    z = x + 0x1e8; FAIL_IF(z < 0x1e8);
    O(0x70) = z;
    /* second copy (sub-pixel): axis coordinates, storage, FDEFs, IDEFs, CVT */
    x = (uint32_t)AT(c, 0x110, uint16_t) * 2;
    z += x; FAIL_IF(z < x); O(0x54) = z;
    x = (uint32_t)AT(mp, 0x12, uint16_t) << 2;
    z += x; FAIL_IF(z < x); O(0x58) = z;
    x = (uint32_t)AT(mp, 0x14, uint16_t) << 3;
    z += x; FAIL_IF(z < x); O(0x5c) = z;
    x = (uint32_t)AT(mp, 0x16, uint16_t) << 3;
    z += x; FAIL_IF(z < x); O(0x60) = z;
    z += cv; FAIL_IF(z < cv);
    O(0x64) = z;
    x = z + 7; FAIL_IF(x < z);
    x &= ~7u; O(0x64) = x;
    y = x + 0x1e8; FAIL_IF(y < 0x1e8);
    O(0x14) = y;
    /* font program, pre-program */
    y += AT(c, 0x6c, uint32_t); FAIL_IF(y < AT(c, 0x6c, uint32_t)); O(0x18) = y;
    y += AT(c, 0x4c, uint32_t); FAIL_IF(y < AT(c, 0x4c, uint32_t));
    O(0x1c) = y;
    x = y + 7; FAIL_IF(x < y);
    x &= ~7u; O(0x1c) = x;
    /* two twilight zone structs (0x70 each) */
    y = x + 0x70; FAIL_IF(y < 0x70);
    O(0x68) = y;
    x = y + 7; FAIL_IF(x < y);
    x &= ~7u; O(0x68) = x;
    x += 0x70; FAIL_IF(x < 0x70);
    O(0x20) = x;
    y = x + 3; FAIL_IF(y < x);
    y &= ~3u; O(0x20) = y;
    /* twilight zone arrays */
    uint32_t n = AT(mp, 0x10, uint16_t);
    O(0x3c) = 0;
    x = n + 1; FAIL_IF(x < n);
    x &= ~1u; O(0x40) = x;
    y = x + 2; FAIL_IF(y < x);
    x = y + 2; O(0x44) = y; FAIL_IF(x < y);
    uint32_t r9 = x + 1; O(0x4c) = x; FAIL_IF(r9 < x);
    y = r9 + 3; FAIL_IF(y < r9);
    y &= ~3u;
    uint32_t n4 = n * 4;
    O(0x24) = y;
    uint32_t r10 = n4 + y; FAIL_IF(r10 < y);
    r9 = r10 + n4; O(0x28) = r10; FAIL_IF(r9 < r10);
    FAIL_IF(r9 + 7 < r9);              /* alignment check whose result is not used */
    r10 = r9 + n4; O(0x2c) = r9; FAIL_IF(r10 < r9);
    FAIL_IF(r10 + 7 < r10);            /* same */
    y = r10 + n4; O(0x30) = r10; FAIL_IF(y < r10);
    r9 = y + n4; O(0x34) = y; FAIL_IF(r9 < y);
    y = r9 + n4; O(0x38) = r9; FAIL_IF(y < r9);
    x = y + n; O(0x48) = y; FAIL_IF(x < y);
    y = x + 3; FAIL_IF(y < x);
    y &= ~3u;
    O(0x50) = y;
    r9 = y + n * 3 * 4; FAIL_IF(r9 < y);
    y = O(0x20) + r9; FAIL_IF(y < r9);
    O(0x6c) = y;
    x = y + 3; FAIL_IF(x < y);
    x &= ~3u; O(0x6c) = x;
    a = r9 + x; FAIL_IF(a < x);
    FAIL_IF(O(0) > a);
    return a - O(0);
#undef ADD
}
PRESERVING_EXPORT(tt_fsg_PrivateFontSpaceSize, scaler_init_PrivateFontSpaceSize);

/* fsg_WorkSpaceSetOffsets(maxp, offsets, *private_size): lays out the per-glyph work space
 * (memory base 3: element struct array for the component depth, glyph zone arrays with the
 * phantom points, the interpreter stack) and returns its size. maxp fields: +6 points,
 * +8 contours, +0xa composite points, +0xc composite contours, +0x18 stack elements,
 * +0x1c component elements, +0x1e component depth. */
uint32_t scaler_init_WorkSpaceSetOffsets(const uint8_t *mp, uint8_t *o, int32_t *out) {
    uint32_t edx, ecx, eax, r8, r11, ebp, edi16, r11c, uv;
    uint16_t ax, di, dx;
    O(8) = 0;
    /* element structs: depth clamped to 1..20, plus one */
    ax = AT(mp, 0x1e, uint16_t);
    if (ax < 1) ax = 1;
    else if (ax > 0x14) ax = 0x14;
    edx = ((uint32_t)ax + 1) * 0x70;
    ecx = edx + 1; FAIL_IF(ecx < edx);
    ecx &= ~1u;
    O(4) = ecx;
    /* points: max(points, composite points), at least 1, plus 8 phantom/extra */
    ax = AT(mp, 0xa, uint16_t);
    if (AT(mp, 6, uint16_t) >= ax) ax = AT(mp, 6, uint16_t);
    if (ax < 1) ax = 1;
    edx = (uint32_t)ax + 8; FAIL_IF(edx > 0xffff);
    eax = AT(mp, 0x1c, uint16_t);
    uint32_t edi = eax + 4; FAIL_IF(edi > 0xffff); FAIL_IF(edi < eax);
    di = (uint16_t)edi; dx = (uint16_t)edx;
    if (dx >= di) di = dx;
    /* contours: max(contours, composite contours), at least 1 */
    ax = AT(mp, 0xc, uint16_t);
    if (AT(mp, 8, uint16_t) >= ax) ax = AT(mp, 8, uint16_t);
    if (ax < 1) ax = 1;
    O(0x2c) = 0;
    ebp = di;
    edx = ebp + 1; FAIL_IF(edx < ebp);
    edx &= ~1u;
    r11 = ax;
    O(0x30) = edx;
    eax = r11 * 2;
    r8 = eax + edx; FAIL_IF(r8 < edx);
    edx = r8 + eax; O(0x34) = r8; FAIL_IF(edx < r8);
    eax = edx + r11; O(0x3c) = edx; FAIL_IF(eax < edx);
    edx = eax + 3; FAIL_IF(edx < eax);
    edx &= ~3u;
    eax = ebp * 4;
    O(0x14) = edx;
    r8 = eax + edx; FAIL_IF(r8 < edx);
    edx = r8 + eax; O(0x18) = r8; FAIL_IF(edx < r8);
    r8 = edx + 7; O(0x44) = edx; FAIL_IF(r8 < edx);
    r8 &= ~7u; O(0x44) = r8;
    r8 = edx + eax; O(0x1c) = edx; FAIL_IF(r8 < edx);
    edx = r8 + 7; O(0x44) = r8; FAIL_IF(edx < r8);
    edx &= ~7u; O(0x44) = edx;
    edx = r8 + eax; O(0x20) = r8; FAIL_IF(edx < r8);
    r8 = edx + eax; O(0x24) = edx; FAIL_IF(r8 < edx);
    edx = r8 + eax; O(0x28) = r8; FAIL_IF(edx < r8);
    eax = edx + ebp; O(0x38) = edx; FAIL_IF(eax < edx);
    r11c = eax + 3; FAIL_IF(r11c < eax);
    r11c &= ~3u;
    edi16 = di;
    O(0x40) = r11c;
    r8 = r11c + edi16 * 3 * 4; FAIL_IF(r8 < r11c);
    /* the glyph zone needs at least points * 0x20 bytes past offset 0x44 */
    eax = (uint32_t)AT(mp, 6, uint16_t) << 5;
    uint32_t w44 = O(0x44);
    edx = r8 - w44;
    if (edx < eax) r8 = r8 - edx + eax;
    uint32_t r11d = w44 + ecx; FAIL_IF(r11d < w44);
    edx = ecx + r8; O(0x44) = r11d; FAIL_IF(edx < ecx);
    O(0xc) = edx;
    /* stack: max(component elements, 3) + depth (1..20) + 1 entries of 0xf8 bytes */
    uint16_t si;
    uint16_t depth = AT(mp, 0x1e, uint16_t);
    if (depth < 1) si = 1;
    else si = depth > 0x14 ? 0x14 : depth;
    ecx = (uint32_t)si + 1;
    ax = 3;
    if (AT(mp, 0x1c, uint16_t) >= 3) ax = AT(mp, 0x1c, uint16_t);
    r8 = (uint32_t)ax + ecx; FAIL_IF(r8 < ecx);
    ecx = edx + r8; FAIL_IF(ecx < edx);
    eax = ecx + 7; FAIL_IF(eax < ecx);
    eax &= ~7u;
    edx = r8 * 0xf8;
    O(0x10) = eax;
    edx += eax; FAIL_IF(edx < eax);
    ecx = edx + 3; FAIL_IF(ecx < edx);
    ecx &= ~3u;
    O(0) = ecx;
    uv = ecx + (uint32_t)AT(mp, 0x18, uint16_t) * 4; FAIL_IF(uv < ecx);
    FAIL_IF(uv > 0x7fffffffu);
    FAIL_IF(r11d > 0x7fffffffu);
    int64_t diff = (int64_t)(int32_t)uv - (int64_t)(int32_t)r11d;
    FAIL_IF((uint64_t)(diff + 0x80000000ll) > 0xffffffffu);
    *out = (int32_t)diff;
    O(0x48) = 0;
    O(0x4c) = 0;
    return uv;
}
PRESERVING_EXPORT(tt_fsg_WorkSpaceSetOffsets, scaler_init_WorkSpaceSetOffsets);

/* fs_NewSfnt(input, info): binds the client record to the input's callbacks, maps the table
 * directory, loads maxp/head metrics, sizes the private font space (info + 0x10) and the
 * work space (info + 0xc), and selects the cmap. */
TT_EXPORT int32_t tt_fs_NewSfnt(uint8_t *in, uint8_t *info) {
    uint8_t *c = AT(in, 8, uint8_t *);
    if (!c) return 0x1003;
    AT(c, 0x118, void *) = in + 8;
    AT(c, 0x120, void *) = info;
    if (!info) return 0x1003;
    if (!(AT(c, 8, void *) = AT(in, 0x50, void *))) return 0x1008;          /* GetSfntFragment */
    if (!(AT(c, 0x10, void *) = AT(in, 0x58, void *)))                      /* ReleaseSfntFrag */
        AT(c, 0x10, void *) = (uint8_t *)idwr_orig(RVA_nop_release);
    if (!(AT(c, 0xf8, void *) = AT(in, 0x60, void *))) return 0x100d;       /* variation callbacks */
    if (!(AT(c, 0x100, void *) = AT(in, 0x68, void *))) return 0x100e;
    if (!(AT(c, 0x108, void *) = AT(in, 0x70, void *))) return 0x100f;
    AT(c, 0, void *) = AT(in, 0x78, void *);                                /* client id */
    int32_t e = ORIG(int32_t (*)(uint8_t *), RVA_sfac_DoOffsetTableMap)(c);
    if (e) return e;
    e = ORIG(int32_t (*)(uint8_t *, uint8_t *, uint8_t *, uint8_t *), RVA_sfac_LoadCriticalSfntMetrics)(
        c, c + 0x174, c + 0x19e, c + 0x1b0);
    if (e) return e;
    AT(c, 0x110, uint16_t) = AT(in, 0x94, uint16_t);                        /* variation axis count */
    uint32_t s = ORIG(uint32_t (*)(uint8_t *, uint8_t *, uint8_t *), RVA_fsg_PrivateFontSpaceSize)(
        c, c + 0x1b0, c + 0x258);
    uint64_t s4 = (uint64_t)s + 4;                                          /* + the 'sfnt' end tag */
    if (s4 > 0xffffffffu || (uint32_t)s4 > 0x7fffffffu) { safeint_overflow(); return 0; }
    AT(info, 0x10, uint32_t) = (uint32_t)s4;
    s = ORIG(uint32_t (*)(uint8_t *, uint8_t *, uint8_t *), RVA_fsg_WorkSpaceSetOffsets)(
        c + 0x1b0, c + 0x208, c + 0x3b4);
    s4 = (uint64_t)s + 4;
    if (s4 > 0xffffffffu || (uint32_t)s4 > 0x7fffffffu) { safeint_overflow(); return 0; }
    AT(info, 0xc, uint32_t) = (uint32_t)s4;
    e = ORIG(int32_t (*)(uint8_t *, uint16_t, uint16_t), RVA_sfac_ComputeMapping)(
        c, AT(in, 0x90, uint16_t), AT(in, 0x92, uint16_t));
    if (e) return e;
    AT(c, 0x1d0, uint32_t) = 2;     /* state: font opened */
    AT(c, 0x1d5, uint8_t) = 1;      /* font program still to run */
    return 0;
}

/* ======================================================================================
 * fs__NewTransformation and its helpers
 * ====================================================================================== */

/* fsg_QueryGlobalGSSubPixel(private space, unused, offsets): the sub-pixel copy of the
 * global graphics state. The SafeInt<int64> * 1 of the original is the identity here (the
 * offset is a zero-extended uint32). */
uint8_t *scaler_init_QueryGlobalGSSubPixel(uint8_t *base, uint64_t unused, const uint8_t *o) {
    (void)unused;
    return ptr_add((uintptr_t)base, AT(o, 0x64, uint32_t));
}
PRESERVING_EXPORT(tt_fsg_QueryGlobalGSSubPixel, scaler_init_QueryGlobalGSSubPixel);

/* fsg_QueryTwilightElementSubPixel: the sub-pixel twilight zone struct, with its arrays
 * bound (unlike the main twilight zone, +0x58 is not set here). */
uint8_t *scaler_init_QueryTwilightElementSubPixel(uint8_t *base, uint64_t unused, const uint8_t *o) {
    (void)unused;
    uint8_t *arrays = ptr_add((uintptr_t)base, AT(o, 0x6c, uint32_t));
    uint8_t *e = ptr_add((uintptr_t)base, AT(o, 0x68, uint32_t));
    for (int i = 0; i < 6; i++) AT(e, 8 * i, uint8_t *) = arrays + AT(o, 0x24 + 4 * i, uint32_t);
    AT(e, 0x38, uint8_t *) = arrays + AT(o, 0x40, uint32_t);
    AT(e, 0x40, uint8_t *) = arrays + AT(o, 0x44, uint32_t);
    AT(e, 0x30, uint8_t *) = arrays + AT(o, 0x3c, uint32_t);
    AT(e, 0x48, uint8_t *) = arrays + AT(o, 0x48, uint32_t);
    AT(e, 0x68, uint8_t *) = arrays + AT(o, 0x50, uint32_t);
    return e;
}
PRESERVING_EXPORT(tt_fsg_QueryTwilightElementSubPixel, scaler_init_QueryTwilightElementSubPixel);

/* fsg_CopyFontProgramResults(G, G sub-pixel): what the font program defined (FDEFs, IDEFs,
 * the IDEF opcode list) carries over to the sub-pixel state. */
void scaler_init_CopyFontProgramResults(const uint8_t *G, uint8_t *G2) {
    for (int i = 0; i < (int)AT(AT(G, 0x178, uint8_t *), 0x14, uint16_t); i++)
        AT(AT(G2, 0xb8, uint8_t *), 8 * i, uint64_t) = AT(AT(G, 0xb8, uint8_t *), 8 * i, uint64_t);
    AT(G2, 0x16c, int32_t) = AT(G, 0x16c, int32_t);
    for (int i = 0; i < AT(G, 0x16c, int32_t); i++)
        AT(AT(G2, 0xc0, uint8_t *), 8 * i, uint64_t) = AT(AT(G, 0xc0, uint8_t *), 8 * i, uint64_t);
    AT(G2, 0x1c2, uint16_t) = AT(G, 0x1c2, uint16_t);
    AT(G2, 0x1c4, uint16_t) = AT(G, 0x1c4, uint16_t);
    for (int i = 0; i < (int)AT(G2, 0x1c4, uint16_t); i++)
        AT(G2, 0x1c6 + 2 * i, uint16_t) = AT(G, 0x1c6 + 2 * i, uint16_t);
}
PRESERVING_EXPORT(tt_fsg_CopyFontProgramResults, scaler_init_CopyFontProgramResults);

#ifndef IDWR_STANDALONE
typedef int32_t (*scl_InitializeScaling_fn)(uint8_t *G, uint8_t, uint8_t, uint8_t *matrix, uint8_t, uint16_t,
                                            int32_t, int16_t, int16_t, uint16_t, uint16_t, int16_t, int32_t,
                                            uint8_t, uint8_t, uint8_t, int16_t *, int16_t *, uint8_t,
                                            uint32_t *);
#else
/* Standalone: the port's own integer widths (scaler_scl.c, which narrows each one). Stack
 * arguments must have the callee's types: Apple arm64 packs them by their declared size. */
typedef int32_t (*scl_InitializeScaling_fn)(uint8_t *G, uint32_t, uint32_t, uint8_t *matrix, uint32_t, uint32_t,
                                            int32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int32_t,
                                            uint32_t, uint32_t, uint32_t, int16_t *, int16_t *, uint32_t,
                                            uint32_t *);
#endif

/* fsg_InitInterpreterTrans body (fs__NewTransformation inlines the same code for the main
 * state with t = client + 0x174). t: +0 u16, +4 TransMatrix, +0x28 Max45 flag,
 * +0x29 positive square, +0x2a, +0x30 flags, +0x34 stretch. */
static int32_t init_interpreter_trans(uint8_t *t, uint8_t *G, uint8_t b3, uint8_t b4, int32_t a5, int16_t a6,
                                      int16_t a7, uint8_t a8, uint16_t a9, uint16_t a10, int16_t a11, int32_t a12,
                                      uint8_t a13, uint8_t a14, uint8_t a15, uint16_t naxes, const int16_t *coords,
                                      int16_t *p18, int16_t *p19) {
    uint8_t *m = t + 4;
    uint32_t flags_out = 0;
    int32_t e = ORIG(scl_InitializeScaling_fn, RVA_scl_InitializeScaling)(
        G, b3, AT(t, 0x2a, uint8_t), m, b4, AT(t, 0, uint16_t), a5, a6, a7, a9, a10, a11, a12, a13, a14, a15,
        p18, p19, a8, &flags_out);
    if (e) return e;
    uint32_t f = 0xff;
    AT(t, 0x28, uint8_t) = 0;
    if (flags_out > 0xff) AT(t, 0x30, uint32_t) = 0xff;
    else { AT(t, 0x30, uint32_t) = flags_out; f = flags_out; }
    uint8_t sq = CALL_PositiveSquare(m);
    AT(t, 0x29, uint8_t) = sq;
    int32_t ma = AT(m, 0, int32_t), mb = AT(m, 4, int32_t), mc = AT(m, 0xc, int32_t), md = AT(m, 0x10, int32_t);
    if (mb != 0 || mc != 0 || ma < 0 || md < 0) { f |= 0x2000; AT(t, 0x30, uint32_t) = f; }
    if (!sq) {
        if ((ma != 0 || md != 0) && (mc != 0 || mb != 0)) f |= 0x400;
        f |= 0x1000;
        AT(t, 0x30, uint32_t) = f;
        uint8_t r = CALL_Max45Trick(ma, mb);
        r |= CALL_Max45Trick(mc, md);
        AT(t, 0x28, uint8_t) = r;
    }
    AT(t, 0x34, uint8_t) = a9 != 0 || a10 != 0;
    /* any non-default variation coordinate */
    AT(G, 0x1d8, uint32_t) = naxes;
    AT(G, 0x1d5, uint8_t) = 0;
    for (int i = 0; i < (int)naxes; i++)
        if (coords[i] != 0) { AT(G, 0x1d5, uint8_t) = 1; break; }
    return 0;
}

/* Narrow parameters arrive as 64-bit slots with undefined upper bits (MSVC does not extend
 * them), so the exports take uint64_t and truncate. */
TT_EXPORT int32_t tt_fsg_InitInterpreterTrans(uint8_t *t, uint8_t *G, uint64_t b3, uint64_t b4, uint64_t a5,
                                              uint64_t a6, uint64_t a7, uint64_t a8, uint64_t a9, uint64_t a10,
                                              uint64_t a11, uint64_t a12, uint64_t a13, uint64_t a14, uint64_t a15,
                                              uint64_t a16, const int16_t *coords, int16_t *p18, int16_t *p19) {
    return init_interpreter_trans(t, G, (uint8_t)b3, (uint8_t)b4, (int32_t)a5, (int16_t)a6, (int16_t)a7, (uint8_t)a8,
                                  (uint16_t)a9, (uint16_t)a10, (int16_t)a11, (int32_t)a12, (uint8_t)a13,
                                  (uint8_t)a14, (uint8_t)a15, (uint16_t)a16, coords, p18, p19);
}

typedef int32_t (*itrp_Execute_fn)(uint8_t *twilight, uint8_t *glyph, const uint8_t *p, const uint8_t *end,
                                   uint8_t *G, void *trace);

/* fsg_RunPreProgram(client, maxp, t, G, work space, twilight, trace): resets the graphics
 * state defaults, copies and scales the CVT, clears the twilight zone, runs prep. Unless
 * prep set "instruction control: keep the graphics state" (G + 0x88 bit 1), the defaults
 * are put back afterwards. Also called from fs__Contour. */
TT_EXPORT int32_t tt_fsg_RunPreProgram(uint8_t *c, const uint8_t *mp, const uint8_t *t, uint8_t *G, uint8_t *ws,
                                       uint8_t *tw, void *trace) {
    AT(G, 0x1a0, int32_t) = 10000000;                                       /* instruction budget */
    int32_t t2c = AT(t, 0x2c, int32_t);
    AT(G, 0x50, void *) = (uint8_t *)idwr_orig(RVA_itrp_RoundToGrid);         /* round state RTG */
    AT(G, 0x58, uint16_t) = 3;
    AT(G, 0x4c, uint32_t) = 0x40;                                           /* minimum distance 1 px */
    AT(G, 0x38, uint64_t) = 0x44;                                           /* control value cut-in 17/16 */
    AT(G, 0x6c, uint32_t) = 0x80;
    AT(G, 0x70, uint8_t) = 1;
    AT(G, 0x68, uint32_t) = 0x30009;
    int32_t v = (int32_t)(0x16c0au - (uint32_t)t2c) >> 10;
    AT(G, 0x44, uint32_t) = AT(G, 0x1bf, uint8_t) ? 0x120 : 0;
    AT(G, 0x48, uint32_t) = 0;
    AT(G, 0x28, int32_t) = v;
    AT(G, 0x2c, int32_t) = (int32_t)(0u - (uint32_t)v);
    AT(G, 0x30, uint32_t) = 0;
    AT(G, 0x24, uint32_t) = 0;
    int32_t *cvt = AT(G, 0x18, int32_t *);
    int32_t e = ORIG(int32_t (*)(uint8_t *, int32_t *), RVA_sfac_CopyCVT)(c, cvt);
    if (e) return e;
    uint16_t ncvt = AT(G, 0x180, uint16_t);
    if (ncvt)
        ORIG(void (*)(uint8_t *, void *, int32_t *, int32_t *, uint32_t), RVA_scl_Scale)(
            G + 0x150, AT(G, 0xe8, void *), cvt, AT(G, 0x18, int32_t *), ncvt);
    /* clear the twilight zone */
    uint32_t n = AT(mp, 0x10, uint16_t);
    size_t bytes = (size_t)n * 4;
    memset(AT(tw, 0x00, void *), 0, bytes);
    memset(AT(tw, 0x10, void *), 0, bytes);
    memset(AT(tw, 0x20, void *), 0, bytes);
    memset(AT(tw, 0x08, void *), 0, bytes);
    memset(AT(tw, 0x18, void *), 0, bytes);
    memset(AT(tw, 0x28, void *), 0, bytes);
    memset(AT(tw, 0x30, void *), 0, n);
    memset(AT(tw, 0x48, void *), 0, n);
    *AT(tw, 0x38, int16_t *) = 0;
    *AT(tw, 0x40, int16_t *) = 0;
    uint16_t np = AT(mp, 0x10, uint16_t);
    if (np > 0x7fff) { safeint_overflow(); return 0; }
    *AT(tw, 0x38, int16_t *) = 0;                                           /* one contour */
    *AT(tw, 0x40, int16_t *) = (int16_t)(np - 1);
    AT(tw, 0x50, uint16_t) = 1;
    uint8_t *glyph = AT(ws, 0x10, uint8_t *);
    AT(G, 0x16a, uint16_t) = 1;                                             /* program kind: prep */
    memcpy(G + 0x78, G + 0x38, 0x40);                                       /* save the defaults */
    int32_t r = 0;
    const uint8_t *prep = AT(G, 0xf0, const uint8_t *);
    if (prep)
        r = ORIG(itrp_Execute_fn, RVA_itrp_Execute)(tw, glyph, prep, prep + AT(G, 0xf8, uint32_t), G, trace);
    if (!(AT(G, 0x88, uint8_t) & 2)) memcpy(G + 0x38, G + 0x78, 0x40);
    return r;
}

/* fs__NewTransformation(input, info, mode): sets up the scaler for a new matrix and size.
 * With the client in state 6 (bit 2) and the same transformation inputs it only refreshes
 * two matrix entries. Otherwise it binds the work space and private font space, builds the
 * global graphics state (and its ClearType sub-pixel copy when input + 0xd0 asks for it),
 * runs the font program once per font and the pre-program per transformation. */
TT_EXPORT int32_t tt_fs__NewTransformation(uint8_t *in, uint8_t *info, uint64_t mode_) {
    uint8_t mode = (uint8_t)mode_;
    uint8_t *gsub = NULL, *twsub = NULL;
    if (!AT(in, 8, void *) || !AT(in, 0x20, void *) || !AT(in, 0x28, void *)) return 0x1003;
    /* 'sfnt' tag at the end of memory bases 3 and 4 */
    for (int i = 0; i < 2; i++)
        AT(AT(in, 0x20 + 8 * i, uint8_t *), (int64_t)AT(info, 0xc + 4 * i, int32_t) - 4, uint32_t) = 0x73666e74;
    uint8_t *c = AT(in, 8, uint8_t *);
    if (!c) return 0x1001;
    AT(c, 0x118, void *) = in + 8;
    AT(c, 0x120, void *) = info;
    if (!info) return 0x1003;
    if (!(AT(c, 8, void *) = AT(in, 0x50, void *))) return 0x1008;
    if (!(AT(c, 0x10, void *) = AT(in, 0x58, void *))) AT(c, 0x10, void *) = (uint8_t *)idwr_orig(RVA_nop_release);
    if (!(AT(c, 0xf8, void *) = AT(in, 0x60, void *))) return 0x100d;
    if (!(AT(c, 0x100, void *) = AT(in, 0x68, void *))) return 0x100e;
    if (!(AT(c, 0x108, void *) = AT(in, 0x70, void *))) return 0x100f;
    uint32_t state = AT(c, 0x1d0, uint32_t);
    if (!(state & 2)) return 0x1005;
    AT(c, 0, void *) = AT(in, 0x78, void *);
    if (state & 4) {
        /* same transformation as the last one: refresh +0x180/+0x18c of the matrix */
        if (AT(c, 0x458, int32_t) == AT(in, 0x90, int32_t) && AT(c, 0x45c, int16_t) == AT(in, 0x94, int16_t) &&
            AT(c, 0x45e, int16_t) == AT(in, 0x96, int16_t) && AT(c, 0x464, int32_t) == AT(in, 0x9c, int32_t) &&
            AT(c, 0x474, int32_t) == AT(in, 0xac, int32_t) && AT(c, 0x470, int32_t) == AT(in, 0xa8, int32_t) &&
            AT(c, 0x468, int32_t) == AT(in, 0xa0, int32_t) && AT(c, 0x47c, int32_t) == AT(in, 0xb4, int32_t) &&
            AT(c, 0x480, int32_t) == AT(in, 0xb8, int32_t) && AT(c, 0x484, int32_t) == AT(in, 0xbc, int32_t) &&
            AT(c, 0x488, int16_t) == AT(in, 0xc0, int16_t) && AT(c, 0x49e, uint8_t) == AT(in, 0xd6, uint8_t) &&
            AT(c, 0x498, int16_t) == AT(in, 0xd0, int16_t) && AT(c, 0x49a, int16_t) == AT(in, 0xd2, int16_t) &&
            AT(c, 0x49c, int16_t) == AT(in, 0xd4, int16_t) && AT(c, 0x48a, int16_t) == AT(in, 0xc2, int16_t) &&
            AT(c, 0x48c, int16_t) == AT(in, 0xc4, int16_t) && AT(c, 0x490, int32_t) == AT(in, 0xc8, int32_t) &&
            AT(c, 0x494, uint8_t) == AT(in, 0xcc, uint8_t) && AT(c, 0x495, uint8_t) == AT(in, 0xcd, uint8_t) &&
            AT(c, 0x497, uint8_t) == AT(in, 0xcf, uint8_t) && AT(c, 0x496, uint8_t) == AT(in, 0xce, uint8_t) &&
            AT(c, 0x1a0, int32_t) == AT(in, 0x98, int32_t) && AT(in, 0x80, void *) == NULL) {
            AT(c, 0x180, int32_t) = AT(in, 0xa4, int32_t);
            AT(c, 0x18c, int32_t) = AT(in, 0xb0, int32_t);
            return 0;
        }
        AT(c, 0x1d0, uint32_t) = state & ~4u;
    }
    AT(c, 0x414, uint8_t) = mode;

    /* work space (memory base 3): element pointers and the glyph zone arrays */
    uintptr_t b3 = (uintptr_t)AT(in, 0x20, uint8_t *);
    AT(c, 0x1d8, uintptr_t) = b3 + AT(c, 0x208, uint32_t);
    uintptr_t arrays = b3 + AT(c, 0x20c, uint32_t);
    AT(c, 0x1e0, uintptr_t) = arrays;
    uint8_t *ge = (uint8_t *)(b3 + AT(c, 0x210, uint32_t));
    AT(c, 0x1e8, uint8_t *) = ge;
    AT(c, 0x1f0, uintptr_t) = b3 + AT(c, 0x214, uint32_t);
    AT(c, 0x1f8, uintptr_t) = b3 + AT(c, 0x218, uint32_t);
    AT(c, 0x200, uintptr_t) = b3 + AT(c, 0x24c, uint32_t);
    for (int i = 0; i < 6; i++) AT(ge, 8 * i, uintptr_t) = arrays + AT(c, 0x21c + 4 * i, uint32_t);
    AT(ge, 0x38, uintptr_t) = arrays + AT(c, 0x238, uint32_t);
    AT(ge, 0x40, uintptr_t) = arrays + AT(c, 0x23c, uint32_t);
    AT(ge, 0x30, uintptr_t) = arrays + AT(c, 0x234, uint32_t);
    AT(ge, 0x48, uintptr_t) = arrays + AT(c, 0x240, uint32_t);
    AT(ge, 0x58, uintptr_t) = arrays + AT(c, 0x244, uint32_t);
    AT(ge, 0x68, uintptr_t) = arrays + AT(c, 0x248, uint32_t);

    /* private font space (memory base 4): the global graphics state and its sub-pixel copy */
    uint8_t *elements = AT(c, 0x1d8, uint8_t *);
    uintptr_t b4 = (uintptr_t)AT(AT(c, 0x118, uint8_t *), 0x20, uint8_t *);
    uint8_t *cvt = ptr_add(b4, AT(c, 0x264, uint32_t));
    uint8_t *storage = ptr_add(b4, AT(c, 0x258, uint32_t));
    uint8_t *fdefs = ptr_add(b4, AT(c, 0x25c, uint32_t));
    uint8_t *idefs = ptr_add(b4, AT(c, 0x260, uint32_t));
    uint8_t *axes = ptr_add(b4, AT(c, 0x2c8, uint32_t));
    uint8_t *G = ptr_add(b4, AT(c, 0x268, uint32_t));
    uint8_t *fpgm = ptr_add(b4, AT(c, 0x26c, uint32_t));
    uint8_t *prep = ptr_add(b4, AT(c, 0x270, uint32_t));
    uint32_t fpgm_len = AT(c, 0x6c, uint32_t);
    uint32_t cvt_bytes = AT(c, 0x44, uint32_t);
    uint32_t prep_len = AT(c, 0x4c, uint32_t);
    uint32_t ncvt = 0;
    if (cvt_bytes) {
        ncvt = cvt_bytes >> 1;
        if (ncvt > 0xffff) { safeint_overflow(); return 0; }
    }
    uint8_t *mp = c + 0x1b0;
    for (int copy = 0; copy < 2; copy++) {
        if (copy == 1) {
            cvt = ptr_add(b4, AT(c, 0x2b8, uint32_t));
            storage = ptr_add(b4, AT(c, 0x2ac, uint32_t));
            fdefs = ptr_add(b4, AT(c, 0x2b0, uint32_t));
            idefs = ptr_add(b4, AT(c, 0x2b4, uint32_t));
            G = ptr_add(b4, AT(c, 0x2bc, uint32_t));
        }
        AT(G, 0xc0, uint8_t *) = idefs;
        AT(G, 0x1a8, void *) = AT(c, 0, void *);
        AT(G, 0, uint8_t *) = elements;
        AT(G, 0xb8, uint8_t *) = fdefs;
        AT(G, 0x1e0, uint8_t *) = axes;
        AT(G, 0x18, uint8_t *) = cvt;
        AT(G, 0x10, uint8_t *) = storage;
        AT(G, 8, uint16_t) = AT(mp, 0x18, uint16_t);                        /* stack size */
        uint16_t pts = AT(mp, 0xa, uint16_t);
        if (AT(mp, 6, uint16_t) >= pts) pts = AT(mp, 6, uint16_t);
        AT(G, 0x1b0, uint32_t) = (uint32_t)pts + 4;
        AT(G, 0x108, uint32_t) = fpgm_len ? fpgm_len : 0;
        AT(G, 0x100, uint8_t *) = fpgm_len ? fpgm : NULL;
        AT(G, 0xf8, uint32_t) = prep_len ? prep_len : 0;
        AT(G, 0xf0, uint8_t *) = prep_len ? prep : NULL;
        AT(G, 0x171, uint8_t) = 0;
        AT(G, 0x178, uint8_t *) = mp;
        AT(G, 0x180, uint16_t) = (uint16_t)ncvt;
        AT(G, 0x195, uint8_t) = 1;                                          /* word store at 0x195 */
        AT(G, 0x196, uint8_t) = 0;
        AT(G, 0x198, int32_t) = 10000;                                      /* jump budget */
        AT(G, 0x19c, int32_t) = 100;
        AT(G, 0x1a0, int32_t) = 10000000;                                   /* instruction budget */
        memset(storage, 0, (size_t)AT(mp, 0x12, uint16_t) << 2);
    }

    /* the twilight zone struct and its arrays */
    uintptr_t b4b = (uintptr_t)AT(AT(c, 0x118, uint8_t *), 0x20, uint8_t *);
    G = ptr_add(b4b, AT(c, 0x268, uint32_t));
    uint8_t *tarr = ptr_add(b4b, AT(c, 0x278, uint32_t));
    uint8_t *tw = ptr_add(b4b, AT(c, 0x274, uint32_t));
    for (int i = 0; i < 6; i++) AT(tw, 8 * i, uint8_t *) = tarr + AT(c, 0x27c + 4 * i, uint32_t);
    AT(tw, 0x38, uint8_t *) = tarr + AT(c, 0x298, uint32_t);
    AT(tw, 0x40, uint8_t *) = tarr + AT(c, 0x29c, uint32_t);
    AT(tw, 0x30, uint8_t *) = tarr + AT(c, 0x294, uint32_t);
    AT(tw, 0x48, uint8_t *) = tarr + AT(c, 0x2a0, uint32_t);
    AT(tw, 0x58, uint8_t *) = tarr + AT(c, 0x2a4, uint32_t);
    AT(tw, 0x68, uint8_t *) = tarr + AT(c, 0x2a8, uint32_t);
    int16_t *coords = (int16_t *)ptr_add((uintptr_t)AT(AT(c, 0x118, uint8_t *), 0x20, uint8_t *),
                                         AT(c, 0x2c8, uint32_t));
    /* GetVariationAxisCoordinates(client id, coords, axis count) */
    ((void (*)(void *, int16_t *, uint16_t))AT(c, 0x108, void *))(AT(c, 0, void *), coords, AT(c, 0x110, uint16_t));
    memcpy(c + 0x128, AT(c, 0x118, uint8_t *), 0x48);                      /* the 9 memory bases */
    AT(c, 0x1d4, uint8_t) = AT(in, 0x80, void *) == NULL;                   /* prep may be skipped later */
    if (AT(c, 0x414, uint8_t) == 0) {                                       /* mode 0: run nothing now */
        AT(c, 0x1d4, uint8_t) = 1;
        AT(c, 0x1d5, uint8_t) = 0;
    }
    if (AT(c, 0x1d5, uint8_t)) {
        int32_t e = ORIG(int32_t (*)(uint8_t *, uint8_t *, uint32_t, uint8_t *, uint32_t), RVA_sfac_CopyFontAndPrePrograms)(
            c, fpgm, fpgm_len, prep, prep_len);
        if (e) return e;
    }

    /* the transformation inputs, kept for the next call's comparison */
    memcpy(c + 0x178, in + 0x9c, 0x20);
    AT(c, 0x198, int32_t) = AT(in, 0xbc, int32_t);
    AT(c, 0x1a0, int32_t) = AT(in, 0x98, int32_t);
    AT(c, 0x458, int32_t) = AT(in, 0x90, int32_t);
    AT(c, 0x45c, uint16_t) = AT(in, 0x94, uint16_t);
    AT(c, 0x45e, uint16_t) = AT(in, 0x96, uint16_t);
    memcpy(c + 0x464, in + 0x9c, 0x20);
    AT(c, 0x484, int32_t) = AT(in, 0xbc, int32_t);
    AT(c, 0x488, uint16_t) = AT(in, 0xc0, uint16_t);
    AT(c, 0x48a, uint16_t) = AT(in, 0xc2, uint16_t);
    AT(c, 0x48c, uint16_t) = AT(in, 0xc4, uint16_t);
    AT(c, 0x490, int32_t) = AT(in, 0xc8, int32_t);
    AT(c, 0x494, uint8_t) = AT(in, 0xcc, uint8_t);
    AT(c, 0x495, uint8_t) = AT(in, 0xcd, uint8_t);
    AT(c, 0x496, uint8_t) = AT(in, 0xce, uint8_t);
    AT(c, 0x497, uint8_t) = AT(in, 0xcf, uint8_t);
    AT(c, 0x498, uint16_t) = AT(in, 0xd0, uint16_t);
    AT(c, 0x49e, uint8_t) = AT(in, 0xd6, uint8_t);
    int16_t in94 = AT(in, 0x94, int16_t);

    /* scan/ClearType flags (input + 0xd0) -> client + 0x41a */
    uint8_t fl = AT(in, 0xd0, uint8_t);
    uint16_t f = 0;
    if (fl & 1) f = 1;
    if ((fl & 2) && AT(c, 0x17c, int32_t) == 0) f |= 2;        /* sub-pixel copy only without skew */
    if (fl & 4) f |= 4;
    if ((fl & 0x21) == 1 && AT(c, 0x178, int32_t) == 0) f ^= 4;
    if (fl & 8) f |= 8;
    if (fl & 0x10) f |= 0x10;
    if (fl & 0x20) f |= 0x20;
    if (fl & 0x80) f |= 0x80;
    if (fl & 0x40) f |= 0x40;
    AT(c, 0x41a, uint16_t) = f;
    AT(c, 0x49a, uint16_t) = AT(in, 0xd2, uint16_t);
    AT(c, 0x49c, uint16_t) = AT(in, 0xd4, uint16_t);
    uint16_t gs_flags = 0;
    if (f & 1) {
        if (AT(c, 0x488, uint16_t) != 0) return 0x1701;
    } else if (f & 0xbe) {
        return 0x1701;
    }
    if (f & 1) {
        if (f & 2) {
            uint8_t *pfs = AT(AT(c, 0x118, uint8_t *), 0x20, uint8_t *);
            gsub = ORIG(uint8_t *(*)(uint8_t *, uint32_t, uint8_t *), RVA_fsg_QueryGlobalGSSubPixel)(pfs, 0, c + 0x258);
            pfs = AT(AT(c, 0x118, uint8_t *), 0x20, uint8_t *);
            twsub = ORIG(uint8_t *(*)(uint8_t *, uint32_t, uint8_t *), RVA_fsg_QueryTwilightElementSubPixel)(pfs, 0, c + 0x258);
            memcpy(c + 0x41c, c + 0x174, 0x38);                             /* sub-pixel transform record */
        } else {
            gs_flags = f;
        }
    }
    uint16_t os = AT(c, 0x488, uint16_t);
    if (os) {
        if (!((1u << ((os - 1) & 31)) & 0x8b)) return 0x1701;               /* 1, 2, 4 or 8 */
        if (os > 0x1f) return 0x1701;
    }
    if (AT(in, 0x8c, int32_t) == 2) os = 0;
    AT(G, 0x1c0, uint16_t) = gs_flags;
    AT(G, 0x194, uint8_t) = os != 0;
    int32_t e = init_interpreter_trans(c + 0x174, G, AT(in, 0x88, uint8_t), AT(in, 0xce, uint8_t), AT(in, 0x90, int32_t),
                                       in94, AT(in, 0x96, int16_t), AT(in, 0xd6, uint8_t), AT(in, 0xc2, uint16_t),
                                       AT(in, 0xc4, uint16_t), AT(c, 0xf4, int16_t), AT(in, 0xc8, int32_t),
                                       AT(in, 0xcc, uint8_t), AT(in, 0xcd, uint8_t), AT(in, 0xcf, uint8_t),
                                       AT(c, 0x110, uint16_t), coords, (int16_t *)(c + 0x416), (int16_t *)(c + 0x418));
    if (e) return e;
    if (AT(c, 0x41a, uint16_t) & 2) {
        uint16_t f2 = AT(c, 0x41a, uint16_t);
        AT(gsub, 0x194, uint8_t) = CALL_IsGrayScale(in, c);
        AT(gsub, 0x1c0, uint16_t) = f2;
#ifndef IDWR_STANDALONE
        typedef int32_t (*init_trans_fn)(uint8_t *, uint8_t *, uint8_t, uint8_t, int32_t, int16_t, int16_t, uint8_t,
                                         uint16_t, uint16_t, int16_t, int32_t, uint8_t, uint8_t, uint8_t, uint16_t,
                                         const int16_t *, int16_t *, int16_t *);
#else
        /* the export's own widths (it narrows each): Apple arm64 packs stack arguments by size */
        typedef int32_t (*init_trans_fn)(uint8_t *, uint8_t *, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                         uint64_t, uint64_t, const int16_t *, int16_t *, int16_t *);
#endif
        e = ORIG(init_trans_fn, RVA_fsg_InitInterpreterTrans)(
            c + 0x41c, gsub, AT(in, 0x88, uint8_t), AT(in, 0xce, uint8_t), AT(in, 0x90, int32_t), in94,
            AT(in, 0x96, int16_t), AT(in, 0xd6, uint8_t), AT(in, 0xc2, uint16_t), AT(in, 0xc4, uint16_t),
            AT(c, 0xf4, int16_t), AT(in, 0xc8, int32_t), AT(in, 0xcc, uint8_t), AT(in, 0xcd, uint8_t),
            AT(in, 0xcf, uint8_t), AT(c, 0x110, uint16_t), coords, (int16_t *)(c + 0x416), (int16_t *)(c + 0x418));
        if (e) return e;
    }

    /* font program, once per font (client + 0x1d5) */
    if (AT(c, 0x1d5, uint8_t)) {
        void *trace = AT(in, 0x80, void *);
        AT(G, 0x1a0, int32_t) = 10000000;
        memset(G + 0x24, 0, 0x10);
        AT(G, 0x1c2, uint32_t) = 0;                                         /* IDEF opcode count words */
        AT(G, 0x90, void *) = (uint8_t *)idwr_orig(RVA_itrp_RoundToGrid);
        AT(G, 0x16c, int32_t) = 0;                                          /* IDEF count */
        AT(G, 0x16a, uint16_t) = 0x101;                                     /* program kind: fpgm */
        AT(G, 0x98, uint16_t) = 3;
        for (int i = 0; i < (int)AT(AT(G, 0x178, uint8_t *), 0x14, uint16_t); i++)
            AT(AT(G, 0xb8, uint8_t *), 8 * i + 6, uint16_t) = 2;           /* FDEFs undefined */
        const uint8_t *p = AT(G, 0x100, const uint8_t *);
        if (p) {
            e = ORIG(itrp_Execute_fn, RVA_itrp_Execute)(tw, ge, p, p + AT(G, 0x108, uint32_t), G, trace);
            if (e) return e;
        }
        AT(c, 0x1d5, uint8_t) = 0;
    }
    if (AT(c, 0x41a, uint8_t) & 2)
        ORIG(void (*)(uint8_t *, uint8_t *), RVA_fsg_CopyFontProgramResults)(G, gsub);

    /* pre-program, per transformation */
    if (AT(c, 0x1d4, uint8_t) == 0) {
        typedef int32_t (*run_fn)(uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, uint8_t *, void *);
        e = ORIG(run_fn, RVA_fsg_RunPreProgram)(c, c + 0x1b0, c + 0x174, G, c + 0x1d8, tw, AT(in, 0x80, void *));
        if (!e && (AT(c, 0x41a, uint8_t) & 2))
            e = ORIG(run_fn, RVA_fsg_RunPreProgram)(c, c + 0x1b0, c + 0x41c, gsub, c + 0x1d8, twsub, AT(in, 0x80, void *));
        if (e) {
            AT(c, 0x414, uint8_t) = 0;
            return e;
        }
    }
    AT(info, 0xa0, uint8_t *) = ptr_add((uintptr_t)AT(AT(c, 0x118, uint8_t *), 0x20, uint8_t *), AT(c, 0x264, uint32_t));
    uint16_t rot = CALL_90deg(c + 0x178);
    int32_t s418 = AT(c, 0x418, int16_t), s416 = AT(c, 0x416, int16_t);
    AT(c, 0x3da, int16_t) = (int16_t)((int32_t)((uint32_t)AT(G, 0x184, int32_t) + 0x8000u) >> 16);
    AT(c, 0x40e, uint16_t) = AT(c, 0x174, uint16_t);
    AT(c, 0x3e2, uint16_t) = rot;
    AT(c, 0x3dc, int16_t) = (int16_t)((int32_t)((uint32_t)AT(G, 0x188, int32_t) + 0x8000u) >> 16);
    AT(c, 0x40c, uint8_t) = 0;
    AT(c, 0x3d8, uint16_t) = 0;
    int16_t a416 = (int16_t)(s416 < 0 ? -s416 : s416), a418 = (int16_t)(s418 < 0 ? -s418 : s418);
    if ((((uint16_t)(rot - 1)) & 0xfffd) == 0) {                            /* 90 or 270 degrees: swap */
        AT(c, 0x410, int16_t) = a418;
        AT(c, 0x412, int16_t) = a416;
    } else {
        AT(c, 0x410, int16_t) = a416;
        AT(c, 0x412, int16_t) = a418;
    }
    AT(c, 0x1d0, uint32_t) = 6;     /* state: transformation set */
    return 0;
}
#undef O
#undef FAIL_IF
