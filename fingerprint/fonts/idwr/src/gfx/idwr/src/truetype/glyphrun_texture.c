/*
 * DirectWrite alpha-texture creation from the cached glyph bitmaps, ported from
 * DWrite.dll 10.0.26100.9711 so it is bit-identical. Path:
 *   IDWriteGlyphRunAnalysis::CreateAlphaTexture
 *     -> DWriteGlyphRunAnalysis::CreateAlphaTexture              0x180015dd0
 *          ALIASED_1x1  -> GlyphRunAnalysis::CreateAlphaTextureGrayscale 0x180017528
 *          CLEARTYPE_3x1-> GlyphRunAnalysis::CreateAlphaTexture3x1       0x180015f04
 * It merges each glyph's cached overscale 1-bit/compressed bitmap into one run bitmap (OR,
 * never additive), filters subsample counts to 8-bit coverage, then draws the aliased /
 * embedded-strike glyphs over the result.
 *
 * Every call between this group's functions goes through the original entry (ORIG by RVA) so
 * the oracle's per-function swap keeps exact semantics. Callees outside the group
 * (FillGlyphBitmaps, MergeScanLine, CachedBitmapReader::MoveNextX/Y, the vector reallocator,
 * SafeIntOnOverflow) are also reached through ORIG; MergeGlyph1Bit/MergeGlyph4x4 are the
 * original dispatchers whose leaves are the hooked merges here.
 *
 * ---- struct layouts read from this path's assembly ----
 *
 * GlyphBitmapLayout (per-glyph cache entry; header 0x10 bytes, then packed bitmap):
 *   +0x00 u32 packed: bits0..25 = compressed byte count (uncompressed / uchar-RLE),
 *                     or (packed>>1)&0x1ffffff = ushort count (ushort-RLE);
 *                     bits26..31 = kind: &0x10 uncompressed, &0x08 ushort-RLE, else uchar-RLE
 *   +0x04, +0x0c      not read by this path
 *   +0x08 u16 width   (in subsamples: already x-overscaled)
 *   +0x0a u16 height  (in subsample rows: already y-overscaled)
 *   +0x10     bitmap data, one of:
 *   - uncompressed 1-bit: height rows of ceil(width/32) u32 words, bit 0 = leftmost; the merges
 *     and draws check packed&0x3ffffff == height*words*4 (else InvalidCacheDataException).
 *   - uncompressed 4x4 (grayscale 4x4 merge): ceil(height/4) rows of
 *     ((roundup4(width)*4+31)/8 & ~3) bytes; each u16 is one output pixel, bit 4*row + col
 *     (row, col = 0..3 inside the pixel).
 *   - RLE (uchar or ushort elements T): a sequence of scanline records. Header h: if h&1 and
 *     another element follows, repeat = h>>1 (extra copies of this scanline) and the next element
 *     n gives the length n>>1; otherwise the length is h>>1. The record body (length elements,
 *     clamped to the data end) is alternating deltas: start = x0 + d0 (relative to the previous
 *     end), end = start + d1; a missing last d1 means "to the glyph's right edge". Each [start,
 *     end) span is ink. A repeated record is replayed for the following repeat scanlines.
 *
 * GlyphRunAnalysis "ga" (the object CreateAlphaTexture passes as this+8):
 *   +0x28 u32 glyphCount   +0x30 GlyphBitmapLayout** layouts   +0x38 u64* positions(lo=x,hi=y)
 *   +0x48 u32 overlayStart (glyphs [overlayStart,glyphCount) are the aliased overlay)
 *   +0x70 std::vector<u32> runBitmap { +0x70 _First, +0x78 _Last, +0x80 _End }
 *   +0x88 u32 attributes (byte0 lo nib = x overscale, hi nib = y overscale;
 *                         bit17 grayscale, bit22 grayscale vertical AA, bits26..31 raster type)
 *
 * CachedBitmapReader<T> (0x38 bytes, on the caller's stack; driven by the original MoveNext*):
 *   +0x00 x0  +0x04 xLimit  +0x08 f08  +0x0c f0c  +0x10 y  +0x14 runRem
 *   +0x18 cur +0x20 rowEnd  +0x28 cursor +0x30 dataEnd
 */
#include <string.h>
#include "portable.h"   /* the SSE intrinsics, or stand-ins elsewhere */
#include "tt_state.h"
#include "orig.h"

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

#include "glyphrun_tables.h"

struct ClearTypeFilter { u8 m0, m1, pad[6]; const u16 *table; };
static const struct ClearTypeFilter g_classicPlattFilterRGB = { 2, 2, {0,0,0,0,0,0}, g_classicPlattFilterTable };

struct FilterTarget { u32 f0, width, height; u64 rowStride; u8 *cur; u8 *end; };

typedef struct { i32 x0, xLimit, f08, f0c, y, runRem; const u8 *cur, *rowEnd, *cursor, *dataEnd; } Reader;

#define BASE_AT(rva) ((u8 *)idwr_orig(rva))
#define O_FillGlyphBitmaps  ((void(*)(u32,u32,void*,void*,void*,u32,u32,void*))BASE_AT(0x191ac))
#define O_MergeScanLine     ((void(*)(u32*,u32,u32,u32,u32,u32,u16))BASE_AT(0x155b0))
#define O_MoveNextY_u16     ((char(*)(Reader*,u32))BASE_AT(0x15518))
#define O_MoveNextX_u16     ((char(*)(Reader*))BASE_AT(0xd11ec))
#define O_MoveNextY_u8      ((char(*)(Reader*,u32))BASE_AT(0xac460))
#define O_Resize            ((void(*)(void*,u64,void*))BASE_AT(0xf7174))
#define O_SafeInt           ((void(*)(void))BASE_AT(0xfa5cc))

#define O_MergeGrayscale    ((u8*(*)(void*,void*,u32*,u32*,u32*))BASE_AT(0x15a24))
#define O_FillRunBitmap     ((void(*)(void*,u32,void*,u32,u32,void*))BASE_AT(0x158bc))
#define O_ApplyGray         ((void(*)(void*,u8*,u32,u32,u32,u8*))BASE_AT(0x15bbc))
#define O_ApplyFilter1      ((void(*)(struct FilterTarget*,u32,i16,const struct ClearTypeFilter*,const void*,u32,u32,u64))BASE_AT(0x172e0))
#define O_ApplyFilter5      ((void(*)(struct FilterTarget*,u32,i16,const struct ClearTypeFilter*,const void*,u32,u32,u64))BASE_AT(0x16720))
#define O_MergeUncomp1Bit   ((void(*)(const u8*,int,int,u32,u32,u32,u32*))BASE_AT(0x16ef0))
#define O_MergeComp1Bit_u16 ((void(*)(const u8*,int,int,u32,u32,u32,u32*))BASE_AT(0x14f94))
#define O_DrawAliasedLMR    ((void(*)(const u8*,int,int,const void*,u8*,u32,u32))BASE_AT(0x15698))
#define O_DrawCU8U8         ((void(*)(const u8*,int,int,u8,u8*,u32,u32))BASE_AT(0xac344))
#define O_DrawCU8U16        ((void(*)(const u8*,int,int,u8,u8*,u32,u32))BASE_AT(0x10a1a4))
#define O_DrawUU8           ((void(*)(const u8*,int,int,u8,u8*,u32,u32))BASE_AT(0xd6c58))
#define O_CreateGray        ((void(*)(void*,void*,u8*,u32))BASE_AT(0x17528))
#define O_Create3x1         ((void(*)(void*,void*,u8*,u32))BASE_AT(0x15f04))
#define O_DrawUncLMR        ((void(*)(const u8*,int,int,const void*,u8*,u32,u32))BASE_AT(0x15730))
#define O_DrawCLMRU8        ((void(*)(const u8*,int,int,const void*,u8*,u32,u32))BASE_AT(0xac1ec))
#define O_PartialMerge      ((void(*)(u32,u16*,u32,const u16*,u32,u32,i16,u16))BASE_AT(0x19c40))
#define O_DrawCLMRU16       ((void(*)(const u8*,int,int,const void*,u8*,u32,u32))BASE_AT(0x147b6c))

/* SafeInt overflow checks of the original: call its SafeIntOnOverflow (throws). */
static void TRAP(void) { O_SafeInt(); }

/* Error paths reachable through the public API (too-small buffer, CLEARTYPE_3x1 on a
 * grayscale analysis, corrupt cache) raise exactly the original exception: same class,
 * HRESULT, tag and line, through the original Exception ctor / LogAndThrow<T>, so the
 * original's catch in DWriteGlyphRunAnalysis::CreateAlphaTexture maps it to the same HRESULT.
 * SafeInt overflows call the original SafeIntOnOverflow, FailAssert the original FailAssert. */
#define THROW_ARG    0xfb6cc   /* LogAndThrow<ArgumentException> */
#define THROW_OS     0xf9848   /* LogAndThrow<OSException> */
#define THROW_NOTSUP 0x10bedc  /* LogAndThrow<NotSupportedException> */
#define THROW_CACHE  0x10bf90  /* LogAndThrow<InvalidCacheDataException> */
static void throw_exc(u32 rva, u32 hr, u64 tag, u32 line, const void *layout, int captureOnly) {
    __attribute__((aligned(16))) u8 e[32];
    memset(e, 0, sizeof e);
    if (captureOnly) {                     /* the "dword + CaptureStack" form */
        *(u32 *)e = hr;
        ((void (*)(u32, u32))(BASE_AT(0x1cf60)))(hr, 0);
    } else {
        ((void (*)(void *, u32, u32))(BASE_AT(0xaa7f0)))(e, hr, 0);   /* Exception::Exception */
    }
    if (layout) *(const void **)(e + 8) = layout;   /* InvalidCacheDataException keeps the entry */
    ((void (*)(void *, u64, u32))(BASE_AT(rva)))(e, tag, line);
}
#define FAILASSERT(n) ((void (*)(u32, const char *))(BASE_AT(0x10cb50)))((n), 0)


/* tagRECT { i32 left, top, right, bottom } */
typedef struct { i32 left, top, right, bottom; } RECT4;

/* ================================================================= merge leaves ======== */

/* 0x180016ef0  MergeUncompressedGlyph1Bit */
TT_EXPORT void tt_MergeUncompressedGlyph1Bit(const u8 *p1, int x, int y, u32 clipW, u32 clipH,
                                             u32 stride, u32 *dst) {
    u32 words = (u32)(*(const u16 *)(p1 + 8) + 0x1f) >> 5;
    if ((*(const u32 *)p1 & 0x3ffffff) != (u32)(*(const u16 *)(p1 + 10)) * words * 4u) { throw_exc(THROW_CACHE, 0x88985007, 0x7461645f6d62ULL, 0x281, p1, 0); return; }
    u32 right = (u32)(*(const u16 *)(p1 + 8)) + (u32)x;
    u32 bottom = (u32)(*(const u16 *)(p1 + 10)) + (u32)y;
    int x0 = (x < 0) ? 0 : x;
    if ((i32)clipW < (i32)right) right = clipW;
    int y0 = (y < 0) ? 0 : y;
    if ((i32)clipH < (i32)bottom) bottom = clipH;
    if (!(y0 < (i32)bottom && x0 < (i32)right)) return;

    u32 t = (u32)x & 0x8000001f;
    if ((i32)t < 0) t = ((t - 1) | 0xffffffe0u) + 1;
    i32 d = x - (i32)t;
    if ((i32)t < 0) d = (x - (i32)t) - 0x20;
    int skipSrc = 0;
    u32 dstWord = (u32)((i32)(d + ((d >> 31) & 0x1f)) >> 5);
    if ((i32)dstWord < 0) { skipSrc = -(i32)dstWord; dstWord = 0; }
    u32 srcWords = (clipW + 0x1f) >> 5;
    u32 n = srcWords - dstWord;
    if ((i32)(words - (u32)skipSrc) <= (i32)n) n = words - (u32)skipSrc;
    u32 lastWordIdx = n + dstWord;
    if (stride < lastWordIdx * 4u) { throw_exc(THROW_ARG, 0x80070057, 0x6d626772656dULL, 0x29f, 0, 0); return; }

    i64 nWords = (i32)n;
    const u8 *src = p1 + ((u64)((u32)(y0 - y) * words) + (u32)skipSrc) * 4u + 0x10;
    u8 *drow = (u8 *)dst + (u64)dstWord * 4u + (u64)((u32)y0 * stride);
    i16 sh = (i16)((i16)x - (i16)d);
    u32 rows = bottom - (u32)y0;

    if (sh == 0) {
        for (u32 r = 0; r < rows; r++) {
            for (i64 j = 0; j < nWords; j++)
                *(u32 *)(drow + j * 4) |= *(const u32 *)(src + j * 4);
            src += (u64)words * 4u;
            drow += stride;
        }
    } else {
        u8 lo = (u8)sh, hi = (u8)(0x20 - (int)lo);
        for (u32 r = 0; r < rows; r++) {
            u32 carry = (skipSrc == 0) ? 0 : (*(const u32 *)(src - 4) >> (hi & 0x1f));
            i64 off = src - drow;
            u8 *pg = drow;
            for (i64 j = 0; j < nWords; j++) {
                u32 w = *(const u32 *)(pg + off);
                *(u32 *)pg |= (w << (lo & 0x1f)) | carry;
                carry = w >> (hi & 0x1f);
                pg += 4;
            }
            if (carry != 0 && (i64)(i32)lastWordIdx < (i64)(u64)srcWords)
                *(u32 *)(drow + nWords * 4) |= carry;
            src += (u64)words * 4u;
            drow += stride;
        }
    }
}

/* set subsample bits [lo,hi) of one dword-row of the 1-bit run bitmap */
static void set_span_1bit(u8 *dst, u64 rowOff, u32 lo, u32 hi) {
    u32 w = lo >> 5, wend = hi >> 5;
    u32 maskLo = 0xffffffffu << (lo & 0x1f);
    u32 maskHi = 0xffffffffu >> ((0x20 - (hi & 0x1f)) & 0x1f);
    if (w == wend) {
        *(u32 *)(dst + rowOff + (u64)w * 4) |= maskHi & maskLo;
    } else {
        *(u32 *)(dst + rowOff + (u64)w * 4) |= maskLo;
        w++;
        if (w < wend) memset(dst + rowOff + (u64)w * 4, -1, (u64)(wend - w) << 2);
        if ((hi & 0x1f) != 0) *(u32 *)(dst + rowOff + (u64)(hi >> 5) * 4) |= maskHi;
    }
}

/* 0x180014f94  MergeCompressedGlyph1Bit<unsigned short>: walks the RLE stream itself after one
 * MoveNextY positions the first non-negative scanline. */
TT_EXPORT void tt_MergeCompressedGlyph1Bit_u16(const u8 *p1, int x, int y, u32 clipW, u32 clipH,
                                               u32 stride, u32 *dst) {
    if ((stride >> 2) << 5 < clipW) { throw_exc(THROW_ARG, 0x80070057, 0x6d626772656dULL, 0x4c8, 0, 0); return; }
    Reader rd;
    rd.x0 = x;
    rd.xLimit = (i32)((u32)(*(const u16 *)(p1 + 8)) + (u32)x);
    rd.f08 = 0; rd.f0c = 0;
    rd.y = y - 1;
    rd.runRem = 0;
    rd.cur = rd.rowEnd = rd.cursor = p1 + 0x10;
    rd.dataEnd = (p1 + 0x10) + (u64)((*(const u32 *)p1 >> 1) & 0x1ffffff) * 2u;

    do { if (!O_MoveNextY_u16(&rd, 1)) return; } while ((i32)rd.y < 0);

    const u8 *cursor = rd.cursor;
    const u8 *rowEnd = rd.rowEnd;
    const u8 *cur    = rd.cur;
    u32 accX   = (u32)rd.f0c;
    u32 runRem = (u32)rd.runRem;
    i32 yy     = rd.y;

    for (;;) {
        for (;;) {
            if (clipH <= (u32)yy) return;
            u64 rowOff = (u64)((u32)yy * stride);
            while (cursor != rowEnd) {
                u32 a = *(const u16 *)cursor + accX;
                const u8 *nx = cursor + 2;
                accX = (u32)rd.xLimit;
                if (nx != rowEnd) { accX = (u32)(*(const u16 *)nx) + a; nx = cursor + 4; }
                u32 lo = ((i32)a < 0) ? 0 : a;
                u32 hi = accX;
                if ((i32)clipW < (i32)accX) hi = clipW;
                cursor = nx;
                if ((i32)lo < (i32)hi) set_span_1bit((u8 *)dst, rowOff, lo, hi);
            }
            yy++;
            accX = (u32)rd.x0;
            if (runRem == 0) break;
            runRem--;
            cursor = cur;
        }
        if (rowEnd == rd.dataEnd) break;
        {
            u16 w0 = *(const u16 *)rowEnd;
            u32 span = w0;
            cur = rowEnd + 2;
            if ((w0 & 1) != 0 && cur < rd.dataEnd) { span = *(const u16 *)cur; runRem = (u32)(w0 >> 1); cur = rowEnd + 4; }
            i64 room = (rd.dataEnd - cur) >> 1;
            u64 take = (u64)(span >> 1);
            if ((i64)(u64)(span >> 1) > room) take = (u64)room;
            cursor = cur;
            rowEnd = cur + take * 2;
        }
    }
}

/* 0x180019c40  PartialMergeUncompressedGlyph4x4_Shift<2>: straight OR of src into dst,
 * rows x cols ushorts, byte strides dstStride / srcStride. (Collapsed from the vectorized
 * original; value-identical.) */
TT_EXPORT void tt_PartialMergeUncompressedGlyph4x4_Shift2(u32 dstStride, u16 *dst, u32 srcStride,
                                                          const u16 *src, u32 cols, u32 rows,
                                                          i16 unused1, u16 unused2) {
    (void)unused1; (void)unused2;
    for (u32 r = 0; r < rows; r++) {
        for (u32 c = 0; c < cols; c++) dst[c] |= src[c];
        dst = (u16 *)((u8 *)dst + dstStride);
        src = (const u16 *)((const u8 *)src + srcStride);
    }
}

/* 0x180017c18  MergeUncompressedGlyph4x4 (vectorized inner loops collapsed to scalar OR, which
 * is value-identical to the original's SSE unrolling). */
TT_EXPORT void tt_MergeUncompressedGlyph4x4(const u8 *p1, int x, int y, u32 clipW, u32 clipH,
                                            u32 stride, u16 *dst) {
    u32 gw = (u32)((*(const u16 *)(p1 + 8) + 3) & 0xfffffffc);
    u32 tmp = gw * 4;
    u64 ck = (u64)tmp + 0x1f;
    if (ck < tmp) { TRAP(); return; }
    u32 srcStride = (u32)(ck >> 3) & 0x1ffffffc;           /* bytes per glyph row */
    if ((*(const u32 *)p1 & 0x3ffffff) != srcStride * ((u32)(*(const u16 *)(p1 + 10) + 3) >> 2)) { throw_exc(THROW_CACHE, 0x88985007, 0x7461645f6d62ULL, 0x412, p1, 0); return; }

    int iVar1 = (int)gw + x;
    int iVar2 = (int)((*(const u16 *)(p1 + 10) + 3) & 0xfffffffc) + y;

    for (int qy = 0; qy < 2; qy++) {
        for (int qx = 0; qx < 2; qx++) {
            u32 phx = (u32)x & 3; if (qx != 0) phx = ((u32)x & 3) - 4;
            u32 phy = (u32)y & 3; if (qy != 0) phy = ((u32)y & 3) - 4;
            i16 sh = (i16)((i16)phy * 4 + (i16)phx);
            u16 mx = g_merge4x4MaskX[(i32)phx + 4];
            u16 my = g_merge4x4MaskY[(i32)phy + 4];
            u16 m = (u16)(my & mx);
            if (m == 0) continue;

            int sx = (qx == 0) ? x : x + 3;         sx >>= 2;
            int sxSkip = 0; if (sx < 0) { sxSkip = -sx; sx = 0; }
            int sy = (qy == 0) ? y : y + 3;         sy >>= 2;
            int sySkip = 0; if (sy < 0) { sySkip = -sy; sy = 0; }
            int dxRaw = (qx == 0) ? iVar1 : iVar1 + 3;  int dx = dxRaw >> 2;
            if ((int)(clipW >> 4) < dx) dx = (int)(clipW >> 4);
            int dyRaw = (qy == 0) ? iVar2 : iVar2 + 3;  int dy = dyRaw >> 2;
            if ((int)clipH < dy) dy = (int)clipH;
            if (!(sx < dx && sy < dy)) continue;

            u32 cols = (u32)(dx - sx);
            u32 drows = (u32)(dy - sy);
            u16 *drow = (u16 *)((u8 *)dst + (u64)sx * 2 + (u64)((u32)sy * stride));
            const u8 *srow = p1 + ((u64)sxSkip * 2 + (u64)((u32)sySkip * srcStride)) + 0x10;

            if (sh == 0) {
                O_PartialMerge(stride, drow, srcStride, (const u16 *)srow, cols, drows, 0, 0);
            } else if (sh < 0) {
                u32 rsh = (u32)(-(i32)sh) & 0x1f;
                for (u32 r = 0; r < drows; r++) {
                    const u16 *s = (const u16 *)srow;
                    u16 *d = drow;
                    for (u32 c = 0; c < cols; c++) d[c] |= (u16)((u16)s[c] >> rsh) & m;
                    drow = (u16 *)((u8 *)drow + stride);
                    srow += srcStride;
                }
            } else {
                u32 lsh = (u32)(i32)sh & 0x1f;
                for (u32 r = 0; r < drows; r++) {
                    const u16 *s = (const u16 *)srow;
                    u16 *d = drow;
                    for (u32 c = 0; c < cols; c++) d[c] |= (u16)(((u32)(i16)s[c]) << lsh) & m;
                    drow = (u16 *)((u8 *)drow + stride);
                    srow += srcStride;
                }
            }
        }
    }
}

/* 0x180015210  MergeCompressedGlyph4x4<unsigned short> */
TT_EXPORT void tt_MergeCompressedGlyph4x4_u16(const u8 *p1, int x, int y, u32 clipW, u32 clipH,
                                              u32 stride, u32 *dst) {
    if ((stride >> 2) << 5 < clipW) { throw_exc(THROW_ARG, 0x80070057, 0x6d626772656dULL, 0x515, 0, 0); return; }
    Reader rd;
    rd.cur = rd.rowEnd = rd.cursor = p1 + 0x10;
    rd.f08 = 0; rd.f0c = 0; rd.runRem = 0;
    rd.x0 = x;
    rd.xLimit = (i32)((u32)(*(const u16 *)(p1 + 8)) + (u32)x);
    rd.y = y - 1;
    rd.dataEnd = (p1 + 0x10) + (u64)((*(const u32 *)p1 >> 1) & 0x1ffffff) * 2u;

    do { if (!O_MoveNextY_u16(&rd, 1)) return; } while ((i32)rd.y < 0);

    for (;;) {
        u32 yq = (u32)rd.y >> 2;
        if (clipH <= yq) return;
        u64 reps = 0;
        u32 local_84 = 0;
        int sVar14 = (int)(u8)(((u16)rd.y & 3) << 2);
        u32 base = (u32)(0xf000f << sVar14);
        u32 *rowPtr = (u32 *)((u8 *)dst + (u64)(yq * stride));
        const u8 *pRun = rd.cursor;
        u32 accX = (u32)rd.f0c;
        const u8 *savedCursor;

        if (((rd.y & 3) == 0) && (3 < (int)((u32)rd.runRem + 1))) {
            local_84 = ((u32)rd.y + (u32)rd.runRem + 1) >> 2;
            if ((i32)clipH < (i32)local_84) local_84 = clipH;
            local_84 -= yq;
            reps = local_84;
        }

        u64 iVar7;
        for (;;) {
            iVar7 = reps;
            if (pRun == rd.rowEnd) break;
            savedCursor = pRun + 2;
            u32 a = *(const u16 *)pRun + accX;
            accX = (u32)rd.xLimit;
            if (savedCursor != rd.rowEnd) { accX = (u32)(*(const u16 *)savedCursor) + a; savedCursor = pRun + 4; }
            u32 lo = ((i32)a < 0) ? 0 : a;
            u32 hi = accX;
            if ((i32)(clipW >> 2) < (i32)accX) hi = clipW >> 2;
            pRun = savedCursor;
            rd.f0c = (i32)accX;
            rd.cursor = savedCursor;
            if ((i32)lo < (i32)hi) {
                u32 startW = lo >> 3;
                u16 tail = (u16)((u16)hi & 7);
                u32 endW = hi >> 3;
                u32 leftMask = (0xffffffffu << ((lo & 4) * 4 + (lo & 3) + sVar14)) & base;
                u32 rightMask = ~(0xffffffffu << ((tail & 4) * 4 + (tail & 3) + sVar14)) & base;
                if (iVar7 == 0) {
                    O_MergeScanLine(rowPtr, leftMask, rightMask, base, startW, endW, tail);
                } else {
                    u32 lrep = ((leftMask << 4 | leftMask) << 4 | leftMask) << 4 | leftMask;
                    u32 rrep = ((rightMask << 4 | rightMask) << 4 | rightMask) << 4 | rightMask;
                    u32 *p = rowPtr;
                    u64 k = reps;
                    do {
                        O_MergeScanLine(p, lrep, rrep, 0xffffffff, startW, endW, tail);
                        p = (u32 *)((u8 *)p + stride);
                        k--;
                    } while (k != 0);
                    pRun = savedCursor;
                }
            }
        }
        u32 adv = (u32)(i32)iVar7 * 4;
        if ((i32)iVar7 == 0) adv = 1;
        if (!O_MoveNextY_u16(&rd, adv)) return;
    }
}

/* 0x1800182d0  MergeCompressedGlyph4x4<unsigned char>: the uchar-RLE reader is inlined.
 * runRem = uVar16 (repeat count seen at the top of a scanline), rep = local_94 (the span's
 * remaining repeat count), rowStart = local_80, cursor = pGVar9, rowEnd = pGVar7. */
TT_EXPORT void tt_MergeCompressedGlyph4x4_u8(const u8 *p1, int x, int y, u32 clipW, u32 clipH,
                                             u32 stride, u32 *dst) {
    if ((stride >> 2) << 5 < clipW) { throw_exc(THROW_ARG, 0x80070057, 0x6d626772656dULL, 0x515, 0, 0); return; }
    u16 gw = *(const u16 *)(p1 + 8);
    i32 yy = y - 1;
    const u8 *rowEnd = p1 + 0x10;
    const u8 *end = (p1 + 0x10) + (*(const u32 *)p1 & 0x3ffffff);
    const u8 *cursor = rowEnd, *rowStart = rowEnd;
    u32 runRem = 0, rep = 0;

    do {
        yy++;
        if (runRem == 0) {
            if (rowEnd == end) return;
            u8 h = *rowEnd;
            u64 span = h;
            rowStart = rowEnd + 1;
            if ((h & 1) != 0 && rowStart < end) { span = *rowStart; runRem = (u32)(h >> 1); rowStart = rowEnd + 2; rep = runRem; }
            u64 take = (u64)(end - rowStart);
            if ((i64)(span >> 1) <= (i64)(end - rowStart)) take = span >> 1;
            rowEnd = rowStart + take;
            cursor = rowStart;
        } else {
            runRem--; rep = runRem;
        }
    } while (yy < 0);

    for (;;) {
        u32 yq = (u32)yy >> 2;
        if (clipH <= yq) return;
        u64 reps = 0;
        int sh = (int)(u8)(((u16)yy & 3) << 2);
        u32 base = (u32)(0xf000f << sh);
        u8 *rowPtr = (u8 *)dst + (u64)(u32)(yq * stride);
        if (((u32)yy & 3) == 0 && 3 < (i32)(runRem + 1)) {
            u32 l88 = ((u32)yy + runRem + 1) >> 2;
            if ((i32)clipH < (i32)l88) l88 = clipH;
            l88 -= yq;
            reps = l88;
        }
        u32 accX = (u32)x;
        i32 it;
        for (;;) {
            it = (i32)reps;
            if (cursor == rowEnd) break;
            u32 a = (u32)(*cursor) + accX;
            const u8 *nx = cursor + 1;
            u32 b = (u32)gw + (u32)x;
            if (nx != rowEnd) { b = (u32)(*nx) + a; nx = cursor + 2; }
            accX = b;
            u32 lo = ((i32)a < 0) ? 0 : a;
            u32 hi = b;
            if ((i32)(clipW >> 2) < (i32)b) hi = clipW >> 2;
            cursor = nx;
            if ((i32)lo < (i32)hi) {
                u32 w = lo >> 3, wEnd = hi >> 3;
                u32 lm = (0xffffffffu << (((lo & 4) * 4 + (lo & 3) + sh) & 0x1f)) & base;
                u32 rm = ~(0xffffffffu << (((hi & 4) * 4 + (hi & 3) + sh) & 0x1f)) & base;
                if (it == 0) {
                    if (w == wEnd) {
                        *(u32 *)(rowPtr + (u64)w * 4) |= rm & lm;
                    } else {
                        *(u32 *)(rowPtr + (u64)w * 4) |= lm;
                        u32 w2 = w + 1;
                        if (w2 < wEnd) { u32 *p = (u32 *)(rowPtr + (u64)w2 * 4); for (u32 k = wEnd - w2; k; k--) *p++ |= base; }
                        if ((hi & 7) != 0) *(u32 *)(rowPtr + (u64)wEnd * 4) |= rm;
                    }
                } else {
                    u32 lr = ((lm << 4 | lm) << 4 | lm) << 4 | lm;
                    u32 rr = ((rm << 4 | rm) << 4 | rm) << 4 | rm;
                    u8 *p = rowPtr;
                    u64 k = reps;
                    do {
                        if (w == wEnd) {
                            *(u32 *)(p + (u64)w * 4) |= rr & lr;
                        } else {
                            *(u32 *)(p + (u64)w * 4) |= lr;
                            u32 w2 = w + 1;
                            if (w2 < wEnd) memset(p + (u64)w2 * 4, -1, (u64)(wEnd - w2) << 2);
                            if ((hi & 7) != 0) *(u32 *)(p + (u64)wEnd * 4) |= rr;
                        }
                        p += stride;
                        k--;
                    } while (k != 0);
                }
            }
        }
        it = (it == 0) ? 1 : (i32)((u32)it << 2);
        yy = (i32)((u32)yy + (u32)it);
        if (rep != 0) {
            if (it != (i32)(rep + 1)) {
                runRem = rep - (u32)it;
                cursor = rowStart;
                rep = runRem;
                continue;
            }
            rep = 0;
        }
        if (rowEnd == end) return;
        {
            u8 h = *rowEnd;
            u64 span = h;
            rowStart = rowEnd + 1;
            if ((h & 1) != 0 && rowStart < end) { span = *rowStart; rep = (u32)(h >> 1); rowStart = rowEnd + 2; }
            u64 take = (u64)(end - rowStart);
            if ((i64)(span >> 1) <= (i64)(end - rowStart)) take = span >> 1;
            rowEnd = rowStart + take;
            cursor = rowStart;
            runRem = rep;
        }
    }
}

/* ================================================================= aliased overlay ===== */

/* 0x1800d6c58  DrawAliasedGlyphUncompressed<unsigned char>: OR 0xff per set bit into an 8-bit
 * coverage buffer. */
TT_EXPORT void tt_DrawAliasedGlyphUncompressed_u8(const u8 *p1, int x, int y, u8 value, u8 *buf,
                                                  u32 W, u32 H) {
    (void)value;
    u32 words = (u32)(*(const u16 *)(p1 + 8) + 0x1f) >> 5;
    if ((*(const u32 *)p1 & 0x3ffffff) != (u32)(*(const u16 *)(p1 + 10)) * words * 4u) { throw_exc(THROW_CACHE, 0x88985007, 0x7461645f6d62ULL, 0x642, p1, 0); return; }
    int x0 = (x < 0) ? 0 : x;
    u32 right = (u32)(*(const u16 *)(p1 + 8)) + (u32)x; if ((i32)W < (i32)right) right = W;
    int y0 = (y < 0) ? 0 : y;
    u32 bottom = (u32)(*(const u16 *)(p1 + 10)) + (u32)y; if ((i32)H < (i32)bottom) bottom = H;
    if (!(y0 < (i32)bottom && x0 < (i32)right)) return;
    const u8 *src = p1 + 0x10 + (u64)((u32)(y0 - y) * words) * 4u;
    u8 *drow = buf + (u64)((u32)y0 * W);
    int bc0 = x0 - x;
    for (u32 r = 0; r < bottom - (u32)y0; r++) {
        int bc = bc0;
        for (u32 c = (u32)x0; c < right; c++) {
            if (*(const u32 *)(src + (u64)(bc >> 5) * 4) >> (bc & 0x1f) & 1) drow[c] = 0xff;
            bc++;
        }
        src += (u64)words * 4u;
        drow += W;
    }
}

/* 0x1800ac344  DrawAliasedGlyphCompressed<unsigned char,unsigned char> */
TT_EXPORT void tt_DrawAliasedGlyphCompressed_u8_u8(const u8 *p1, int x, int y, u8 value, u8 *buf,
                                                   u32 W, u32 H) {
    (void)value;
    Reader rd;
    rd.x0 = x;
    rd.xLimit = (i32)((u32)(*(const u16 *)(p1 + 8)) + (u32)x);
    rd.f08 = 0; rd.f0c = 0;
    rd.y = y - 1;
    rd.runRem = 0;
    rd.cur = rd.rowEnd = rd.cursor = p1 + 0x10;
    rd.dataEnd = (p1 + 0x10) + (*(const u32 *)p1 & 0x3ffffff);
    if (!O_MoveNextY_u8(&rd, 1)) return;
    while ((i32)rd.y < 0) { if (!O_MoveNextY_u8(&rd, 1)) return; }
    for (;;) {
        if ((u32)rd.y >= H) return;
        u8 *row = buf + (u64)((u32)rd.y * W);
        const u8 *cur = rd.cursor;
        u32 accX = (u32)rd.f0c;
        while (cur != rd.rowEnd) {
            u32 a = (u32)(*cur) + accX;
            cur++;
            u32 b = (u32)rd.xLimit;
            if (cur != rd.rowEnd) { b = (u32)(*cur) + a; cur++; }
            accX = b;
            rd.f08 = (i32)a; rd.f0c = (i32)b; rd.cursor = cur;
            u32 lo = ((i32)a < 0) ? 0 : a;
            u32 hi = b; if ((i32)W < (i32)b) hi = W;
            if ((i32)lo < (i32)hi) memset(row + lo, 0xff, hi - lo);
        }
        if (!O_MoveNextY_u8(&rd, 1)) return;
    }
}

/* 0x18010a1a4  DrawAliasedGlyphCompressed<unsigned char,unsigned short> */
TT_EXPORT void tt_DrawAliasedGlyphCompressed_u8_u16(const u8 *p1, int x, int y, u8 value, u8 *buf,
                                                    u32 W, u32 H) {
    (void)value;
    Reader rd;
    rd.x0 = x;
    rd.xLimit = (i32)((u32)(*(const u16 *)(p1 + 8)) + (u32)x);
    rd.f08 = 0; rd.f0c = 0;
    rd.y = y - 1;
    rd.runRem = 0;
    rd.cur = rd.rowEnd = rd.cursor = p1 + 0x10;
    rd.dataEnd = (p1 + 0x10) + (u64)((*(const u32 *)p1 >> 1) & 0x1ffffff) * 2u;
    if (!O_MoveNextY_u16(&rd, 1)) return;
    while ((i32)rd.y < 0) { if (!O_MoveNextY_u16(&rd, 1)) return; }
    for (;;) {
        if ((u32)rd.y >= H) return;
        u8 *row = buf + (u64)((u32)rd.y * W);
        while (O_MoveNextX_u16(&rd)) {
            u32 a = (u32)rd.f08;
            u32 lo = ((i32)a < 0) ? 0 : a;
            u32 hi = (u32)rd.f0c; if ((i32)W < (i32)hi) hi = W;
            if ((i32)lo < (i32)hi) memset(row + lo, 0xff, hi - lo);
        }
        if (!O_MoveNextY_u16(&rd, 1)) return;
    }
}

/* 0x180015730  DrawAliasedGlyphUncompressed<PixelLMR24>: 3 colour bytes per set bit. */
TT_EXPORT void tt_DrawAliasedGlyphUncompressed_LMR(const u8 *p1, int x, int y, const void *color,
                                                   u8 *buf, u32 W, u32 H) {
    const u8 *c = (const u8 *)color;
    u32 words = (u32)(*(const u16 *)(p1 + 8) + 0x1f) >> 5;
    if ((*(const u32 *)p1 & 0x3ffffff) != (u32)(*(const u16 *)(p1 + 10)) * words * 4u) { throw_exc(THROW_CACHE, 0x88985007, 0x7461645f6d62ULL, 0x642, p1, 0); return; }
    int x0 = (x < 0) ? 0 : x;
    u32 right = (u32)(*(const u16 *)(p1 + 8)) + (u32)x; if ((i32)W < (i32)right) right = W;
    int y0 = (y < 0) ? 0 : y;
    u32 bottom = (u32)(*(const u16 *)(p1 + 10)) + (u32)y; if ((i32)H < (i32)bottom) bottom = H;
    if (!(y0 < (i32)bottom && x0 < (i32)right)) return;
    const u8 *src = p1 + 0x10 + (u64)((u32)(y0 - y) * words) * 4u;
    u8 *drow = buf + (u64)((u32)y0 * W) * 3;
    int bc0 = x0 - x;
    for (u32 r = 0; r < bottom - (u32)y0; r++) {
        int bc = bc0;
        u8 *d = drow + (u64)((u32)x0 * 3);
        for (u32 col = (u32)x0; col < right; col++) {
            if (*(const u32 *)(src + (u64)(bc >> 5) * 4) >> (bc & 0x1f) & 1) { d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; }
            d += 3; bc++;
        }
        src += (u64)words * 4u;
        drow += (u64)W * 3;
    }
}

/* 0x1800ac1ec  DrawAliasedGlyphCompressed<PixelLMR24,unsigned char> */
TT_EXPORT void tt_DrawAliasedGlyphCompressed_LMR_u8(const u8 *p1, int x, int y, const void *color,
                                                    u8 *buf, u32 W, u32 H) {
    const u8 *c = (const u8 *)color;
    Reader rd;
    rd.x0 = x;
    rd.xLimit = (i32)((u32)(*(const u16 *)(p1 + 8)) + (u32)x);
    rd.f08 = 0; rd.f0c = 0;
    rd.y = y - 1;
    rd.runRem = 0;
    rd.cur = rd.rowEnd = rd.cursor = p1 + 0x10;
    rd.dataEnd = (p1 + 0x10) + (*(const u32 *)p1 & 0x3ffffff);
    if (!O_MoveNextY_u8(&rd, 1)) return;
    while ((i32)rd.y < 0) { if (!O_MoveNextY_u8(&rd, 1)) return; }
    for (;;) {
        if ((u32)rd.y >= H) return;
        u8 *row = buf + (u64)((u32)rd.y * W) * 3;
        const u8 *cur = rd.cursor;
        u32 accX = (u32)rd.f0c;
        while (cur != rd.rowEnd) {
            u32 a = (u32)(*cur) + accX;
            cur++;
            u32 b = (u32)rd.xLimit;
            if (cur != rd.rowEnd) { b = (u32)(*cur) + a; cur++; }
            accX = b;
            rd.f08 = (i32)a; rd.f0c = (i32)b; rd.cursor = cur;
            u32 lo = ((i32)a < 0) ? 0 : a;
            u32 hi = b; if ((i32)W < (i32)b) hi = W;
            if ((i32)lo < (i32)hi) {
                u8 *d = row + (u64)lo * 3;
                for (u32 k = hi - lo; k; k--) { d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d += 3; }
            }
        }
        if (!O_MoveNextY_u8(&rd, 1)) return;
    }
}

/* 0x180147b6c  DrawAliasedGlyphCompressed<PixelLMR24,unsigned short> */
TT_EXPORT void tt_DrawAliasedGlyphCompressed_LMR_u16(const u8 *p1, int x, int y, const void *color,
                                                     u8 *buf, u32 W, u32 H) {
    const u8 *c = (const u8 *)color;
    Reader rd;
    rd.x0 = x;
    rd.xLimit = (i32)((u32)(*(const u16 *)(p1 + 8)) + (u32)x);
    rd.f08 = 0; rd.f0c = 0;
    rd.y = y - 1;
    rd.runRem = 0;
    rd.cur = rd.rowEnd = rd.cursor = p1 + 0x10;
    rd.dataEnd = (p1 + 0x10) + (u64)((*(const u32 *)p1 >> 1) & 0x1ffffff) * 2u;
    if (!O_MoveNextY_u16(&rd, 1)) return;
    while ((i32)rd.y < 0) { if (!O_MoveNextY_u16(&rd, 1)) return; }
    for (;;) {
        if ((u32)rd.y >= H) return;
        u8 *row = buf + (u64)((u32)rd.y * W) * 3;
        while (O_MoveNextX_u16(&rd)) {
            u32 a = (u32)rd.f08;
            u32 lo = ((i32)a < 0) ? 0 : a;
            u32 hi = (u32)rd.f0c; if ((i32)W < (i32)hi) hi = W;
            if ((i32)lo < (i32)hi) {
                u8 *d = row + (u64)lo * 3;
                for (u32 k = hi - lo; k; k--) { d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d += 3; }
            }
        }
        if (!O_MoveNextY_u16(&rd, 1)) return;
    }
}

/* 0x180015698  DrawAliasedGlyph<PixelLMR24>: dispatch on cache kind. */
TT_EXPORT void tt_DrawAliasedGlyph_LMR(const u8 *p1, int x, int y, const void *color, u8 *buf,
                                       u32 W, u32 H) {
    u32 k = *(const u32 *)p1 >> 0x1a;
    if (k & 0x10)      O_DrawUncLMR(p1, x, y, color, buf, W, H);
    else if (k & 8)    O_DrawCLMRU16(p1, x, y, color, buf, W, H);
    else               O_DrawCLMRU8(p1, x, y, color, buf, W, H);
}

/* ================================================================= filters ============= */

/* 0x180015bbc  GlyphRunAnalysis::ApplyGrayscaleFilter: subsample counts -> 8-bit coverage. */
TT_EXPORT void tt_ApplyGrayscaleFilter(void *ga, u8 *src, u32 stride, u32 width, u32 height, u8 *out) {
    i32 rt = (i32)AT(ga, 0x88, u32) >> 0x1a;
    if (rt == 3) {
        for (u32 row = 0; row < height; row++) {
            const u8 *s = src + (u64)(u32)(row * stride);
            u8 *d = out + (u64)(u32)(row * width);
            for (u32 col = 0; col < width; col++) d[col] = g_Overscale8x1ToCoverage[s[col]];
        }
    } else if (rt == 4) {
        for (u32 row = 0; row < height; row++) {
            const u8 *s = src + (u64)(u32)(row * stride);
            u8 *d = out + (u64)(u32)(row * width);
            for (u32 col = 0; col < width; col++) {
                u32 cnt = (u32)g_BitCounter[s[1]] + (u32)g_BitCounter[s[0]];
                d[col] = (u8)((cnt * 0xff + 8) >> 4);
                s += 2;
            }
        }
    } else if (rt == 5) {
        for (u32 row = 0; row < height; row++) {
            const u8 *s0 = src + (u64)(u32)(2 * (row * stride));
            const u8 *s1 = s0 + stride;
            u8 *d = out + (u64)(u32)(row * width);
            u32 col = 0;
            while (col < width) {
                u8 b0 = *s0++, b1 = *s1++;
                u32 end = col + 4; if (end > width) end = width;
                for (; col < end; col++) {
                    u32 idx = ((b0 & 3) << 2) | (b1 & 3);
                    b0 >>= 2; b1 >>= 2;
                    d[col] = g_Overscale2x2ToCoverage[idx];
                }
            }
        }
    } else {
        /* raster type 0 (aliased) and any other: zero the whole output */
        memset(out, 0, (u64)(u32)(width * height));
    }
}

/* 0x1800172e0  ApplyFilterImpl<AlphaTextureTarget,1>  (6x1 ClearType) */
TT_EXPORT void tt_ApplyFilterImpl1(struct FilterTarget *t, u32 p2, i16 shift,
                                   const struct ClearTypeFilter *f, const void *src, u32 p6,
                                   u32 rowCount, u64 p8) {
    (void)p2; (void)p6;
    u32 w = t->width;
    u32 h = t->height; if (rowCount < t->height) h = rowCount;
    u64 dwStride = p8 >> 2;
    if (dwStride > 0xffffffff) { TRAP(); return; }
    u16 window = (u16)((u16)(u8)f->m0 + (u8)f->m1 + 6);
    for (u32 row = 0; row < h; row++) {
        u16 bitcur = (u16)(shift + 0x20);
        u64 win;
        u32 s0 = *(const u32 *)src;
        if (shift < 0) win = (u64)(s0 >> ((u8)(-shift) & 0x3f));
        else win = (u64)s0 << ((u8)shift & 0x3f);
        u32 px = 0, srcIdx = 1;
        for (; px < w; px++) {
            if (bitcur < window) {
                if (srcIdx < (u32)dwStride) { win |= (u64)(*(const u32 *)((const u8 *)src + (u64)srcIdx * 4)) << ((u8)bitcur & 0x3f); srcIdx++; }
                bitcur = (u16)(bitcur + 0x20);
            }
            u64 idx = win & (u64)(((u32)1 << (window & 0x1f)) - 1);
            win >>= 6;
            u16 e = f->table[idx];
            u8 R = g_AlphaNormalizationTable6x1[e & 0x1f];
            u8 G = g_AlphaNormalizationTable6x1[(e >> 5) & 0x1f];
            bitcur = (u16)(bitcur - 6);
            u8 *d = t->cur + (u64)px * 3;
            d[2] = g_AlphaNormalizationTable6x1[(e >> 10) & 0x1f];
            d[0] = R; d[1] = G;
        }
        u8 *cur = t->cur;
        src = (const u8 *)src + dwStride * 4;
        u8 *nc = cur + t->rowStride;
        if (t->end < nc) { FAILASSERT(0x87); return; }
        t->cur = nc;
        memset(cur + (u32)(t->width * 3), 0, (size_t)(nc - (cur + (u32)(t->width * 3))));
    }
}

/* 0x180016720  ApplyFilterImpl<AlphaTextureTarget,5>  (6x5 ClearType, [4,9,10,9,4] vertical) */
TT_EXPORT void tt_ApplyFilterImpl5(struct FilterTarget *t, u32 p2, i16 shift,
                                   const struct ClearTypeFilter *f, const void *src, u32 p6,
                                   u32 rowCount, u64 p8) {
    (void)p2; (void)p6;
    u32 w = t->width;
    u32 h = rowCount / 5; if (t->height <= h) h = t->height;
    u64 dwStride = p8 >> 2;
    if (dwStride > 0xffffffff) { TRAP(); return; }
    u16 window = (u16)((u16)(u8)f->m0 + (u8)f->m1 + 6);
    u64 mask = (u64)(((u32)1 << (window & 0x1f)) - 1);
    u8 bl = (u8)shift, bn = (u8)(-(int)shift);
    for (u32 row = 0; row < h; row++) {
        u32 r0 = *(const u32 *)src;
        u32 r1 = *(const u32 *)((const u8 *)src + dwStride * 4);
        u32 r2 = *(const u32 *)((const u8 *)src + dwStride * 8);
        u32 r3 = *(const u32 *)((const u8 *)src + dwStride * 12);
        u32 r4 = *(const u32 *)((const u8 *)src + dwStride * 16);
        u64 w0, w1, w2, w3, w4;
        if (shift < 0) {
            w0 = (u64)(r0 >> (bn & 0x3f)); w1 = (u64)(r1 >> (bn & 0x3f));
            w2 = (u64)(r2 >> (bn & 0x3f)); w3 = (u64)(r3 >> (bn & 0x3f)); w4 = (u64)(r4 >> (bn & 0x3f));
        } else {
            w0 = (u64)r0 << (bl & 0x3f); w1 = (u64)r1 << (bl & 0x3f);
            w2 = (u64)r2 << (bl & 0x3f); w3 = (u64)r3 << (bl & 0x3f); w4 = (u64)r4 << (bl & 0x3f);
        }
        u16 bitcur = (u16)(shift + 0x20);
        u32 px = 0, srcIdx = 1;
        for (; px < w; px++) {
            if (bitcur < window) {
                if (srcIdx < (u32)dwStride) {
                    u8 sb = (u8)bitcur;
                    const u8 *sp = (const u8 *)src;
                    w0 |= (u64)(*(const u32 *)(sp + (u64)srcIdx * 4)) << (sb & 0x3f);
                    w1 |= (u64)(*(const u32 *)(sp + (u64)(srcIdx + dwStride) * 4)) << (sb & 0x3f);
                    w2 |= (u64)(*(const u32 *)(sp + (u64)(srcIdx + dwStride * 2) * 4)) << (sb & 0x3f);
                    w3 |= (u64)(*(const u32 *)(sp + (u64)(srcIdx + dwStride * 3) * 4)) << (sb & 0x3f);
                    w4 |= (u64)(*(const u32 *)(sp + (u64)(srcIdx + dwStride * 4) * 4)) << (sb & 0x3f);
                    srcIdx++;
                }
                bitcur = (u16)(bitcur + 0x20);
            }
            const u16 *tbl = f->table;
            u16 e0 = tbl[w0 & mask];
            u16 e3 = tbl[w3 & mask];
            u16 e1 = tbl[w1 & mask];
            u16 e2 = tbl[w2 & mask];
            u16 e4 = tbl[w4 & mask];
            u8 *d = t->cur + (u64)px * 3;
            d[0] = g_AlphaNormalizationTable6x5[((e1 & 0x1f) + (e3 & 0x1f)) * 9 + ((e4 & 0x1f) + (e0 & 0x1f)) * 4 + (e2 & 0x1f) * 10];
            d[1] = g_AlphaNormalizationTable6x5[(((e1 >> 5) & 0x1f) + ((e3 >> 5) & 0x1f)) * 9 + (((e0 >> 5) & 0x1f) + ((e4 >> 5) & 0x1f)) * 4 + ((e2 >> 5) & 0x1f) * 10];
            d[2] = g_AlphaNormalizationTable6x5[(((e1 >> 10) & 0x1f) + ((e3 >> 10) & 0x1f)) * 9 + (((e0 >> 10) & 0x1f) + ((e4 >> 10) & 0x1f)) * 4 + ((e2 >> 10) & 0x1f) * 10];
            w0 >>= 6; w1 >>= 6; w2 >>= 6; w3 >>= 6; w4 >>= 6;
            bitcur = (u16)(bitcur - 6);
        }
        src = (const u8 *)src + dwStride * 20;
        u8 *cur = t->cur;
        u8 *nc = cur + t->rowStride;
        if (t->end < nc) { FAILASSERT(0x87); return; }
        t->cur = nc;
        memset(cur + (u32)(t->width * 3), 0, (size_t)(nc - (cur + (u32)(t->width * 3))));
    }
}

/* ================================================================= run bitmap =========== */

/* 0x1800158bc  GlyphRunAnalysis::FillRunBitmap */
TT_EXPORT void tt_FillRunBitmap(void *ga, u32 rt, void *rect, u32 rows, u32 bufSize, void *buf) {
    u32 attr = AT(ga, 0x88, u32);
    u32 lowNib = attr & 0xf, highNib = (attr >> 4) & 0xf;
    u32 count = AT(ga, 0x48, u32);
    switch (rt) {
        case 0: count = AT(ga, 0x28, u32); break;
        case 1: if (lowNib != 6 || highNib != 1) return; break;
        case 2: if (lowNib != 6 || highNib != 5) return; break;
        case 3: if (lowNib != 8 || highNib != 1) return; break;
        case 4: if (lowNib != 4 || highNib != 4) return; break;
        case 5: if (lowNib != 2 || highNib != 2) return; break;
        default: throw_exc(THROW_ARG, 0x80070057, 0x6d625f6c6c6966ULL, 0x676, 0, 0); return;
    }
    if (count == 0) return;
    O_FillGlyphBitmaps(rt, count, AT(ga, 0x30, void *), AT(ga, 0x38, void *), rect, rows, bufSize, buf);
}

/* 0x180123380  DWriteGlyphRunAnalysis::FillRunBitmap (public wrapper; not on the
 * CreateAlphaTexture path, so not exercised by reimpl/alpha.py). */
TT_EXPORT long tt_DWriteFillRunBitmap(void *self, u32 rt, void *rect, u32 rows, u32 bufSize, void *buf) {
    if (rect == 0) return 0x80070057;
    O_FillRunBitmap((u8 *)self + 8, rt, rect, rows, bufSize, buf);
    return 0;
}

/* 0x180015a24  GlyphRunAnalysis::MergeGrayscaleGlyphBitmaps */
TT_EXPORT u8 *tt_MergeGrayscaleGlyphBitmaps(void *ga, void *rectp, u32 *outStride, u32 *outC, u32 *outRows) {
    u32 attr = AT(ga, 0x88, u32);
    RECT4 *rect = (RECT4 *)rectp;
    if (!(attr & (1u << 17))) { *outStride = 0; *outC = 0; *outRows = 0; return 0; }
    u32 width = (u32)(rect->right - rect->left);
    u32 height = (u32)(rect->bottom - rect->top);
    u32 xov = attr & 0xf, yov = (attr >> 4) & 0xf;
    u32 r15, rows;
    if (attr & (1u << 22)) { xov = xov * yov; r15 = width * xov; rows = height; }
    else { rows = height * yov; r15 = width * xov; }
    u64 sz = (u64)r15 + 3;
    if (sz < (u64)r15 || sz > 0xffffffff) { TRAP(); return 0; }
    u32 stride = (u32)sz & 0xfffffffc;

    void *vec = (u8 *)ga + 0x70;
    u8 *first = AT(vec, 0, u8 *), *last = AT(vec, 8, u8 *);
    if (first != last) { AT(vec, 8, u8 *) = first; last = first; }
    u64 need = (u64)(stride >> 2) * rows;
    if (need > 0xffffffff) { TRAP(); return 0; }
    u32 newCount = (u32)need;
    if (newCount != 0) {
        u64 cap = (u64)((AT(vec, 0x10, u8 *) - first)) >> 2;
        if (newCount > cap) {
            O_Resize(vec, newCount, 0);
            first = AT(vec, 0, u8 *);
        } else {
            memset(first, 0, (u64)newCount * 4u);
            AT(vec, 8, u8 *) = first + (u64)newCount * 4u;
        }
    }
    first = AT(vec, 0, u8 *);
    u32 rt = (u32)((i32)AT(ga, 0x88, u32) >> 0x1a);
    O_FillRunBitmap(ga, rt, rect, rows, stride, first);
    *outStride = stride;
    *outC = r15;
    *outRows = rows;
    return first;
}

/* ================================================================= entry points ========= */

/* 0x180017528  GlyphRunAnalysis::CreateAlphaTextureGrayscale */
TT_EXPORT void tt_CreateAlphaTextureGrayscale(void *ga, void *rectp, u8 *out, u32 bufSize) {
    RECT4 *rect = (RECT4 *)rectp;
    if (!(rect->right > rect->left && rect->bottom > rect->top)) { throw_exc(THROW_ARG, 0x80070057, 0x72675f6168706c61ULL, 0x5a8, 0, 0); return; }  /* IsRectEmpty */
    u32 width = (u32)(rect->right - rect->left);
    u32 height = (u32)(rect->bottom - rect->top);
    if ((u64)height * width > (u64)bufSize) { throw_exc(THROW_OS, 0x8007007a, 0x6168706c61ULL, 0x5ad, 0, 0); return; }
    u32 area = height * width;
    u32 local38[3] = {0, 0, 0};
    u8 *buf = O_MergeGrayscale(ga, rect, &local38[0], &local38[2], &local38[1]);
    O_ApplyGray(ga, buf, local38[0], width, height, out);
    for (u32 i = AT(ga, 0x48, u32); i < AT(ga, 0x28, u32); i++) {
        const u8 *layout = *(const u8 **)((u8 *)AT(ga, 0x30, u8 *) + (u64)i * 8);
        u64 pos = *(const u64 *)((u8 *)AT(ga, 0x38, u8 *) + (u64)i * 8);
        int ix = (int)(u32)pos - rect->left;
        int iy = (int)(u32)(pos >> 32) - rect->top;
        u32 k = *(const u32 *)layout >> 0x1a;
        if (!(k & 0x10)) {
            if (!(k & 8)) O_DrawCU8U8(layout, ix, iy, 0, out, width, height);
            else          O_DrawCU8U16(layout, ix, iy, 0, out, width, height);
        } else {
            O_DrawUU8(layout, ix, iy, 0, out, width, height);
        }
    }
    if (area < bufSize) memset(out + area, 0, bufSize - area);
}

/* 0x180015f04  GlyphRunAnalysis::CreateAlphaTexture3x1 */
TT_EXPORT void tt_CreateAlphaTexture3x1(void *ga, void *rectp, u8 *out, u32 bufSize) {
    RECT4 *rect = (RECT4 *)rectp;
    if ((AT(ga, 0x88, u8) & 0xf) != 6) { throw_exc(THROW_NOTSUP, 0x88985009, 0x31783368706c61ULL, 0x5ed, 0, 0); return; }  /* NotSupportedException */
    int top = rect->top, right0 = rect->right, bottom = rect->bottom, left = rect->left;
    u32 width = (u32)(right0 - left);
    u64 w3 = (u64)width * 3;
    if (w3 > 0xffffffff) { TRAP(); return; }
    w3 &= 0xffffffff;
    u64 areaU = (u64)(u32)(bottom - top) * w3;
    if (areaU > 0xffffffff) { TRAP(); return; }
    u32 area = (u32)areaU;
    if (bufSize < area) { throw_exc(THROW_OS, 0x8007007a, 0x31783368706c61ULL, 0x5f4, 0, 0); return; }

    void *vec = (u8 *)ga + 0x70;
    int bVar11 = (((AT(ga, 0x88, u32) >> 4) & 0xf) > 1);
    if (!(right0 > left && bottom > top)) { throw_exc(THROW_ARG, 0x80070057, 0x726f74635f7463ULL, 0x186, 0, 0); return; }

    i64 lLeft6 = (i64)left * 6;
    if ((u64)(lLeft6 + 0x80000000) > 0xffffffff) { TRAP(); return; }
    lLeft6 = (i64)(i32)lLeft6 - 2;
    if (lLeft6 < -0x80000000LL) { TRAP(); return; }
    if ((u64)((i64)right0 * 6 + 0x80000000) > 0xffffffff) { TRAP(); return; }
    i64 lRight6 = (i64)(i32)((i64)right0 * 6) + 2;
    if (lRight6 > 0x7fffffff) { TRAP(); return; }
    i64 pTop, pBot;
    if (bVar11) {
        pTop = (i64)top * 5;
        if ((u64)(pTop + 0x80000000) > 0xffffffff) { TRAP(); return; }
        pBot = (i64)bottom * 5;
        if ((u64)(pBot + 0x80000000) > 0xffffffff) { TRAP(); return; }
    } else { pTop = top; pBot = bottom; }

    i32 subLeft = (i32)lLeft6;
    u32 subW = (u32)((i32)lRight6 - subLeft);
    u32 rowsN = (u32)((i32)pBot - (i32)pTop);
    u64 u24 = (u64)subW + 0x1f;
    if (u24 < (u64)subW || u24 > 0xffffffff) { TRAP(); return; }
    u32 strideBytes = (((u32)u24 & 0xffffffe0) >> 3);
    u32 words = (u32)u24 >> 5;
    u32 stride = words * 4;

    u8 *first = AT(vec, 0, u8 *), *last = AT(vec, 8, u8 *);
    if (first != last) { AT(vec, 8, u8 *) = first; last = first; }
    if ((u64)rowsN * words > 0xffffffff) { TRAP(); return; }
    u32 newCount = rowsN * words;
    /* _Last was reset to _First, so curCount == 0 */
    if (newCount != 0) {
        u64 cap = (u64)(AT(vec, 0x10, u8 *) - first) >> 2;
        if (newCount > cap) { O_Resize(vec, newCount, 0); first = AT(vec, 0, u8 *); }
        else { memset(first, 0, (u64)newCount * 4u); AT(vec, 8, u8 *) = first + (u64)newCount * 4u; }
    }
    u32 *bmp = (u32 *)AT(vec, 0, u8 *);
    if ((words << 5) < subW || (u64)((AT(vec, 8, u8 *) - (u8 *)bmp) >> 2) < (u64)(u32)(rowsN * words)) FAILASSERT(0x60b);

    /* merge glyphs [0, overlayStart) into the subsample bitmap */
    for (u32 i = 0; i < AT(ga, 0x48, u32); i++) {
        const u8 *layout = *(const u8 **)((u8 *)AT(ga, 0x30, u8 *) + (u64)i * 8);
        u64 pos = *(const u64 *)((u8 *)AT(ga, 0x38, u8 *) + (u64)i * 8);
        int ix = (int)(u32)pos - subLeft;
        int iy = (int)(u32)(pos >> 32) - (int)pTop;
        u32 k = *(const u32 *)layout >> 0x1a;
        if (k & 0x10) {
            O_MergeUncomp1Bit(layout, ix, iy, subW, rowsN, stride, bmp);
        } else if (k & 8) {
            O_MergeComp1Bit_u16(layout, ix, iy, subW, rowsN, stride, bmp);
        } else {
            /* inline uchar-RLE 1-bit merge */
            u16 gw = *(const u16 *)(layout + 8);
            const u8 *dataEnd = (layout + 0x10) + (*(const u32 *)layout & 0x3ffffff);
            const u8 *pRun = layout + 0x10;
            const u8 *cursor = layout + 0x10;
            const u8 *rowEnd = layout + 0x10;
            u32 runRem = 0;
            i32 yy = iy - 1;
            /* skip negative-y rows */
            do {
                yy++;
                if (runRem == 0) {
                    if (pRun == dataEnd) goto next_glyph;
                    u8 h0 = *pRun;
                    u64 span = h0;
                    cursor = pRun + 1;
                    if ((h0 & 1) != 0 && cursor < dataEnd) { span = *cursor; runRem = (u32)(h0 >> 1); cursor = pRun + 2; }
                    u64 room = (u64)(dataEnd - cursor);
                    if ((i64)(span >> 1) <= (i64)room) room = span >> 1;
                    pRun = cursor + room;
                    rowEnd = pRun;
                } else { runRem--; }
            } while ((i32)yy < 0);

            {
                const u8 *runStart = cursor;   /* row start for repeats */
                u32 accX = (u32)ix;
                for (;;) {
                    for (;;) {
                        if ((u32)rowsN <= (u32)yy) goto next_glyph;
                        u64 rowOff = (u64)((u32)yy * stride);
                        while (cursor != rowEnd) {
                            u32 a = (u32)(*cursor) + accX;
                            const u8 *nx = cursor + 1;
                            accX = (u32)gw + (u32)ix;
                            if (nx != rowEnd) { accX = (u32)(*nx) + a; nx = cursor + 2; }
                            u32 lo = ((i32)a < 0) ? 0 : a;
                            u32 hi = accX; if ((i32)subW < (i32)accX) hi = subW;
                            cursor = nx;
                            if ((i32)lo < (i32)hi) set_span_1bit((u8 *)bmp, rowOff, lo, hi);
                        }
                        yy++;
                        accX = (u32)ix;
                        if (runRem == 0) break;
                        runRem--;
                        cursor = runStart;
                    }
                    if (rowEnd == dataEnd) goto next_glyph;
                    u8 h0 = *rowEnd;
                    u64 span = h0;
                    runStart = rowEnd + 1;
                    if ((h0 & 1) != 0 && runStart < dataEnd) { span = *runStart; runRem = (u32)(h0 >> 1); runStart = rowEnd + 2; }
                    u64 room = (u64)(dataEnd - runStart);
                    if ((i64)(span >> 1) <= (i64)room) room = span >> 1;
                    cursor = runStart;
                    rowEnd = runStart + room;
                }
            }
        }
        next_glyph:;
    }

    /* one filter pass */
    if (subW != 0 && rowsN != 0) {
        struct FilterTarget ft;
        ft.f0 = 0;
        ft.width = (u32)(right0 - left);
        ft.height = (u32)(bottom - top);
        ft.rowStride = (u64)(ft.width * 3);
        if (w3 != 0 && w3 < ft.rowStride) { throw_exc(THROW_OS, 0x8007007a, 0x747261656c63ULL, 0x1b9, 0, 1); return; }
        /* original: OSException 0x8007007A when the bitmap is null or height*rowStride > bufSize */
        if (bmp == 0 || (u64)ft.height * ft.rowStride > (u64)bufSize) { throw_exc(THROW_OS, 0x8007007a, 0x747261656c63ULL, 0x1bf, 0, 0); return; }
        ft.cur = out;
        ft.end = out + ft.rowStride * ft.height;
        if (bVar11)
            O_ApplyFilter5(&ft, ft.width, 0, &g_classicPlattFilterRGB, bmp, stride, rowsN, strideBytes);
        else
            O_ApplyFilter1(&ft, ft.width, 0, &g_classicPlattFilterRGB, bmp, stride, rowsN, strideBytes);
    }

    /* aliased overlay (white) */
    u8 color[3] = {0xff, 0xff, 0xff};
    for (u32 i = AT(ga, 0x48, u32); i < AT(ga, 0x28, u32); i++) {
        const u8 *layout = *(const u8 **)((u8 *)AT(ga, 0x30, u8 *) + (u64)i * 8);
        u64 pos = *(const u64 *)((u8 *)AT(ga, 0x38, u8 *) + (u64)i * 8);
        int ix = (int)(u32)pos - rect->left;
        int iy = (int)(u32)(pos >> 32) - rect->top;
        O_DrawAliasedLMR(layout, ix, iy, color, out, width, (u32)(bottom - top));
    }

    if (area < bufSize) memset(out + area, 0, bufSize - area);
}

/* 0x180015dd0  DWriteGlyphRunAnalysis::CreateAlphaTexture (dispatcher) */
TT_EXPORT long tt_DWriteCreateAlphaTexture(void *self, u32 texType, void *rectp, u8 *buf, u32 bufSize) {
    if (rectp == 0) return 0x80070057;
    RECT4 *rect = (RECT4 *)rectp;
    u32 saved = _mm_getcsr();
    if ((saved & 0xff80) != 0x1f80) _mm_setcsr(0x1f80);
    if (rect->right <= rect->left || rect->bottom <= rect->top) {
        if ((saved & 0xff80) != 0x1f80) _mm_setcsr(saved & 0xffffffc0);
        return 0x80070057;
    }
    if (texType == 0) O_CreateGray((u8 *)self + 8, rect, buf, bufSize);
    else if (texType == 1) O_Create3x1((u8 *)self + 8, rect, buf, bufSize);
    else { if ((saved & 0xff80) != 0x1f80) _mm_setcsr(saved & 0xffffffc0); return 0x80070057; }
    if ((saved & 0xff80) != 0x1f80) _mm_setcsr(saved & 0xffffffc0);
    return 0;
}
