/*
 * Leaf functions of the glyph-run texture and positioning path, ported from DWrite.dll
 * 10.0.26100.9711 so a texture can be built without the original: the callees of
 * glyphrun_texture.c (its O_* macros) and of glyphrun_layout.c (ORIG) that are pure
 * computation on the caller's data. Each is exported as tt_<name> for the function swap.
 *
 *   texture path
 *     FillGlyphBitmaps                          0x1800191ac  1-bit / 4x4 run bitmap of a glyph run
 *     IsRectEmpty                               0x1800194a8
 *     GetRenderingModeAttributes(RASTER_TYPE)   0x1800194c8  table 0x1801c15f8
 *     MergeGlyph1Bit                            0x180016be0  dispatcher + the inlined uchar-RLE merge
 *     MergeGlyph4x4                             0x180017b78  dispatcher (clips whole glyphs first)
 *     MergeScanLine                             0x1800155b0  4x4 ushort-RLE scanline OR
 *     CachedBitmapReader<uint16_t>::MoveNextY   0x180015518
 *     CachedBitmapReader<uint16_t>::MoveNextX   0x1800d11ec
 *     CachedBitmapReader<uint8_t>::MoveNextY    0x1800ac460
 *   positioning / layout path
 *     GetRenderingModeAttributes(MODE2, AA)     0x1800fb630  tables 0x1801e1268 (AA 0), 0x1801e1240 (AA 1)
 *     GetTransformMaxScaleFactor                0x180075424
 *     ComputeGlyphOrigins                       0x180049844
 *     MetricsConverter::MetricsConverter        0x180049674
 *     MetricsConverter::DesignToClientUnits     0x180049428
 *     PartitionArray<aliased glyphs last>       0x1800fb708  and its swap lambda 0x180151c3c
 *     PartitionArray<odd-phase glyphs first>    0x180151b8c
 *     DownSample2x2                             0x1800cdde8  tables 0x1801d84b0, 0x1801d85b0
 *     CountBits                                 0x180087730
 *     Align<unsigned int>                       0x1800195f0
 *     ConstrainGlyphIndex                       0x1800a6c44
 *     HashBlob                                  0x180055380  Jenkins lookup3 hashlittle
 *     HashWords                                 0x18004c650  Jenkins lookup3 hashword
 * plus tt_ModeAttributeTable, a copy of the uint32[2][8] table 0x1801d8420 that
 * GlyphRunAnalysis::GlyphRunAnalysis indexes inline (glyphrun_layout.c reads it in the DLL).
 *
 * As in the other groups, a call between functions that have a port goes through the original
 * entry (ORIG by RVA), so swapping one function alone keeps the rest original. Callees that stay
 * DWrite infrastructure are reached through ORIG too: the exception throwers
 * (Exception::Exception, LogAndThrow<T>, CaptureStack, SafeIntOnOverflow) and, on the
 * ComputeGlyphOrigins paths that need font metrics (RTL, sideways or no advances),
 * FontFace::GetGlyphMetrics / GetGlyphAdvances.
 *
 * Register use: DWrite.dll is built with whole-program optimisation and its callers keep values
 * in volatile registers across calls that are known not to touch them (PartitionArray keeps R11
 * across its swap lambda; ComputeGlyphOrigins keeps XMM3-XMM5 and R8-R11 across
 * DesignToClientUnits). Every export is therefore a thunk that saves RAX, RCX, RDX, R8-R11 and
 * XMM0-XMM5, re-pushes 16 stack arguments, calls the port with the original x64 signature and
 * restores all of them except the return register (RAX, or XMM0 for a float result). The bool
 * results return the original's whole RAX (the readers leave a pointer in its upper bits), and
 * the thunks carry SEH unwind data so the original's exceptions unwind through them.
 */
#include <stdlib.h>
#include "fatal.h"
#include <string.h>
#include "portable.h"   /* the SSE intrinsics, or stand-ins elsewhere */
#include "tt_state.h"
#include "orig.h"

/* Standalone mode (-DIDWR_STANDALONE): every caller is a C port, so the exports are not the
 * register-preserving thunks of the end of the file but the ports themselves, renamed here to
 * the export names (plain C functions on any ABI). */
#ifdef IDWR_STANDALONE
#define gh_FillGlyphBitmaps                tt_FillGlyphBitmaps
#define gh_IsRectEmpty                     tt_IsRectEmpty
#define gh_GetRasterTypeAttributes         tt_GetRasterTypeAttributes
#define gh_MergeGlyph1Bit                  tt_MergeGlyph1Bit
#define gh_MergeGlyph4x4                   tt_MergeGlyph4x4
#define gh_MergeScanLine                   tt_MergeScanLine
#define gh_MoveNextY_u16                   tt_CachedBitmapReader_u16_MoveNextY
#define gh_MoveNextX_u16                   tt_CachedBitmapReader_u16_MoveNextX
#define gh_MoveNextY_u8                    tt_CachedBitmapReader_u8_MoveNextY
#define gh_GetRenderingModeAttributes      tt_GetRenderingModeAttributes
#define gh_GetTransformMaxScaleFactor      tt_GetTransformMaxScaleFactor
#define gh_ComputeGlyphOrigins             tt_ComputeGlyphOrigins
#define gh_MetricsConverter_ctor           tt_MetricsConverter_ctor
#define gh_DesignToClientUnits             tt_MetricsConverter_DesignToClientUnits
#define gh_PartitionAliased                tt_PartitionAliased
#define gh_PartitionAliasedSwap            tt_PartitionAliasedSwap
#define gh_PartitionOddPhase               tt_PartitionOddPhase
#define gh_DownSample2x2                   tt_DownSample2x2
#define gh_CountBits                       tt_CountBits
#define gh_Align                           tt_Align
#define gh_ConstrainGlyphIndex             tt_ConstrainGlyphIndex
#define gh_HashBlob                        tt_HashBlob
#define gh_HashWords                       tt_HashWords
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t  i32;
typedef int64_t  i64;


/* callees with a port (reached at their original entry) */
#define RVA_IsRectEmpty                   0x194a8
#define RVA_GetRasterTypeAttributes       0x194c8
#define RVA_MergeGlyph1Bit                0x16be0
#define RVA_MergeGlyph4x4                 0x17b78
#define RVA_MergeUncompressedGlyph1Bit    0x16ef0
#define RVA_MergeCompressedGlyph1Bit_u16  0x14f94
#define RVA_MergeUncompressedGlyph4x4     0x17c18
#define RVA_MergeCompressedGlyph4x4_u8    0x182d0
#define RVA_MergeCompressedGlyph4x4_u16   0x15210
#define RVA_Align                         0x195f0
#define RVA_PartitionAliasedSwap          0x151c3c
#define RVA_MetricsConverter_ctor         0x49674
#define RVA_DesignToClientUnits           0x49428
/* DWrite infrastructure */
#define RVA_FontFace_GetGlyphMetrics      0x4a3d4
#define RVA_FontFace_GetGlyphAdvances     0x4a8d0
#define RVA_Exception_ctor                0xaa7f0   /* Exception::Exception(int hr, uint) */
#define RVA_CaptureStack                  0x1cf60
#define RVA_LogAndThrow_Argument          0xfb6cc   /* LogAndThrow<ArgumentException> */
#define RVA_LogAndThrow_ArgOutOfRange     0x10bf54  /* LogAndThrow<ArgumentOutOfRangeException> */
#define RVA_SafeIntOnOverflow             0xfa5cc

#define HR_E_INVALIDARG 0x80070057u

/* ================================================================== tables ============== */
/* RenderingModeAttributes by DWRITE_RASTER_TYPE (0x1801c15f8) */
static const u32 g_rasterTypeAttributes[6] = { 0x00800011, 0x06010016, 0x0a010056, 0x0c020318, 0x10422244, 0x14021122 };
/* by DWRITE_RENDERING_MODE2, DWRITE_TEXT_ANTIALIAS_MODE_CLEARTYPE (0x1801e1268) */
static const u32 g_modeAttributesAA0[10] = { 0xfc000000, 0x00b80011, 0x06bd0016, 0x069d0016, 0x06910016, 0x0a910056, 0xfc000000, 0x0a910056, 0x0abd0056, 0x0a9d0056 };
/* by DWRITE_RENDERING_MODE2, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE (0x1801e1240) */
static const u32 g_modeAttributesAA1[10] = { 0xfc000000, 0x00b80011, 0x0cba0318, 0x0c9a0318, 0x0c920318, 0x10d22244, 0xfc000000, 0x14921122, 0x10fa2244, 0x10da2244 };
/* g_convert4x4BitsTo2x2, used when the row phase is 1 (0x1801d84b0) */
static const u8 g_convert4x4BitsTo2x2[256] = {
    0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1,
    0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1,
    0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 3,
    0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 2, 3, 3, 3,
    0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 2, 3, 3, 3,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3,
    0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 3,
    0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 2, 3, 3, 3,
    0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 2, 3, 3, 3,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 3, 3,
    0, 0, 0, 1, 2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 2, 3,
    0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3,
    0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3,
    1, 1, 1, 1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
};
/* the table after it, used when the row phase is 0 (0x1801d85b0) */
static const u8 g_convert4x4BitsTo2x2Odd[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 2, 2,
    0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 3,
    0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 3,
    0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 2, 3, 3, 3,
    0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    0, 0, 0, 1, 2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 2, 3,
    0, 0, 0, 1, 2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 2, 3,
    0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3,
    0, 0, 0, 0, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    0, 0, 0, 1, 2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 2, 3,
    0, 0, 0, 1, 2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 2, 3,
    0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 2, 3,
    2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 2, 3, 2, 2, 2, 3,
    2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3,
};

/* exported copy of 0x1801d8420 for glyphrun_layout.c's inline lookup [antialias][mode] */
TT_EXPORT const u32 tt_ModeAttributeTable[16] = {
    0xfc000000, 0x00b80011, 0x06bd0016, 0x069d0016, 0x06910016, 0x0a110056, 0xfc000000, 0x0a110056,
    0xfc000000, 0x00b80011, 0x0cba0318, 0x0c9a0318, 0x0c920318, 0x10522244, 0xfc000000, 0x14121122 };

/* ================================================================ exceptions ============ */

__attribute__((noreturn)) static void safeint_overflow(void) {
    ORIG(void (*)(void), RVA_SafeIntOnOverflow)();
    IDWR_FATAL("glyphrun_helpers.c:174");
}

/* Exception::Exception(&e, hr, 0), then LogAndThrow<T>(e, tag, line) */
__attribute__((noreturn)) static void throw_hr(u32 thrower, u64 tag, u32 line) {
    __attribute__((aligned(16))) u8 e[32];
    memset(e, 0, sizeof e);
    ORIG(void (*)(void *, u32, u32), RVA_Exception_ctor)(e, HR_E_INVALIDARG, 0);
    ORIG(void (*)(void *, u64, u32), thrower)(e, tag, line);
    IDWR_FATAL("glyphrun_helpers.c:183");
}

/* the "dword hr + CaptureStack" form */
__attribute__((noreturn)) static void throw_captured(u64 tag, u32 line) {
    __attribute__((aligned(16))) u8 e[32];
    memset(e, 0, sizeof e);
    *(u32 *)e = HR_E_INVALIDARG;
    ORIG(void (*)(u32, u32), RVA_CaptureStack)(HR_E_INVALIDARG, 0);
    ORIG(void (*)(void *, u64, u32), RVA_LogAndThrow_Argument)(e, tag, line);
    IDWR_FATAL("glyphrun_helpers.c:193");
}

#define TAG_RENDERING_ATTR 0x727474615f72ull   /* "r_attr" */
#define TAG_FILLBMP        0x706d626c6c6966ull /* "fillbmp" */
#define TAG_MERGBM         0x6d626772656dull   /* "mergbm" */
#define TAG_CGID           0x64696763ull       /* "cgid" */

typedef void (*merge_fn)(const u8 *, i32, i32, u32, u32, u32, void *);

/* ============================================================ rendering attributes ====== */

/* IsRectEmpty 0x1800194a8: right <= left || bottom <= top. RAX keeps the last coordinate read
 * above AL. */
u64 gh_IsRectEmpty(const i32 *r) {
    u32 v = (u32)r[0];
    if (r[2] <= (i32)v) return (v & ~0xffu) | 1;
    v = (u32)r[1];
    if (r[3] <= (i32)v) return (v & ~0xffu) | 1;
    return v & ~0xffu;
}

/* GetRenderingModeAttributes(DWRITE_RASTER_TYPE) 0x1800194c8 */
u32 gh_GetRasterTypeAttributes(u32 type) {
    if (type >= 6) throw_hr(RVA_LogAndThrow_Argument, TAG_RENDERING_ATTR, 0xd5);
    return g_rasterTypeAttributes[type];
}

/* GetRenderingModeAttributes(DWRITE_RENDERING_MODE2, DWRITE_TEXT_ANTIALIAS_MODE) 0x1800fb630 */
u32 gh_GetRenderingModeAttributes(u32 mode, u32 aa) {
    if (mode >= 10) throw_hr(RVA_LogAndThrow_Argument, TAG_RENDERING_ATTR, 0xbf);
    if (aa == 0) return g_modeAttributesAA0[mode];
    if (aa != 1) throw_hr(RVA_LogAndThrow_Argument, TAG_RENDERING_ATTR, 0xcb);
    return g_modeAttributesAA1[mode];
}

/* ===================================================================== merges ============ */

/* MergeGlyph1Bit 0x180016be0: uncompressed and ushort-RLE layouts go to their merges; the
 * uchar-RLE merge is inlined here. Each RLE record (see glyphrun_texture.c) gives the ink spans
 * of one scanline, replayed for `repeat` more rows; spans are OR-ed as runs of 1 bits into
 * row-major u32 words (bit 0 leftmost), clipped to [0, clipW). */
void gh_MergeGlyph1Bit(const u8 *l, i32 x, i32 y, u32 clipW, u32 rows, u32 stride, u8 *dst) {
    u32 hdr = *(const u32 *)l, kind = hdr >> 26;
    if (kind & 0x10) {
        ORIG(merge_fn, RVA_MergeUncompressedGlyph1Bit)(l, x, y, clipW, rows, stride, dst);
        return;
    }
    if (kind & 8) {
        ORIG(merge_fn, RVA_MergeCompressedGlyph1Bit_u16)(l, x, y, clipW, rows, stride, dst);
        return;
    }
    if (((stride >> 2) << 5) < clipW) throw_hr(RVA_LogAndThrow_Argument, TAG_MERGBM, 0x4c8);
    i32 xLimit = (i32)((u32)*(const u16 *)(l + 8) + (u32)x);
    const u8 *end = l + 0x10 + (hdr & 0x3ffffff);
    const u8 *rec = l + 0x10, *recEnd = l + 0x10, *p;
    u32 rep = 0;
    i32 row = (i32)((u32)y - 1);

#define NEXT_RECORD()                                                              \
    do {                                                                           \
        if (recEnd == end) return;                                                 \
        u64 h = *recEnd;                                                           \
        rec = recEnd + 1;                                                          \
        if ((h & 1) && rec < end) { rep = (u32)h >> 1; h = *rec; rec++; }          \
        i64 n = (i64)(h >> 1), avail = end - rec;                                  \
        recEnd = rec + (avail >= n ? n : avail);                                   \
    } while (0)

    do {    /* skip the records of rows above the bitmap */
        row = (i32)((u32)row + 1);
        if (rep != 0) rep--;
        else NEXT_RECORD();
        p = rec;
    } while (row < 0);

    for (;;) {
        if ((u32)row >= rows) return;
        u32 *drow = (u32 *)(dst + (u32)((u32)row * stride));
        i32 prev = x;
        while (p != recEnd) {
            i32 s = (i32)((u32)*p++ + (u32)prev);
            if (p == recEnd) prev = xLimit;
            else prev = (i32)((u32)*p++ + (u32)s);
            i32 a = s < 0 ? 0 : s;
            i32 b = (i32)clipW < prev ? (i32)clipW : prev;
            if (a >= b) continue;
            u32 ws = (u32)a >> 5, we = (u32)b >> 5;
            u32 lm = 0xffffffffu << (a & 31);
            u32 rm = 0xffffffffu >> ((32 - (b & 31)) & 31);
            if (ws == we) {
                drow[ws] = (rm & lm) | drow[ws];
            } else {
                drow[ws] |= lm;
                if (ws + 1 < we) memset(&drow[ws + 1], 0xff, (u64)(we - (ws + 1)) << 2);
                if (b & 0x1f) drow[we] |= rm;
            }
        }
        row = (i32)((u32)row + 1);
        if (rep != 0) rep--;
        else NEXT_RECORD();
        p = rec;
    }
#undef NEXT_RECORD
}

/* MergeGlyph4x4 0x180017b78: skips glyphs entirely outside the 4x4 target, then dispatches */
void gh_MergeGlyph4x4(const u8 *l, i32 x, i32 y, u32 w, u32 h, u32 stride, void *buf) {
    if (x >= (i32)(w >> 2)) return;
    if (y >= (i32)(h * 4)) return;
    if ((i32)((u32)*(const u16 *)(l + 8) + (u32)x) <= 0) return;
    if ((i32)((u32)*(const u16 *)(l + 10) + (u32)y) <= 0) return;
    u32 kind = *(const u32 *)l >> 26;
    u32 rva = (kind & 0x10) ? RVA_MergeUncompressedGlyph4x4
            : (kind & 8) ? RVA_MergeCompressedGlyph4x4_u16 : RVA_MergeCompressedGlyph4x4_u8;
    ORIG(merge_fn, rva)(l, x, y, w, h, stride, buf);
}

/* FillGlyphBitmaps 0x1800191ac: merges every glyph of a run into the caller's bitmap of the
 * run's rect (scaled by the raster type's overscale, widened by the ClearType spread). */
void gh_FillGlyphBitmaps(u32 type, u32 count, const u8 *const *layouts, const u64 *pos, const i32 *rect,
                         u32 rows, u32 stride, void *buf) {
    if ((u8)ORIG(u64 (*)(const i32 *), RVA_IsRectEmpty)(rect)) return;
    u32 attr = ORIG(u32 (*)(u32), RVA_GetRasterTypeAttributes)(type);
    u64 ox = attr & 0xf, oy = attr >> 4 & 0xf, spread = attr >> 24 & 3;
    i64 l = (i64)rect[0] * (i64)ox;
    if ((u64)(l + 0x80000000ll) > 0xffffffffull) safeint_overflow();
    i64 x0 = (i64)(i32)l - (i64)spread;
    if (x0 < -0x80000000ll) safeint_overflow();
    i64 y0 = (i64)rect[1] * (i64)oy;
    if ((u64)(y0 + 0x80000000ll) > 0xffffffffull) safeint_overflow();
    u32 dw = (u32)rect[2] - (u32)rect[0];
    if ((i32)dw < 0) safeint_overflow();
    u64 ww = (u64)dw * ox;
    if (ww > 0xffffffffull) safeint_overflow();
    u32 width = (u32)ww + (u32)spread * 2;
    if (width < (u32)ww) safeint_overflow();
    u32 dh = (u32)rect[3] - (u32)rect[1];
    if ((i32)dh < 0) safeint_overflow();
    u64 hh = (u64)dh * oy;
    if (hh > 0xffffffffull) safeint_overflow();
    u32 height = (u32)hh;
    if (type == 4) {
        if (width << 2 > stride * 8) throw_hr(RVA_LogAndThrow_Argument, TAG_FILLBMP, 0x259);
        if (height >> 2 > rows) throw_hr(RVA_LogAndThrow_Argument, TAG_FILLBMP, 0x25d);
        for (u32 i = 0; i < count; i++)
            ORIG(merge_fn, RVA_MergeGlyph4x4)(layouts[i], (i32)((u32)pos[i] - (u32)x0),
                                              (i32)((u32)(pos[i] >> 32) - (u32)y0), width << 2, height >> 2,
                                              stride, buf);
        return;
    }
    if (type > 3 && type != 5) throw_captured(TAG_FILLBMP, 0x292);
    if (width > stride * 8) throw_hr(RVA_LogAndThrow_Argument, TAG_FILLBMP, 0x279);
    if (height > rows) throw_hr(RVA_LogAndThrow_Argument, TAG_FILLBMP, 0x27d);
    for (u32 i = 0; i < count; i++)
        ORIG(merge_fn, RVA_MergeGlyph1Bit)(layouts[i], (i32)((u32)pos[i] - (u32)x0),
                                           (i32)((u32)(pos[i] >> 32) - (u32)y0), width, height, stride, buf);
}

/* MergeScanLine 0x1800155b0: ORs one 4x4 scanline into row words [start, end] */
void gh_MergeScanLine(u32 *row, u32 leftMask, u32 rightMask, u32 fill, u32 start, u32 end, u16 tail) {
    u32 first = row[start];
    if (start == end) {
        row[start] = (leftMask & rightMask) | first;
        return;
    }
    row[start] = first | leftMask;
    for (u32 i = start + 1; i < end; i++) row[i] |= fill;
    if (tail != 0) row[end] |= rightMask;
}

/* ========================================================== CachedBitmapReader ========== */

/* CachedBitmapReader<T> (0x38 bytes): x0, xLimit, span start, span end, y, repeat count of the
 * current record, its first element, its end, the span cursor, the data end */
typedef struct { i32 x0, xLimit, start, end, y, runRem; const u8 *cur, *rowEnd, *cursor, *dataEnd; } Reader;

#define LOW0(v) ((u64)(v) & ~0xffull)

/* CachedBitmapReader<unsigned short>::MoveNextY 0x180015518: advances `adv` rows; false at the
 * end of the data. The span state restarts at x0. */
u64 gh_MoveNextY_u16(Reader *r, u32 adv) {
    u64 rax = (u32)r->x0;
    r->y = (i32)((u32)r->y + adv);
    u32 rem = (u32)r->runRem;
    r->end = r->x0;
    r->start = r->x0;
    if (rem != 0) {
        rax = (u32)(rem + 1);
        if (adv != rem + 1) {
            const u8 *c = r->cur;
            r->runRem = (i32)(rem - adv);
            r->cursor = c;
            return LOW0(c) | 1;
        }
        r->runRem = 0;
    }
    const u8 *p = r->rowEnd, *e = r->dataEnd;
    if (p == e) return LOW0(rax);
    u64 h = *(const u16 *)p;
    p += 2;
    if ((h & 1) && p < e) {
        r->runRem = (i32)((u32)h >> 1);
        h = *(const u16 *)p;
        p += 2;
    }
    i64 n = (i64)(e - p) >> 1, len = (i64)(h >> 1);
    if (n >= len) n = len;
    r->cur = p;
    r->cursor = p;
    r->rowEnd = p + n * 2;
    return LOW0(r->rowEnd) | 1;
}

/* CachedBitmapReader<unsigned char>::MoveNextY 0x1800ac460 */
u64 gh_MoveNextY_u8(Reader *r, u32 adv) {
    u64 rax = (u32)r->x0;
    r->y = (i32)((u32)r->y + adv);
    u32 rem = (u32)r->runRem;
    r->end = r->x0;
    r->start = r->x0;
    if (rem != 0) {
        rax = (u32)(rem + 1);
        if (adv != rem + 1) {
            const u8 *c = r->cur;
            r->cursor = c;
            r->runRem = (i32)(rem - adv);
            return LOW0(c) | 1;
        }
        r->runRem = 0;
    }
    const u8 *p = r->rowEnd, *e = r->dataEnd;
    if (p == e) return LOW0(rax);
    u64 h = *p++;
    if ((h & 1) && p < e) {
        r->runRem = (i32)((u32)h >> 1);
        h = *p++;
    }
    u64 len = h >> 1;
    i64 n = e - p;
    if (n >= (i64)len) n = (i64)len;
    r->cur = p;
    r->cursor = p;
    r->rowEnd = p + n;
    return LOW0(len) | 1;
}

/* CachedBitmapReader<unsigned short>::MoveNextX 0x1800d11ec: the next [start, end) span of the
 * current row; a missing end means the glyph's right edge (xLimit) */
u64 gh_MoveNextX_u16(Reader *r) {
    const u8 *p = r->cursor;
    if (p == r->rowEnd) return LOW0(p);
    i32 s = (i32)((u32)*(const u16 *)p + (u32)r->end);
    p += 2;
    i32 e;
    if (p == r->rowEnd) e = r->xLimit;
    else { e = (i32)((u32)*(const u16 *)p + (u32)s); p += 2; }
    r->cursor = p;
    r->start = s;
    r->end = e;
    return LOW0(p) | 1;
}

/* ============================================================== cache helpers =========== */

/* CountBits 0x180087730: the length of the run of bits equal to `value` (0 or not) starting at
 * bit `pos` of a row of `words` u32 words (bit 0 first). The word index is 16 bits wide. */
u32 gh_CountBits(const u32 *bits, u32 words, u32 pos, u32 value) {
    u16 w = (u16)(pos >> 5), b = (u16)(pos & 0x1f);
    if ((u32)w >= words) return 0;
    u32 mask = value == 0 ? 0xffffffffu : 0;
    u32 v = mask ^ bits[w], n = 0;
    while ((v >> (b & 31)) & 1) {
        n++;
        if (++b >= 0x20) break;
    }
    while (b == 0x20) {
        u32 saved = n;
        if ((u32)w + 1 >= words) return n;
        w++;
        v = mask ^ bits[w];
        if (v == 0xffffffffu) { n += 0x20; continue; }
        n += 0x10;
        b = 0x10;
        if ((u16)v != 0xffff) { b = 0; n = saved; }
        while ((v >> (b & 31)) & 1) {
            n++;
            if (++b >= 0x20) break;
        }
    }
    return n;
}

/* Align<unsigned int> 0x1800195f0: round up to a power-of-two alignment, SafeInt-checked */
u32 gh_Align(u32 v, u64 a) {
    u64 t = (u64)v + (a - 1);
    if (t < (u64)v || t > 0xffffffffull) safeint_overflow();
    return (u32)t & (0u - (u32)a);
}

/* ConstrainGlyphIndex 0x1800a6c44: out-of-range glyph ids become 0 (throws for an empty font) */
u32 gh_ConstrainGlyphIndex(u16 g, u16 n) {
    if (g < n) return g;
    if (n != 0) return 0;
    throw_hr(RVA_LogAndThrow_ArgOutOfRange, TAG_CGID, 0x31f);
}

#define ROT(x, k) (((x) << (k)) | ((x) >> (32 - (k))))
#define MIX(a, b, c) do { \
    a -= c; a ^= ROT(c, 4);  c += b; \
    b -= a; b ^= ROT(a, 6);  a += c; \
    c -= b; c ^= ROT(b, 8);  b += a; \
    a -= c; a ^= ROT(c, 16); c += b; \
    b -= a; b ^= ROT(a, 19); a += c; \
    c -= b; c ^= ROT(b, 4);  b += a; } while (0)
#define FINAL(a, b, c) do { \
    c ^= b; c -= ROT(b, 14); \
    a ^= c; a -= ROT(c, 11); \
    b ^= a; b -= ROT(a, 25); \
    c ^= b; c -= ROT(b, 16); \
    a ^= c; a -= ROT(c, 4);  \
    b ^= a; b -= ROT(a, 14); \
    c ^= b; c -= ROT(b, 24); } while (0)

/* HashBlob 0x180055380: lookup3 hashlittle. The original has separate word, half-word and byte
 * paths for the pointer's alignment; all of them read the bytes little-endian, as here. */
u32 gh_HashBlob(const void *data, u64 len, u32 init) {
    const u8 *k = (const u8 *)data;
    u32 a, b, c;
    a = b = c = 0xdeadbeefu + (u32)len + init;
#define LE32(p) ((u32)(p)[0] | (u32)(p)[1] << 8 | (u32)(p)[2] << 16 | (u32)(p)[3] << 24)
    while (len > 12) {
        a += LE32(k);
        b += LE32(k + 4);
        c += LE32(k + 8);
        MIX(a, b, c);
        len -= 12;
        k += 12;
    }
    switch (len) {
    case 12: c += (u32)k[11] << 24; /* fall through */
    case 11: c += (u32)k[10] << 16; /* fall through */
    case 10: c += (u32)k[9] << 8;   /* fall through */
    case 9:  c += k[8];             /* fall through */
    case 8:  b += (u32)k[7] << 24;  /* fall through */
    case 7:  b += (u32)k[6] << 16;  /* fall through */
    case 6:  b += (u32)k[5] << 8;   /* fall through */
    case 5:  b += k[4];             /* fall through */
    case 4:  a += (u32)k[3] << 24;  /* fall through */
    case 3:  a += (u32)k[2] << 16;  /* fall through */
    case 2:  a += (u32)k[1] << 8;   /* fall through */
    case 1:  a += k[0]; break;
    case 0:  return c;
    }
#undef LE32
    FINAL(a, b, c);
    return c;
}

/* HashWords 0x18004c650: lookup3 hashword */
u32 gh_HashWords(const u32 *k, u64 n, u32 init) {
    u32 a, b, c;
    a = b = c = 0xdeadbeefu + ((u32)n << 2) + init;
    while (n > 3) {
        a += k[0];
        b += k[1];
        c += k[2];
        MIX(a, b, c);
        n -= 3;
        k += 3;
    }
    switch (n) {
    case 3: c += k[2]; /* fall through */
    case 2: b += k[1]; /* fall through */
    case 1: a += k[0]; FINAL(a, b, c); /* fall through */
    case 0: break;
    }
    return c;
}

/* DownSample2x2 0x1800cdde8: halves a 4x4-overscaled (2 bits per subpixel pair) bitmap to 2x2
 * in place. The bitmap struct: +0 left, +4 top, +8 width, +0xc rows, +0x14 row bytes, +0x18
 * bits. Output byte j of a row pair maps the 4-bit nibbles of two input bytes per row through
 * one of the two tables, alternating per output row (starting from the dithering phase). */
void gh_DownSample2x2(u8 *bm, u8 phase) {
    u32 stride = AT(bm, 0x14, u32);
    i32 top = AT(bm, 4, i32);
    i32 ntop = (i32)(0u - (u32)top);
    i32 rowsEnd = (i32)((u32)AT(bm, 0xc, i32) + (u32)ntop);
    i32 q = (i32)stride / 4;
    u32 newStride = ORIG(u32 (*)(u32, u64), RVA_Align)((u32)q + (u32)q, 4);
    i32 left2 = AT(bm, 0, i32) >> 1;
    i32 top2 = (i32)((u32)top + 1) >> 1;
    u8 par = (u8)((((u8)top2 ^ (u8)left2) & 1) ^ phase);
    u8 *bits = AT(bm, 0x18, u8 *), *out = bits;
    u32 outRows = 0;
    i32 r = (i32)((u32)ntop & 0xfffffffeu);
    if (r < rowsEnd) {
        do {
            u8 *rowEnd = out + newStride;
            const u8 *a = r < ntop ? NULL : bits + (u32)((u32)(r - ntop) * stride);
            i32 r1 = (i32)((u32)r + 1);
            const u8 *b = r1 < rowsEnd ? bits + (u32)(((u32)r1 - (u32)ntop) * stride) : NULL;
            const u8 *T = par ? g_convert4x4BitsTo2x2 : g_convert4x4BitsTo2x2Odd;
            r = (i32)((u32)r + 2);
            u32 k = 0;
            for (; out < rowEnd; out++) {
                u8 a0 = (a && k < stride) ? a[k] : 0;
                u8 b0 = (b && k < stride) ? b[k] : 0;
                k++;
                u8 a1 = (a && k < stride) ? a[k] : 0;
                u8 b1 = (b && k < stride) ? b[k] : 0;
                k++;
                u8 v = T[(a1 & 0xf0) | (b1 >> 4)];
                v = (u8)(v << 2) | T[(u8)(a1 << 4) | (b1 & 0xf)];
                v = (u8)(v << 2) | T[(a0 & 0xf0) | (b0 >> 4)];
                v = (u8)(v << 2) | T[(u8)(a0 << 4) | (b0 & 0xf)];
                *out = v;
            }
            outRows++;
            par ^= 1;
            out = rowEnd;
        } while (r < rowsEnd);
    }
    AT(bm, 0, i32) = left2;
    AT(bm, 4, i32) = top2;
    AT(bm, 0xc, u32) = outRows;
    AT(bm, 8, i32) = (i32)((u32)AT(bm, 8, i32) + 1) >> 1;
    AT(bm, 0x14, u32) = newStride;
}

/* =========================================================== glyph run layout =========== */

/* PartitionArray<...> 0x1800fb708: moves the glyphs whose layout has header bit 26 (aliased
 * embedded bitmaps) to the end; returns the count of the others. The swap is the lambda
 * 0x180151c3c (ctx = { GlyphRunAnalysis *, PointF **origins }). */
u32 gh_PartitionAliased(u32 count, u8 *self, void **ctx) {
    u32 i = 0, j = count - 1;
    if (j != 0) {
        do {
            u8 **L = AT(self, 0x30, u8 **);
            if (!(*(const u32 *)L[i] & 0x4000000)) {
                i++;
            } else {
                if (!(*(const u32 *)L[j] & 0x4000000)) {
                    ORIG(void (*)(void **, u32, u32), RVA_PartitionAliasedSwap)(ctx, i, j);
                    i++;
                }
                j--;
            }
        } while (i < j);
    }
    if (i == j && !(*(const u32 *)AT(self, 0x30, u8 **)[i] & 0x4000000)) i++;
    return i;
}

/* the swap lambda 0x180151c3c: origins (8 bytes), layout pointers and, when present, the glyph
 * index array (+0x40) */
void gh_PartitionAliasedSwap(void **ctx, u32 i, u32 j) {
    u8 *o = *(u8 **)ctx[1];
    u64 t = AT(o, (u64)j * 8, u64);
    u32 ix = AT(o, (u64)i * 8, u32), iy = AT(o, (u64)i * 8 + 4, u32);
    AT(o, (u64)i * 8, u64) = t;
    AT(o, (u64)j * 8, u32) = ix;
    AT(o, (u64)j * 8 + 4, u32) = iy;
    u8 *self = (u8 *)ctx[0];
    u64 *L = AT(self, 0x30, u64 *);
    u64 lj = L[j], li = L[i];
    L[i] = lj;
    L[j] = li;
    u16 *g = AT(self, 0x40, u16 *);
    if (g != NULL) {
        u16 gj = g[j], gi = g[i];
        g[i] = gj;
        g[j] = gi;
    }
}

/* PartitionArray<...> 0x180151b8c: moves the glyphs at odd pixel phase ((x ^ y) & 1 of their
 * integer positions in `self`+0x38) to the front, swapping positions, layouts and glyph indices
 * of `arrays`; returns their count. */
u32 gh_PartitionOddPhase(u32 count, u8 *self, u8 *arrays) {
    u32 i = 0, j = count - 1;
    if (j != 0) {
        do {
            const u8 *P = AT(self, 0x38, const u8 *);
            if ((P[(u64)i * 8 + 4] ^ P[(u64)i * 8]) & 1) {
                i++;
            } else {
                u32 jj = j--;
                if ((P[(u64)jj * 8 + 4] ^ P[(u64)jj * 8]) & 1) {
                    u64 *q = AT(arrays, 0x38, u64 *);
                    u64 qj = q[jj], qi = q[i];
                    q[i] = qj;
                    q[jj] = qi;
                    q = AT(arrays, 0x30, u64 *);
                    qj = q[jj];
                    qi = q[i];
                    q[i] = qj;
                    q[jj] = qi;
                    u16 *g = AT(arrays, 0x40, u16 *);
                    u16 gj = g[jj], gi = g[i];
                    g[i] = gj;
                    g[jj] = gi;
                    i++;
                }
            }
        } while (i < j);
    }
    if (i == j) {
        const u8 *P = AT(self, 0x38, const u8 *);
        if ((P[(u64)i * 8 + 4] ^ P[(u64)i * 8]) & 1) i++;
    }
    return i;
}

/* SSE scalar arithmetic with the original's operand order: with two NaN operands the result is
 * the first (destination) one, so the order is part of the result. */
#if defined(__x86_64__) || defined(_M_X64)
static inline float f_add(float a, float b) { __asm__("addss %1, %0" : "+x"(a) : "x"(b)); return a; }
static inline float f_sub(float a, float b) { __asm__("subss %1, %0" : "+x"(a) : "x"(b)); return a; }
static inline float f_mul(float a, float b) { __asm__("mulss %1, %0" : "+x"(a) : "x"(b)); return a; }
static inline float f_div(float a, float b) { __asm__("divss %1, %0" : "+x"(a) : "x"(b)); return a; }
/* the CRT sqrtf the original calls gives sqrtss's results (-0, the default NaN, quieted NaNs) */
static inline float f_sqrt(float a) { __asm__("sqrtss %0, %0" : "+x"(a)); return a; }
#else
/* other architectures: IEEE single precision gives the same values; only which of two NaN
 * operands propagates (and the NaN's sign) may differ */
#include <math.h>
static inline float f_add(float a, float b) { return a + b; }
static inline float f_sub(float a, float b) { return a - b; }
static inline float f_mul(float a, float b) { return a * b; }
static inline float f_div(float a, float b) { return a / b; }
static inline float f_sqrt(float a) { return sqrtf(a); }
#endif

static int is_zero_tol(float v) { return 1.52587890625e-05f > v && v > -1.52587890625e-05f; }

static float fabs_bits(float v) {
    union { float f; u32 u; } x = { v };
    x.u &= 0x7fffffffu;
    return x.f;
}

/* GetTransformMaxScaleFactor 0x180075424: the largest singular value of the 2x2 part,
 * sqrt((p + q + sqrt((p - q)^2 + 4 c^2)) / 2) with p, q the squared row norms and c their dot
 * product; for a diagonal matrix max(|m22|, |m11|) */
float gh_GetTransformMaxScaleFactor(const float *m) {
    if (m == NULL) return 1.0f;
    if (m[1] == 0.0f && m[2] == 0.0f) {
        float a = fabs_bits(m[3]), b = fabs_bits(m[0]);
        return a > b ? a : b;
    }
    float p = f_add(f_mul(m[0], m[0]), f_mul(m[1], m[1]));
    float q = f_add(f_mul(m[3], m[3]), f_mul(m[2], m[2]));
    float c = f_add(f_mul(m[3], m[1]), f_mul(m[2], m[0]));
    float d = f_sub(p, q);
    float s = f_sqrt(f_add(f_mul(f_mul(c, 4.0f), c), f_mul(d, d)));
    return f_sqrt(f_mul(f_add(f_add(q, p), s), 0.5f));
}

/* MetricsConverter::MetricsConverter 0x180049674: { scale, emSize / (scale * designUnitsPerEm) }.
 * GDI-compatible measuring with a uniform (or 90-degree) transform scales by
 * emSize * pixelsPerDip * |m| / designUnitsPerEm, so DesignToClientUnits rounds to pixels. */
float *gh_MetricsConverter_ctor(float *mc, const u8 *face, u8 unused, i32 measuring, float em, float ppd,
                                const float *m) {
    (void)unused;
    float du = (float)(i32)(u16)AT(AT(face, 0, const u8 *), 0x18, u32);
    float scale = 1.0f;
    if (measuring != 0) {
        float s = 1.0f, a, b;
        int ok = 1;
        if (m != NULL) {
            if (is_zero_tol(m[1])) {
                if (!is_zero_tol(m[2])) ok = 0;
                a = m[0];
                b = m[3];
            } else {
                if (!is_zero_tol(m[0]) || !is_zero_tol(m[3])) ok = 0;
                a = m[1];
                b = m[2];
            }
            if (ok) {
                a = fabs_bits(a);
                b = fabs_bits(b);
                if (!is_zero_tol(a - b)) ok = 0;
                s = a;
            }
        }
        if (ok) scale = f_div(f_mul(f_mul(em, ppd), s), du);
    }
    mc[0] = scale;
    mc[1] = f_div(em, f_mul(scale, du));
    return mc;
}

/* MetricsConverter::DesignToClientUnits 0x180049428 */
float gh_DesignToClientUnits(const float *mc, i32 v) {
    float f = (float)v;
    if (!(mc[0] == 1.0f)) {
        f = f_mul(f, mc[0]);
        if (8388608.0f > fabs_bits(f)) {
            i32 i = _mm_cvtss_si32(_mm_set_ss(f));
            if (f_sub((float)i, f) == -0.5f) i++;
            f = (float)i;
        }
    }
    return f_mul(f, mc[1]);
}

typedef float *(*mc_ctor_fn)(float *, const u8 *, u8, i32, float, float, const float *);
typedef float (*dtc_fn)(const float *, i32);

/* ComputeGlyphOrigins 0x180049844: pen positions of a glyph run (PointF per glyph) from the
 * given advances, or from the font's (design advances / advance heights through a
 * MetricsConverter) for RTL, sideways or advance-less runs; then glyph offsets and, if asked,
 * the transform. *simple stays true only for a run without offsets, sideways or a rotating
 * transform. */
void gh_ComputeGlyphOrigins(void *ctx, const u8 *face, float em, float ppd, const float *m, i32 measuring,
                            const u16 *indices, const float *adv, const float *offs, float *org, u32 count,
                            u8 sideways, u8 rtl, u8 transform, float x, float y, u8 *simple) {
    *simple = 1;
    if (count == 0) return;
    float pen = x;
    if (!rtl && !sideways && adv != NULL) {
        for (u32 i = 0; i < count; i++) {
            org[2 * i] = pen;
            org[2 * i + 1] = y;
            pen = f_add(pen, adv[i]);
        }
    } else {
        float mc[2];
        dtc_fn dtc = ORIG(dtc_fn, RVA_DesignToClientUnits);
        ORIG(mc_ctor_fn, RVA_MetricsConverter_ctor)(mc, face, 0, measuring, em, ppd, m);
        if (sideways) {
            *simple = 0;
            u8 *gm = calloc(count, 0x1c);   /* std::vector<DWRITE_GLYPH_METRICS>(count) */
            if (gm == NULL) IDWR_FATAL("glyphrun_helpers.c:830");
            ORIG(void (*)(const u8 *, void *, float, float, const float *, i32, u8, const u16 *, u32, u8 *),
                 RVA_FontFace_GetGlyphMetrics)(face, ctx, em, ppd, m, measuring, sideways, indices, count, gm);
            for (u32 i = 0; i < count; i++) {
                const u8 *g = gm + (u64)i * 0x1c;
                float cur = pen;
                float aw = dtc(mc, AT(g, 4, i32));      /* advanceWidth */
                float ah = dtc(mc, AT(g, 0x10, i32));   /* advanceHeight */
                float a = adv != NULL ? adv[i] : ah;
                if (!rtl) {
                    pen = f_add(pen, a);
                } else {
                    cur = f_sub(pen, ah);
                    pen = f_sub(pen, a);
                }
                org[2 * i] = cur;
                float vo = dtc(mc, AT(g, 0x18, i32));   /* verticalOriginY */
                org[2 * i] = f_add(vo, org[2 * i]);
                org[2 * i + 1] = f_add(f_mul(aw, 0.5f), y);
            }
            free(gm);
        } else {
            i32 *av = calloc(count, 4);              /* std::vector<int>(count) */
            if (av == NULL) IDWR_FATAL("glyphrun_helpers.c:853");
            ORIG(void (*)(const u8 *, void *, float, float, const float *, i32, u8, u32, const u16 *, i32 *),
                 RVA_FontFace_GetGlyphAdvances)(face, ctx, em, ppd, m, measuring, sideways, count, indices, av);
            for (u32 i = 0; i < count; i++) {
                float a = dtc(mc, av[i]);
                if (!rtl) {
                    org[2 * i] = pen;
                    pen = f_add(pen, a);
                } else {
                    org[2 * i] = f_sub(pen, a);
                    if (adv != NULL) a = adv[i];
                    pen = f_sub(pen, a);
                }
                org[2 * i + 1] = y;
            }
            free(av);
        }
    }
    if (offs != NULL) {
        *simple = 0;
        for (u32 i = 0; i < count; i++) {
            if (rtl) org[2 * i] = f_sub(org[2 * i], offs[2 * i]);
            else org[2 * i] = f_add(offs[2 * i], org[2 * i]);
            org[2 * i + 1] = f_sub(org[2 * i + 1], offs[2 * i + 1]);
        }
    }
    if (m != NULL && transform) {
        if (!(m[1] == 0.0f && m[2] == 0.0f)) *simple = 0;
        for (u32 i = 0; i < count; i++) {
            float ox = org[2 * i], oy = org[2 * i + 1];
            org[2 * i] = f_add(f_add(f_mul(oy, m[2]), f_mul(ox, m[0])), m[4]);
            org[2 * i + 1] = f_add(f_add(f_mul(ox, m[1]), f_mul(oy, m[3])), m[5]);
        }
    }
}

/* ===================================================================== exports =========== */

#ifdef IDWR_STANDALONE
#define EXPORT_RAX(name, impl) _Static_assert(sizeof(&name) == sizeof(&impl), #name)
#define EXPORT_VOID(name, impl) EXPORT_RAX(name, impl)
#define EXPORT_XMM0(name, impl) EXPORT_RAX(name, impl)
#else
/* save RAX, RCX, RDX, R8-R11, XMM0-XMM5; copy 16 stack arguments; call; restore all of them
 * but the result register */
#define THUNK_HEAD(name, impl)                                                          \
    ".text\n.globl " #name "\n.def " #name "; .scl 2; .type 32; .endef\n"               \
    ".p2align 4\n" #name ":\n.seh_proc " #name "\n"                                     \
    "subq $0x148, %rsp\n.seh_stackalloc 0x148\n.seh_endprologue\n"                      \
    "movq %rcx, 0xa0(%rsp)\nmovq %rdx, 0xa8(%rsp)\nmovq %r8, 0xb0(%rsp)\n"              \
    "movq %r9, 0xb8(%rsp)\nmovq %r10, 0xc0(%rsp)\nmovq %r11, 0xc8(%rsp)\n"              \
    "movq %rax, 0xd0(%rsp)\n"                                                           \
    "movaps %xmm0, 0xe0(%rsp)\nmovaps %xmm1, 0xf0(%rsp)\nmovaps %xmm2, 0x100(%rsp)\n"   \
    "movaps %xmm3, 0x110(%rsp)\nmovaps %xmm4, 0x120(%rsp)\nmovaps %xmm5, 0x130(%rsp)\n" \
    "movq 0x170(%rsp), %rax\nmovq %rax, 0x20(%rsp)\nmovq 0x178(%rsp), %rax\nmovq %rax, 0x28(%rsp)\n" \
    "movq 0x180(%rsp), %rax\nmovq %rax, 0x30(%rsp)\nmovq 0x188(%rsp), %rax\nmovq %rax, 0x38(%rsp)\n" \
    "movq 0x190(%rsp), %rax\nmovq %rax, 0x40(%rsp)\nmovq 0x198(%rsp), %rax\nmovq %rax, 0x48(%rsp)\n" \
    "movq 0x1a0(%rsp), %rax\nmovq %rax, 0x50(%rsp)\nmovq 0x1a8(%rsp), %rax\nmovq %rax, 0x58(%rsp)\n" \
    "movq 0x1b0(%rsp), %rax\nmovq %rax, 0x60(%rsp)\nmovq 0x1b8(%rsp), %rax\nmovq %rax, 0x68(%rsp)\n" \
    "movq 0x1c0(%rsp), %rax\nmovq %rax, 0x70(%rsp)\nmovq 0x1c8(%rsp), %rax\nmovq %rax, 0x78(%rsp)\n" \
    "movq 0x1d0(%rsp), %rax\nmovq %rax, 0x80(%rsp)\nmovq 0x1d8(%rsp), %rax\nmovq %rax, 0x88(%rsp)\n" \
    "movq 0x1e0(%rsp), %rax\nmovq %rax, 0x90(%rsp)\nmovq 0x1e8(%rsp), %rax\nmovq %rax, 0x98(%rsp)\n" \
    "movq 0xd0(%rsp), %rax\n"                                                           \
    "callq " #impl "\n"                                                                 \
    "movq 0xa0(%rsp), %rcx\nmovq 0xa8(%rsp), %rdx\nmovq 0xb0(%rsp), %r8\n"              \
    "movq 0xb8(%rsp), %r9\nmovq 0xc0(%rsp), %r10\nmovq 0xc8(%rsp), %r11\n"              \
    "movaps 0xf0(%rsp), %xmm1\nmovaps 0x100(%rsp), %xmm2\nmovaps 0x110(%rsp), %xmm3\n"  \
    "movaps 0x120(%rsp), %xmm4\nmovaps 0x130(%rsp), %xmm5\n"
#define THUNK_TAIL(name)                                                                \
    "addq $0x148, %rsp\nretq\n.seh_endproc\n"                                           \
    ".section .drectve,\"yn\"\n.ascii \" -export:" #name "\"\n.text\n"

/* integer or pointer result in RAX: XMM0 is restored */
#define EXPORT_RAX(name, impl) \
    __asm__(THUNK_HEAD(name, impl) "movaps 0xe0(%rsp), %xmm0\n" THUNK_TAIL(name))
/* no result: RAX and XMM0 are restored */
#define EXPORT_VOID(name, impl) \
    __asm__(THUNK_HEAD(name, impl) "movaps 0xe0(%rsp), %xmm0\nmovq 0xd0(%rsp), %rax\n" THUNK_TAIL(name))
/* float result in XMM0: RAX is restored */
#define EXPORT_XMM0(name, impl) \
    __asm__(THUNK_HEAD(name, impl) "movq 0xd0(%rsp), %rax\n" THUNK_TAIL(name))
#endif

EXPORT_VOID(tt_FillGlyphBitmaps, gh_FillGlyphBitmaps);
EXPORT_RAX(tt_IsRectEmpty, gh_IsRectEmpty);
EXPORT_RAX(tt_GetRasterTypeAttributes, gh_GetRasterTypeAttributes);
EXPORT_VOID(tt_MergeGlyph1Bit, gh_MergeGlyph1Bit);
EXPORT_VOID(tt_MergeGlyph4x4, gh_MergeGlyph4x4);
EXPORT_VOID(tt_MergeScanLine, gh_MergeScanLine);
EXPORT_RAX(tt_CachedBitmapReader_u16_MoveNextY, gh_MoveNextY_u16);
EXPORT_RAX(tt_CachedBitmapReader_u16_MoveNextX, gh_MoveNextX_u16);
EXPORT_RAX(tt_CachedBitmapReader_u8_MoveNextY, gh_MoveNextY_u8);
EXPORT_RAX(tt_GetRenderingModeAttributes, gh_GetRenderingModeAttributes);
EXPORT_XMM0(tt_GetTransformMaxScaleFactor, gh_GetTransformMaxScaleFactor);
EXPORT_VOID(tt_ComputeGlyphOrigins, gh_ComputeGlyphOrigins);
EXPORT_RAX(tt_MetricsConverter_ctor, gh_MetricsConverter_ctor);
EXPORT_XMM0(tt_MetricsConverter_DesignToClientUnits, gh_DesignToClientUnits);
EXPORT_RAX(tt_PartitionAliased, gh_PartitionAliased);
EXPORT_VOID(tt_PartitionAliasedSwap, gh_PartitionAliasedSwap);
EXPORT_RAX(tt_PartitionOddPhase, gh_PartitionOddPhase);
EXPORT_VOID(tt_DownSample2x2, gh_DownSample2x2);
EXPORT_RAX(tt_CountBits, gh_CountBits);
EXPORT_RAX(tt_Align, gh_Align);
EXPORT_RAX(tt_ConstrainGlyphIndex, gh_ConstrainGlyphIndex);
EXPORT_RAX(tt_HashBlob, gh_HashBlob);
EXPORT_RAX(tt_HashWords, gh_HashWords);
