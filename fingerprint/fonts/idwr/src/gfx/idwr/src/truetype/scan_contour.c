/*
 * fs_ContourScan and two scan converter leaves, ported from DWrite.dll 10.0.26100.9711 (REA
 * assembly, authoritative; the Ghidra decompilation for orientation):
 *   fs_ContourScan  0x18006dd58  the glyph's bitmap: scan conversion (fsc_FillGlyph) of an
 *                                outline glyph, then the gray map, ClearType subpixel and bold
 *                                post-processing; or the embedded bitmap (sbit_GetBitmap)
 *   FindExtrema     0x18006d780  the outline's bounds in 26.6 and the pixel rect
 *   GetIxEstimate   0x18006d958  the signed sum of a reversal list
 *
 * Every callee goes through ORIG(rva) like the other ports. The strike branch and the bold
 * simulation of strikes call the sbit_* functions, which are not ported: in standalone mode
 * the driver never lets a glyph reach them (reimpl/oracle/fs_oracle.c).
 *
 * Structures (sfac_ClientRec `cl` = memory base 0, offsets from the original):
 *   cl + 0x118   &input->memoryBases[0]; + 0x18 base 3, + 0x28 base 5, + 0x30 base 6, + 0x38 base 7
 *   cl + 0x1d8   work space addresses, rebuilt from the offsets at cl + 0x208 when base 3 moved
 *   cl + 0x1e8   the glyph element: + 0x00 x, + 0x08 y, + 0x30 onCurve, + 0x38 sp, + 0x40 ep,
 *                + 0x50 contour count, + 0x58 flags
 *   cl + 0x2d0   the 1-bit bitmap (GlyphBitMap: rowBytes, rect top/left/bottom/right as
 *                + 2/+ 4/+ 6/+ 8 ... + 0x20 bits), cl + 0x2f8 the WorkScan,
 *   cl + 0x328   the overscaled bitmap (gray, ClearType), bits at + 0x348
 *   cl + 0x351   the glyph comes from a strike; cl + 0x3c0 the sbit state
 *   cl + 0x416/0x418 emboldening amounts, cl + 0x41a ClearType flags, cl + 0x488 gray
 *                overscale, cl + 0x49a subpixel overscale
 *   input + 0x88 GDI-compatible gray, + 0x89 bit order (1 = least significant bit first),
 *   input + 0x8a keep the overscaled ClearType bitmap, + 0x8c ClearType compatibility word
 */
#include <string.h>
#include "tt_state.h"
#include "orig.h"

#define RVA_SafeIntOnOverflow      0x1090b0
#define RVA_FillGlyph              0x06e980
#define RVA_UpdateWorkSpaceElement 0x0cc07c
#define RVA_ScalerOverScale        0x0d6084
#define RVA_CalcGrayMap            0x14fc50
#define RVA_CalcGrayMapGdi         0x14ff4c
#define RVA_OverscaleToSubPixel    0x150158
#define RVA_OverscaleToBold        0x0026e0
#define RVA_ReverseBitOrder        0x0d8160
#define RVA_ROWBYTESLONG           0x0ed964
#define RVA_sbit_GetBitmap         0x0051a0
#define RVA_sbit_ExpandSubpixelFromMono 0x0dad2c
#define RVA_sbit_EmboldenGray      0x10aca4
#define RVA_sbit_Embolden          0x14ef90
#define RVA_sbit_EmboldenGrayFromMono 0x14f4b8
#define RVA_sbit_EmboldenSubPixel  0x14f6dc
#define RVA_sbit_ExpandGrayFromMono 0x14fa90

static void overflow(void) { ORIG(void (*)(void), RVA_SafeIntOnOverflow)(); }

/* ---------------------------------------------------------------------------------------- */
/* Register-preserving exports for the two leaves: their original callers may keep values in
 * volatile registers across the call (whole-program optimisation), so the swapped-in port
 * saves RCX, RDX, R8-R11 and XMM0-XMM5 around the C body.
 *
 * Standalone mode (-DIDWR_STANDALONE): every caller is a C port, so there is no thunk; the
 * defines rename each body to its export, the plain C function on any ABI. */
#ifdef IDWR_STANDALONE
#define tt_FindExtrema_body   tt_FindExtrema
#define tt_GetIxEstimate_body tt_GetIxEstimate
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

PRESERVING_THUNK(tt_FindExtrema);
PRESERVING_THUNK(tt_GetIxEstimate);

/* FindExtrema 0x18006d780: bounds of the contours of `cl` (ContourList: + 0 count, + 8 sp,
 * + 0x10 ep, + 0x18 x, + 0x20 y) into the bitmap `bm`: the 26.6 extremes at + 0xc (xmin),
 * + 0x10 (xmax), + 0x14 (ymin), + 0x18 (ymax), the pixel rect at + 4 (left), + 8 (right),
 * + 6 (bottom), + 2 (top), and + 0xa = 1 when a zero-width or zero-height rect was widened.
 * Contours whose start and end point coincide are skipped. 0x1201 when the rect does not fit
 * in 16 bits or an edge is 0x7fff. */
__attribute__((used)) int tt_FindExtrema_body(const uint8_t *cl, uint8_t *bm) {
    uint16_t n = AT(cl, 0, uint16_t);
    const int16_t *sp = AT(cl, 8, const int16_t *), *ep = AT(cl, 0x10, const int16_t *);
    const int32_t *xs = AT(cl, 0x18, const int32_t *), *ys = AT(cl, 0x20, const int32_t *);
    int32_t xmax = 0, xmin = 0, ymax = 0, ymin = 0;
    int first = 1;
    for (uint16_t c = 0; c < n; c++) {
        int16_t s = sp[c], e = ep[c];
        if (s == e) continue;
        const int32_t *px = xs + s, *py = ys + s;
        if (first) {
            xmax = xmin = px[0];
            ymax = ymin = py[0];
        }
        first = 0;
        if (s > e) continue;
        for (uint16_t k = (uint16_t)(e - s + 1), i = 0; k; k--, i++) {
            int32_t x = px[i], y = py[i];
            if (x > xmax) xmax = x;
            if (x < xmin) xmin = x;
            if (y > ymax) ymax = y;
            if (y < ymin) ymin = y;
        }
    }
    AT(bm, 0xc, int32_t) = xmin;
    AT(bm, 0x14, int32_t) = ymin;
    AT(bm, 0x10, int32_t) = xmax;
    AT(bm, 0x18, int32_t) = ymax;
    int32_t l = (int32_t)((uint32_t)xmin + 0x1f) >> 6, b = (int32_t)((uint32_t)ymin + 0x1f) >> 6;
    int32_t r = (int32_t)((uint32_t)xmax + 0x20) >> 6, t = (int32_t)((uint32_t)ymax + 0x20) >> 6;
    if ((int16_t)l != l || (int16_t)b != b || (int16_t)r != r || (int16_t)t != t || t == 0x7fff || r == 0x7fff)
        return 0x1201;
    AT(bm, 0xa, uint8_t) = 0;
    if (!first) {
        if (l == r) { r++; AT(bm, 0xa, uint8_t) = 1; }
        if (b == t) { t++; AT(bm, 0xa, uint8_t) = 1; }
    }
    AT(bm, 4, int16_t) = (int16_t)l;
    AT(bm, 8, int16_t) = (int16_t)r;
    AT(bm, 6, int16_t) = (int16_t)b;
    AT(bm, 2, int16_t) = (int16_t)t;
    return 0;
}

/* GetIxEstimate 0x18006d958: sum over the reversal list (records: + 0 value, + 2 direction,
 * + 8 next; a value of 0x7fff or more ends it) of the value, negated for direction 1 */
__attribute__((used)) int32_t tt_GetIxEstimate_body(const uint8_t *r) {
    uint32_t sum = 0;
    while (AT(r, 0, int16_t) < 0x7fff) {
        int32_t v = AT(r, 0, int16_t);
        sum = AT(r, 2, int16_t) == 1 ? sum - (uint32_t)v : sum + (uint32_t)v;
        r = AT(r, 8, const uint8_t *);
    }
    return (int32_t)sum;
}

/* ---------------------------------------------------------------------------------------- */
typedef int (*fill_fn)(uint8_t *list, uint8_t *bm, uint8_t *ws, uint16_t scan_control, uint8_t reverse);
typedef uint16_t (*overscale_fn)(uint8_t *input, uint8_t *cl);
typedef int (*gray_fn)(uint8_t *src, uint8_t *dst, uint16_t overscale);
typedef void (*subpixel_fn)(uint8_t *src, uint8_t *dst);
typedef void (*bold_fn)(uint16_t strength, uint16_t overscale, uint8_t *src, uint8_t *dst);
typedef void (*reverse_fn)(uint32_t n, uint8_t *p);
typedef uint16_t (*rowbytes_fn)(int32_t bits);
typedef int (*getbitmap_fn)(uint8_t *sbit, uint8_t *cl, uint8_t *dst, uint8_t *extra);
typedef void (*expand_sub_fn)(uint8_t *src, uint8_t *dst, uint16_t w, uint16_t h, uint16_t src_rb, uint16_t dst_rb,
                              uint16_t overscale);
typedef void (*emb_gray_fn)(uint8_t *bits, uint16_t w, uint16_t h, uint16_t rb, uint16_t levels, uint16_t ex,
                            uint16_t ey);
typedef void (*emb_fn)(uint8_t *bits, uint16_t w, uint16_t h, uint16_t rb, uint16_t ex, uint16_t ey);
typedef void (*emb_gray_mono_fn)(uint8_t *bits, uint16_t w, uint16_t h);
typedef void (*expand_gray_fn)(uint8_t *src, uint8_t *dst, uint16_t w, uint16_t h, uint16_t overscale);

#define REVERSE(n, p) ORIG(reverse_fn, RVA_ReverseBitOrder)((n), (p))
#define OVERSCALE(in, cl) ORIG(overscale_fn, RVA_ScalerOverScale)((in), (cl))
#define ROWBYTES(b) ORIG(rowbytes_fn, RVA_ROWBYTESLONG)(b)
#define TO_BOLD(cl) ORIG(bold_fn, RVA_OverscaleToBold)(AT(cl, 0x3dc, uint16_t), AT(cl, 0x49a, uint16_t), (cl) + 0x328, (cl) + 0x2d0)
#define TO_SUBPIXEL(cl) ORIG(subpixel_fn, RVA_OverscaleToSubPixel)((cl) + 0x328, (cl) + 0x2d0)

/* rowBytes * (top - bottom) of a GlyphBitMap, with the SafeInt<int> checks */
static uint32_t bitmap_bytes(const uint8_t *bm) {
    int64_t h = (int64_t)AT(bm, 2, int16_t) - AT(bm, 6, int16_t);
    if ((uint64_t)(h + 0x80000000LL) > 0xffffffffULL) overflow();
    int64_t n = (int64_t)AT(bm, 0, int16_t) * (int64_t)(int32_t)h;
    if ((uint64_t)(n + 0x80000000LL) > 0xffffffffULL || (int32_t)n < 0) overflow();
    return (uint32_t)n;
}

/* bitmap width and height (right - left, top - bottom) as the 16-bit values passed on */
#define BM_W(bm) ((uint16_t)(AT(bm, 8, uint16_t) - AT(bm, 4, uint16_t)))
#define BM_H(bm) ((uint16_t)(AT(bm, 2, uint16_t) - AT(bm, 6, uint16_t)))

/* the overscaled ClearType bitmap built from the 1-bit one: each pixel repeated
 * cl + 0x49a times, the 1-bit rect scaled to match */
static void expand_subpixel(uint8_t *in, uint8_t *cl, uint16_t src_rb) {
    uint16_t ov = AT(cl, 0x49a, uint16_t);
    AT(cl, 0x2f0, uint8_t *) = AT(in, 0x30, uint8_t *);
    AT(cl, 0x348, uint8_t *) = AT(in, 0x48, uint8_t *);
    uint16_t w = BM_W(cl + 0x2d0);
    uint16_t dst_rb = ROWBYTES((int32_t)((uint32_t)w * ov));
    if (src_rb == 0) src_rb = ROWBYTES((int32_t)((uint32_t)w * AT(cl, 0x49a, uint16_t)));
    ORIG(expand_sub_fn, RVA_sbit_ExpandSubpixelFromMono)(AT(in, 0x30, uint8_t *), AT(in, 0x48, uint8_t *), w,
                                                         BM_H(cl + 0x2d0), src_rb, dst_rb, ov);
    int32_t s = AT(cl, 0x49a, int16_t);
    AT(cl, 0x2d8, int16_t) = (int16_t)(AT(cl, 0x2d8, int16_t) * s);
    AT(cl, 0x2d4, int16_t) = (int16_t)(AT(cl, 0x2d4, int16_t) * s);
    AT(cl, 0x330, int16_t) = (int16_t)(AT(cl, 0x330, int16_t) * s);
    AT(cl, 0x32c, int16_t) = (int16_t)(AT(cl, 0x32c, int16_t) * s);
    ORIG(bold_fn, RVA_OverscaleToBold)(AT(cl, 0x3dc, uint16_t), (uint16_t)s, cl + 0x328, cl + 0x2d0);
}

static int finish(uint8_t *cl, uint8_t *info) {
    AT(info, 0x58, uint8_t *) = AT(AT(cl, 0x118, uint8_t *), 0x28, uint8_t *);
    return 0;
}

/* the subpixel bitmap is the result: info + 0xe8 bits, + 0xf0 rowBytes */
static int subpixel_result(uint8_t *cl, uint8_t *info) {
    AT(info, 0xe8, uint8_t *) = AT(cl, 0x348, uint8_t *);
    AT(info, 0xf0, uint16_t) = AT(cl, 0x328, uint16_t);
    return 0;
}

TT_EXPORT int tt_fs_ContourScan(uint8_t *in, uint8_t *info) {
    uint8_t *cl = AT(in, 8, uint8_t *);
    if (cl == NULL) return 0x1001;
    AT(cl, 0x118, void *) = in + 8;
    AT(cl, 0x120, void *) = info;
    if (info == NULL) return 0x1003;
    void *p = AT(in, 0x50, void *);
    AT(cl, 8, void *) = p;
    if (p == NULL) return 0x1008;
    p = AT(in, 0x58, void *);
    AT(cl, 0x10, void *) = p;
    if (p == NULL) AT(cl, 0x10, void *) = idwr_orig(0xe5100); /* default ReleaseSfntFrag */
    if (!(AT(cl, 0xf8, void *) = AT(in, 0x60, void *))) return 0x100d;
    if (!(AT(cl, 0x100, void *) = AT(in, 0x68, void *))) return 0x100e;
    if (!(AT(cl, 0x108, void *) = AT(in, 0x70, void *))) return 0x100f;
    if ((AT(cl, 0x1d0, uint8_t) & 0x3e) != 0x3e) return 0x1005;
    AT(cl, 0, void *) = AT(in, 0x78, void *);
    int32_t compat = AT(in, 0x8c, int32_t);

    /* bold emboldening of gray strike/outline bitmaps under compatibility word 2:
     * 1 = embolden in gray only, 0xffff = embolden the 1-bit bitmap first */
    uint16_t emb = 0;
    if (compat == 2) {
        emb = 1;
        if (AT(cl, 0x416, uint16_t) != 1 || AT(cl, 0x418, uint16_t) != 0) emb = 0xffff;
        if (AT(cl, 0x351, uint8_t) != 0 && AT(cl, 0x3e2, uint16_t) != 0) emb = 0xffff;
    }

    uint8_t *bases = AT(cl, 0x118, uint8_t *);
    uint8_t *b3 = AT(bases, 0x18, uint8_t *);
    if (AT(cl, 0x140, uint8_t *) != b3) {
        static const int offs[6] = {0x208, 0x20c, 0x210, 0x214, 0x218, 0x24c};
        for (int i = 0; i < 6; i++) AT(cl, 0x1d8 + 8 * i, uint8_t *) = b3 + AT(cl, offs[i], uint32_t);
        ORIG(void (*)(uint8_t *, uint8_t *), RVA_UpdateWorkSpaceElement)(cl + 0x208, cl + 0x1d8);
        bases = AT(cl, 0x118, uint8_t *);
        AT(cl, 0x140, uint8_t *) = AT(bases, 0x18, uint8_t *);
    }
    uint8_t *r9, *r8;
    if (AT(cl, 0x250, uint32_t) == 0) {
        r9 = AT(bases, 0x30, uint8_t *);
    } else {
        uintptr_t b = (uintptr_t)AT(bases, 0x18, uint8_t *);
        if (b + AT(cl, 0x250, uint32_t) < b) overflow();
        r9 = (uint8_t *)(b + AT(cl, 0x250, uint32_t));
    }
    if (AT(cl, 0x254, uint32_t) == 0) {
        r8 = AT(bases, 0x38, uint8_t *);
    } else {
        uintptr_t b = (uintptr_t)AT(bases, 0x18, uint8_t *);
        if (b + AT(cl, 0x254, uint32_t) < b) overflow();
        r8 = (uint8_t *)(b + AT(cl, 0x254, uint32_t));
    }

    if (AT(cl, 0x351, uint8_t) != 0 && !(AT(in, 0x88, uint8_t) != 0 && (AT(cl, 0x41a, uint8_t) & 1))) {
        /* ---- embedded bitmap ---- */
        uint16_t saved410 = 0, saved412 = 0;
        int keep = emb == 1 || compat == 3;
        if (keep) {
            saved410 = AT(cl, 0x410, uint16_t);
            saved412 = AT(cl, 0x412, uint16_t);
            AT(cl, 0x410, uint32_t) = 0;
        }
        int e = ORIG(getbitmap_fn, RVA_sbit_GetBitmap)(cl + 0x3c0, cl, AT(in, 0x30, uint8_t *), r9);
        if (e) return e;
        if (keep) {
            AT(cl, 0x410, uint16_t) = saved410;
            AT(cl, 0x412, uint16_t) = saved412;
            if (compat == 3) {
                uint16_t w = BM_W(cl + 0x2d0);
                expand_subpixel(in, cl, (uint16_t)(((uint32_t)w + 0x1f) >> 3 & 0xfffc));
                if (AT(in, 0x89, uint8_t)) REVERSE(bitmap_bytes(cl + 0x328), AT(cl, 0x348, uint8_t *));
                return subpixel_result(cl, info); /* info + 0x58 is left as it was */
            }
        }
        if (emb != 0) {
            uint8_t *bits = AT(in, 0x30, uint8_t *);
            ORIG(expand_gray_fn, RVA_sbit_ExpandGrayFromMono)(bits, bits, BM_W(cl + 0x2d0), BM_H(cl + 0x2d0),
                                                              AT(cl, 0x488, uint16_t));
            if (emb == 1 && AT(cl, 0x3d8, uint16_t) != 3)
                ORIG(emb_gray_mono_fn, RVA_sbit_EmboldenGrayFromMono)(AT(in, 0x30, uint8_t *), BM_W(cl + 0x2d0),
                                                                      BM_H(cl + 0x2d0));
        }
        if (AT(in, 0x89, uint8_t)) REVERSE(AT(cl, 0x3d0, uint32_t), AT(in, 0x30, uint8_t *));
        return finish(cl, info);
    }

    /* ---- outline ---- */
    {
        uintptr_t b = (uintptr_t)AT(bases, 0x18, uint8_t *);
        if (b + AT(cl, 0x24c, uint32_t) < b) overflow();
        AT(cl, 0x310, uint8_t *) = (uint8_t *)(b + AT(cl, 0x24c, uint32_t));
    }
    uint8_t *el = AT(cl, 0x1e8, uint8_t *);
    _Alignas(8) uint8_t list[0x38];
    memset(list, 0, sizeof list);
    AT(list, 0, uint16_t) = AT(el, 0x50, uint16_t);
    AT(list, 0x08, uint64_t) = AT(el, 0x38, uint64_t);
    AT(list, 0x10, uint64_t) = AT(el, 0x40, uint64_t);
    AT(list, 0x18, uint64_t) = AT(el, 0x00, uint64_t);
    AT(list, 0x20, uint64_t) = AT(el, 0x08, uint64_t);
    AT(list, 0x28, uint64_t) = AT(el, 0x30, uint64_t);
    AT(list, 0x30, uint64_t) = AT(el, 0x58, uint64_t);
    if (r8 == NULL) AT(cl, 0x170, uint16_t) = 2;
    uint8_t *bm = cl + 0x2d0;
    AT(cl, 0x2f0, uint8_t *) = AT(in, 0x30, uint8_t *);
    int gray = compat != 2 && AT(cl, 0x488, uint16_t) != 0;
    if (gray || (AT(cl, 0x41a, uint8_t) & 1)) {
        AT(cl, 0x348, uint8_t *) = AT(in, 0x48, uint8_t *);
        bm = cl + 0x328;
    }
    AT(cl, 0x318, uint8_t *) = r9;
    AT(cl, 0x320, uint8_t *) = r8;
    int e = ORIG(fill_fn, RVA_FillGlyph)(list, bm, cl + 0x2f8, AT(cl, 0x170, uint16_t), AT(in, 0x89, uint8_t));
    if (e) return e;
    if (gray) {
        uint16_t ov = OVERSCALE(in, cl);
        e = AT(in, 0x88, uint8_t) ? ORIG(gray_fn, RVA_CalcGrayMapGdi)(cl + 0x328, cl + 0x2d0, ov)
                                  : ORIG(gray_fn, RVA_CalcGrayMap)(cl + 0x328, cl + 0x2d0, ov);
        if (e) return e;
    }
    if (AT(cl, 0x41a, uint8_t) & 1) {
        if (AT(in, 0x8a, uint8_t) == 0) {
            if (AT(in, 0x8c, int32_t) != 3) TO_SUBPIXEL(cl);
        } else {
            AT(info, 0xe8, uint8_t *) = AT(cl, 0x348, uint8_t *);
            AT(info, 0xf0, uint16_t) = AT(cl, 0x328, uint16_t);
            AT(info, 0xf4, uint16_t) = AT(cl, 0x32c, uint16_t);
            AT(info, 0xf2, uint16_t) = AT(cl, 0x32e, uint16_t);
            AT(info, 0xf8, uint16_t) = AT(cl, 0x330, uint16_t);
            AT(info, 0xf6, uint16_t) = AT(cl, 0x32a, uint16_t);
        }
    }
    compat = AT(in, 0x8c, int32_t);
    if (compat == 0) return finish(cl, info);

    /* ---- bold simulation (ClearType compatibility word 1, 2 or 3) ---- */
    if (OVERSCALE(in, cl) != 0) {
        /* gray: embolden the gray map */
        uint16_t a = OVERSCALE(in, cl), c = OVERSCALE(in, cl);
        uint16_t levels = (uint16_t)(a * c + 1);
        int64_t h = (int64_t)AT(cl, 0x2d2, int16_t) - AT(cl, 0x2d6, int16_t);
        if ((uint64_t)(h + 0x80000000LL) > 0xffffffffULL || (uint32_t)h > 0xffff) overflow();
        int64_t w = (int64_t)AT(cl, 0x2d8, int16_t) - AT(cl, 0x2d4, int16_t);
        if ((uint64_t)(w + 0x80000000LL) > 0xffffffffULL || (uint32_t)w > 0xffff) overflow();
        ORIG(emb_gray_fn, RVA_sbit_EmboldenGray)(AT(cl, 0x2f0, uint8_t *), (uint16_t)w, (uint16_t)h,
                                                  AT(cl, 0x2d0, uint16_t), levels, AT(cl, 0x416, uint16_t),
                                                  AT(cl, 0x418, uint16_t));
        return finish(cl, info);
    }
    if (AT(cl, 0x41a, uint8_t) & 1) {
        if (compat == 3) {
            /* ClearType: embolden the overscaled bitmap, then subpixel */
            uint32_t n = bitmap_bytes(cl + 0x328);
            uint8_t *bits = AT(cl, 0x348, uint8_t *);
            if (AT(in, 0x89, uint8_t)) REVERSE(n, bits);
            TO_BOLD(cl);
            if (AT(in, 0x89, uint8_t)) REVERSE(n, bits);
            if (AT(in, 0x8a, uint8_t) == 0) TO_SUBPIXEL(cl);
        } else {
            ORIG(emb_fn, RVA_sbit_EmboldenSubPixel)(AT(cl, 0x2f0, uint8_t *), BM_W(cl + 0x2d0), BM_H(cl + 0x2d0),
                                                     AT(cl, 0x2d0, uint16_t), AT(cl, 0x416, uint16_t),
                                                     AT(cl, 0x418, uint16_t));
        }
        return finish(cl, info);
    }
    uint32_t n = bitmap_bytes(bm);
    if (compat == 3) {
        /* aliased under compatibility word 3: expand to the subpixel overscale and embolden */
        if (AT(in, 0x89, uint8_t)) REVERSE(n, AT(bm, 0x20, uint8_t *));
        expand_subpixel(in, cl, 0);
        if (AT(in, 0x89, uint8_t)) REVERSE(bitmap_bytes(cl + 0x328), AT(cl, 0x348, uint8_t *));
        subpixel_result(cl, info);
        return finish(cl, info);
    }
    uint8_t *bits = AT(bm, 0x20, uint8_t *);
    if (AT(in, 0x89, uint8_t)) REVERSE(n, bits);
    if (emb != 1) {
        ORIG(emb_fn, RVA_sbit_Embolden)(AT(bm, 0x20, uint8_t *), BM_W(bm), BM_H(bm), AT(bm, 0, uint16_t),
                                        AT(cl, 0x416, uint16_t), AT(cl, 0x418, uint16_t));
    }
    if (emb != 0) {
        ORIG(expand_gray_fn, RVA_sbit_ExpandGrayFromMono)(AT(bm, 0x20, uint8_t *), AT(bm, 0x20, uint8_t *), BM_W(bm),
                                                          BM_H(bm), AT(cl, 0x488, uint16_t));
        if (emb == 1) ORIG(emb_gray_mono_fn, RVA_sbit_EmboldenGrayFromMono)(AT(bm, 0x20, uint8_t *), BM_W(bm), BM_H(bm));
    }
    if (AT(in, 0x89, uint8_t)) REVERSE(n, bits);
    return finish(cl, info);
}
