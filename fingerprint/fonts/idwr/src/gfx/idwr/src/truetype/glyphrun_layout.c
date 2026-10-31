/*
 * IDWriteGlyphRunAnalysis glyph layout and the glyph bitmap cache, ported from DWrite.dll
 * 10.0.26100.9711: GlyphRunAnalysis::GlyphRunAnalysis (positions), the bounds getters, the
 * GlyphDataElement<GlyphBitmapLayout, ...> cache path that rasterizes and stores each glyph,
 * CachedBitmapWriter (the GlyphBitmapLayout encoder) and the IDWriteGlyphLookupCache path.
 * Callees outside this file are called through ORIG; each export can be swapped alone
 * (reimpl/glyphrun_check.py, reimpl/alpha.py).
 *
 * ==== 1. Positioning (GlyphRunAnalysis::GlyphRunAnalysis 0x1800fa644) ====
 *
 * Inputs: run (face, em, indices, advances, offsets, isSideways, bidiLevel), scale =
 * pixelsPerDip (1 for IDWriteFactory2+), transform M = {m11 m12 m21 m22 dx dy}, rendering mode
 * (DWRITE_RENDERING_MODE2), keep_mode, measuring, grid fit, antialias, origin (x, y).
 * All arithmetic is single precision as written; "rhu(v)" is the original's round half up:
 * i = cvtss2si(v) (round to nearest even); if ((float)i - v == -0.5f) i++.
 *
 * a. Uniform transforms fold away: if |m12| < 2^-16, |m21| < 2^-16 and |m22 - m11| < 2^-16
 *    (strict, false for NaN), then x += dx / m11, y += dy / m11, scale *= m11, M = none
 *    (m11 may be negative: a negative scale).
 * b. attr = table 0x1801d8420[aa][mode] (notes/rasterizer.md): byte 0 = overscale ox | oy << 4,
 *    byte 1 = log2 of it, bits 16 ClearType, 17, 19 GDI-compatible, 20 natural, 21 GDI natural,
 *    23 strikes, byte 3 = raster type << 2 | ClearType filter margin.
 * c. Font matrix (16.16, y down) and the strike ppem byte:
 *    - no M: s = em * scale; f = round_em ? rhu(s) << 16 : FloatToFixed(s), where round_em =
 *      attr bit 19 or head.flags bit 3 (face flag 0x10). Not sideways: {f, 0, 0, f};
 *      sideways: {0, -f, f, 0}. ppem byte = low byte of |f| >> 16 when |f| <= 0xffffff (not
 *      clamped: 300 px gives 44), else 0.
 *    - M: rows r0 = (s, 0), r1 = (0, s) (sideways: r0 = (0, -em*scale), r1 = (s, 0));
 *      a = r0y*m21 + r0x*m11, b = r0y*m22 + r0x*m12, c = r1y*m21 + r1x*m11,
 *      d = r1y*m22 + r1x*m12. If axis aligned (b ~ 0 and c ~ 0: p = a, q = d; or b != 0,
 *      a ~ 0, d ~ 0: p = b, q = c; ~ = |v| < 2^-16) and |p| ~ |q|: p >= 32768 throws,
 *      ip = rhu(|p|), ppem byte = ip if ip <= 255 else 0, and with round_em the matrix is
 *      scaled by (float)ip / |p|. Fixed matrix = {F(a), F(-c), F(-b), F(d)}.
 *    FloatToFixed(v) = clamp32((int64)trunc(v * 65536 * 65536) + 0x8000 >> 16).
 * d. Natural mode (4) whose fixed matrix has both diagonal and off-diagonal entries, with
 *    keep_mode = 0, switches to mode 5 (natural symmetric), or 7 (downsampled) when
 *    scale * 96 * 100 >= 17280 (172.8 dpi; NaN gives 5).
 * e. Grid fit DEFAULT -> DISABLED or ENABLED: sc = (scale*96)/96; kind: no M (and sc not NaN)
 *    = 1 (3 sideways); else M' = sc*M classified: m12'~0 & m21'~0 -> 1, sc = max(|m22'|,
 *    |m11'|); m12'!=0, m11'~0, m22'~0 -> 3, sc = max(|m21'|, |m12'|) (both ^2 when sideways);
 *    otherwise 4/6 (one zero off-diagonal / zero diagonal) or 0, sc =
 *    GetTransformMaxScaleFactor(M'). kind & 5 == 0 -> DISABLED. Else attr bit 19 -> ENABLED;
 *    no bit 20 -> DISABLED; no face list at +0x128 -> ENABLED; else the ppem rhu(sc * em),
 *    clamped to [1, 0xffff], is looked up in the sorted u16 list (count at [face+0x108]+0x34,
 *    lower bound): an odd index means ENABLED (gasp SYMMETRIC_GRIDFIT ranges).
 * f. RenderingFlags: bold sim -> 2, oblique -> 4 (| 8 sideways), ENABLED -> 1; with 1: attr
 *    bit 19 -> 0x20 (0x10 if bit 21), strikes (bit 23, cleared unless the face is legacy East
 *    Asian, or overscale 1x1) -> 0x40; attr bit 17 -> 0x80. The GlyphBitmapRasterizationParameters
 *    (the cache key, 0x20 bytes) = {fixed matrix, rhu(em * 0.75), ox, oy, flags, ppem, 0,0,0}.
 * g. GetGlyphData fills layouts[i]; any/all = OR/AND of the layout flags (header >> 26):
 *    flag 4 forces the grayscale (1x1) texture; flag 1 (embedded bitmap) glyphs are moved
 *    after the others (PartitionArray), n = count of the rest; they are drawn aliased.
 * h. ComputeGlyphOrigins (not ported) gives float origins in DIPs. If any glyph has flag 2,
 *    or attr bit 19: for those glyphs (all, with bit 19) x = rhu(scale*x) / scale (multiplied
 *    by 1/scale) when |scale*x| < 2^23 (whole-pixel origins).
 * i. Glyph bitmap positions (overscaled units): px = rhu(ox*scale * x) + layout.left,
 *    py = rhu(oy*scale * y) - layout.top; horizontal runs (not sideways) use the first glyph's
 *    y for every glyph (ComputeGlyphBitmapPositionsInternal<1>). The run rect is the union of
 *    [px, px + width) x [py, py + height) over non-empty layouts; with oy > 1 its top is floored
 *    and its bottom ceiled to a multiple of oy. The aliased glyphs [n, count) get the same at
 *    1x1 into the second rect. An empty rect collapses to the origin (transformed by M):
 *    rhu(x*scale) * ox, rhu(y*scale) * oy.
 * j. 2x2 (natural symmetric downsampled) runs of 2+ glyphs: glyphs with (px ^ py) odd are
 *    moved first and rasterized again with flag 0x100 (the odd 2x2 dither phase).
 *
 * GetAlphaTextureBounds: the texture type must match attr bit 16 (else an empty rect; types
 * other than 0/1 give E_INVALIDARG). ClearType widens a non-empty rect by the margin
 * (attr >> 24 & 3) on both sides, then left/top are floored and right/bottom ceiled to whole
 * pixels (shifts when the log2 byte is set, else IDIV); a non-empty result is united with a
 * non-empty aliased rect, an empty one is replaced by it. GetRunBitmapBounds: type 0 = the
 * aliased rect, the run's raster type = the same conversion without the union.
 *
 * ==== 2. Rasterizing a glyph (GlyphBitmapRasterizationState) ====
 *
 * The state constructor takes the parameters by value: 2x2 is rasterized at 4x4 (ox = oy = 4)
 * and downsampled (DownSample2x2, not ported: each 2x2 block of the 4x4 bitmap -> 1 bit through
 * g_convert4x4BitsTo2x2 or its other-phase table, phase alternating per output row and starting
 * at ((top+1)>>1 ^ left>>1 ^ odd flag) & 1). spread = 1 ("emboldening" of thin fonts) when face
 * flag 0x60 bit 2 is set, ox > 1, weight <= 500 and no bold/strike flag (flags & 0x42 = 0).
 * RasterizeGlyph: IFontRasterizer NewGlyph + GetBitmap into {left, top, width, height, flags,
 * stride, bits} (1 bit per subpixel, LSB first, rows top down, stride a multiple of 4); an
 * empty or too large bitmap (width > 0xffff - spread, height > 0xffff, w*h > 0x800000)
 * becomes empty; then CachedBitmapWriter::Pack.
 *
 * ==== 3. GlyphBitmapLayout (16-byte header + data) ====
 *
 *   +0  u32  bits 0-25 data size in bytes; bits 26-31 = (writer flags | bitmap flags) << 26:
 *            bits 26-28 are the rasterizer's GlyphBitmap flags, named here by how the
 *            constructor uses them: 26 (1) drawn by the aliased overlay (embedded bitmaps,
 *            notes/rasterizer.md), 27 (2) whole-pixel origin, 28 (4) forces the grayscale
 *            texture; then 29 (8) 16-bit run entries, 30 (0x10) uncompressed,
 *            31 (0x20) 4x4 cell layout (set for every 4x4 glyph; only changes the data
 *            when uncompressed)
 *   +4  s16 left, +6 s16 top (overscaled units, y down: top = rows above the origin)
 *   +8  u16 width (bitmap width + spread), +10 u16 height (rows)
 *   +12 u32 element id (cache consistency check)
 *   +16 data
 *
 * Compressed (bit 30 clear): a sequence of 8-bit (16-bit with bit 29) entries per row:
 *   - an odd entry 2k+1 before a row: the row that follows is repeated k more times;
 *   - the row header 2m: m entries follow, alternating skip (clear pixels from the end of
 *     the previous run, the first from x = 0) and length of a run of set pixels; when m is
 *     odd the last run extends to the end of the row (width, which includes the spread);
 *     m = 0 is an empty row. With spread, each run of set pixels is lengthened by spread
 *     and runs closer than that merge (CountBits).
 *   The coder stops a row when the rest is clear, or when a run reaches the end of the row.
 * Uncompressed (bit 30): chosen when its size <= 3 x the compressed size (bytes).
 *   - 1-bit: rows of ceil((width + spread) / 32) dwords, the source OR-ed with itself
 *     shifted left by spread (carry into the next dword).
 *   - 4x4 (bit 31): ceil(height / 4) cell rows of Align32(ceil4(width + spread) * 4) bits;
 *     dword k holds the 4x4 cells of subpixel columns 8k..8k+3 (bits 0-15) and 8k+4..8k+7
 *     (bits 16-31), bit = 4 * (row & 3) + column within the cell.
 * The data size of an empty glyph is 0 (header only). GlyphBitmapLayout::defaultLayout_
 * (all zero) is used for glyphs outside the element's page table.
 *
 * ==== 4. Cache structures ====
 *
 * GetGlyphData hashes (FontFaceKey, parameters) with Jenkins lookup3 (seeded by HashBlob of
 * the key blobs) and asks the cache context (vtable +0x38) for the element; existing glyphs
 * come from a two-level table (per 64 glyphs a 64-entry table of offsets into the cache
 * block), missing ones are rasterized (AddGlyphs) and appended to the cache file by
 * AddGlyph (CacheWriter, 4-byte aligned, committed by pages). IDWriteGlyphLookupCache keeps
 * its own heap copies (Rasterizer<BitmapTraits>::RasterizeGlyph: list node + layout) in 64-glyph
 * pages of LookupCacheBitmapEntry objects keyed by the same parameters (built by
 * GetLookupCacheBitmapEntry like step 1c-f, but with em in pixels, no scale, no folding and
 * an explicit grid fit). These are ported as-is: their pointers stay owned by the original's
 * allocators.
 */
#include <string.h>
#include "fatal.h"
#include <stdlib.h>
#define TT_NEED_WINDOWS
#include "portable.h"   /* windows.h and the SSE intrinsics, or stand-ins elsewhere */
#include "tt_state.h"
#include "orig.h"
#ifdef IDWR_STANDALONE
/* GlyphBitmapRasterizationState's constructor (0x180053824) and RasterizeGlyph (0x18005225c)
 * each have one C++ catch clause, catch (FileFormatException const &) (their FH4 tables):
 *   - the constructor's try block covers CreateRasterizer and IFontRasterizer::NewTransform
 *     (ip 0x1800538c7..0x180053912, state 2); the handler (0x18018a710) clears the rasterizer,
 *     so every glyph of the run is then empty;
 *   - RasterizeGlyph's covers NewGlyph, GetBitmap and DownSample2x2 (state 0); the handler
 *     (0x18018a35c) zeroes the GlyphBitmap, and the glyph is packed empty.
 * A transform the scaler refuses (fs__NewTransformation fails, NewTransform throws
 * FileFormatException: a GDI-measured em that rounds to 0 ppem, for example) thus gives an
 * analysis with empty glyphs, not an error. In standalone mode the scaler's FileFormatException
 * is idwr_raise(DWRITE_E_FILEFORMAT); other codes are not caught. (Oracle mode calls DWrite's
 * own rasterizer, whose C++ exceptions these C ports cannot catch: the clauses exist only
 * here.) */
#include "native.h"
#define TT_HR_FILEFORMAT ((int)0x88985000)
#endif

/* single precision, evaluated exactly as written (no FMA contraction) */
#pragma clang fp contract(off)

/* ---------------------------------------------------------------------------------------- */
/* originals called by this file (RVAs in DWrite.dll 10.0.26100.9711)                         */
/* ---------------------------------------------------------------------------------------- */
#define RVA_Exception_ctor              0xaa7f0   /* Exception::Exception(int hr, uint) */
#define RVA_LogAndThrow_Argument        0xfb6cc   /* LogAndThrow<ArgumentException> */
#define RVA_LogAndThrow_ArgOutOfRange   0x10bf54  /* LogAndThrow<ArgumentOutOfRangeException> */
#define RVA_SafeIntOnOverflow           0xfa5cc   /* SafeIntExceptionHandler<Exception>::SafeIntOnOverflow */
#define RVA_GetRenderingModeAttributes  0xfb630
#define RVA_GetTransformMaxScaleFactor  0x75424
#define RVA_ComputeGlyphOrigins         0x49844
#define RVA_PartitionAliased            0xfb708   /* PartitionArray<lambda_69a36881...> */
#define RVA_PartitionOddPhase           0x151b8c  /* PartitionArray<lambda_c2cf278b...> */
#define RVA_ComputeGlyphBitmapPositions 0xc6b08
#define RVA_ComputeGlyphBitmapPositionsInternal1 0xc6cac
#define RVA_GetGlyphBitmaps             0x55fbc
#define RVA_GetGlyphData                0x54314   /* GlyphDataElement<GlyphBitmapLayout,...>::GetGlyphData */
#define RVA_ModeAttributeTable          0x1d8420  /* uint32[2][8]: [antialias][DWRITE_RENDERING_MODE2] */
/* glyph data cache (GlyphDataElement<GlyphBitmapLayout, GlyphBitmapRasterizationParameters,
 * GlyphBitmapRasterizationState>) */
#define RVA_GetExistingGlyphs           0x534e8
#define RVA_AddGlyphs                   0x539bc
#define RVA_AddGlyph                    0x19624
#define RVA_State_ctor                  0x53824   /* GlyphBitmapRasterizationState::GlyphBitmapRasterizationState */
#define RVA_State_RasterizeGlyph        0x5225c
#define RVA_State_GetCachedGlyphDataSize 0x19910
#define RVA_State_FillGlyphData         0x19934
#define RVA_Writer_Pack                 0x87210   /* CachedBitmapWriter::Pack */
#define RVA_Writer_StoreUncompressed    0x19ac4
#define RVA_Writer_StoreUncompressed4x4 0xc78fc
#define RVA_ReadFromCache               0xc9590   /* GlyphBitmapLayout::ReadFromCache */
#define RVA_HashBlob                    0x55380
#define RVA_GlyphKey_vftable            0x199788  /* GlyphDataElementKey<GlyphBitmapLayout,...>::`vftable' */
#define RVA_DefaultLayout               0x1c3950  /* GlyphBitmapLayout::defaultLayout_ */
#define RVA_OnOutOfRange                0x108c10  /* FailAsExceptionPolicy<InvalidCacheDataException>::OnOutOfRange */
#define RVA_CaptureStack                0x1cf60
#define RVA_LogAndThrow_InvalidCache    0x10bf90
#define RVA_IntegerOverflowException    0x10c4f8  /* IntegerOverflowException::IntegerOverflowException */
#define RVA_LogAndThrow_IntOverflow     0xfa608
#define RVA_FailAssert                  0x10cb50
#define RVA_PtrArray_push_back          0xacecc   /* IntrusivePointerArray<ICacheReference,...>::push_back */
#define RVA_ConstrainGlyphIndex         0xa6c44
#define RVA_VecU16_Reallocate           0xf61e0   /* std::vector<uint16_t>::_Reallocate<0> */
#define RVA_VecU16_EmplaceReallocate    0xf7dc0   /* std::vector<uint16_t>::_Emplace_reallocate */
#define RVA_VecU16_Tidy                 0x1008c4  /* std::vector<(2-byte element)>::_Tidy */
#define RVA_Align                       0x195f0   /* Align<unsigned int> */
#define RVA_CacheWriter_AllocateDataAligned 0x51fe8
#define RVA_CacheOverflowException      0xf23dc
#define RVA_TI_CacheOverflowException   0x22db80
#define RVA_CxxThrowException           0x11816e
#define RVA_ThrowLastError              0x3e684   /* OSException::ThrowLastError */
#define RVA_CreateRasterizer            0x910f0   /* FontRasterizer::CreateRasterizer(FontFaceReference const &) */
#define RVA_GetFontWeight               0x24a54   /* FontFaceLight::GetFontWeight */
#define RVA_DownSample2x2               0xcdde8
#define RVA_CountBits                   0x87730

#define HR_E_INVALIDARG ((int32_t)0x80070057)

static void safeint_overflow(void) { ORIG(void (*)(void), RVA_SafeIntOnOverflow)(); }

/* Exception::Exception + LogAndThrow<...>(exception, EventTag, line): does not return */
static void throw_hr(uint32_t thrower_rva, int32_t hr, uint64_t tag, uint32_t line) {
    uint8_t ex[64];
    ORIG(void (*)(void *, int32_t, uint32_t), RVA_Exception_ctor)(ex, hr, 0);
    ORIG(void (*)(void *, uint64_t, uint32_t), thrower_rva)(ex, tag, line);
    IDWR_FATAL("glyphrun_layout.c:221");
}

/* ---------------------------------------------------------------------------------------- */
/* float helpers, bit-exact with the original's SSE sequences                                */
/* ---------------------------------------------------------------------------------------- */

/* cvtss2si with the current MXCSR rounding (nearest even by default) */
static inline int32_t cvtss2si(float v) { return _mm_cvtss_si32(_mm_set_ss(v)); }

/* The original's round-half-up idiom: i = cvtss2si(v); if ((float)i - v == -0.5) i++ */
static inline int32_t round_half_up(float v) {
    int32_t i = cvtss2si(v);
    if ((float)i - v == -0.5f) i++;
    return i;
}

/* IsZeroWithTolerance 0x18004c348: -2^-16 < v < 2^-16 (false for NaN) */
static inline int is_zero_tol(float v) { return 1.52587890625e-05f > v && v > -1.52587890625e-05f; }

/* Fixed16_16::FloatToFixedInt32 0x18004c304: v * 65536 * 65536 truncated to int64 (NaN or out
 * of range gives INT64_MIN), + 0x8000, arithmetic >> 16, clamped to the int32 range. */
static inline int32_t float_to_fixed(float v) {
    v = v * 65536.0f;
    v = v * 65536.0f;
    int64_t t = _mm_cvttss_si64(_mm_set_ss(v));
    t = (t + 0x8000) >> 16;
    if (t < INT32_MIN) return INT32_MIN;
    if (t > INT32_MAX) return INT32_MAX;
    return (int32_t)t;
}

static inline float fabs_bits(float v) {
    union { float f; uint32_t u; } x = { v };
    x.u &= 0x7fffffffu;
    return x.f;
}

/* maxss a, b: a > b ? a : b (b when unordered) */
static inline float maxss(float a, float b) { return a > b ? a : b; }

/* ---------------------------------------------------------------------------------------- */
/* GlyphBitmapLayout header                                                                   */
/* ---------------------------------------------------------------------------------------- */
#define GBL_HDR(l)    AT(l, 0x0, uint32_t)
#define GBL_LEFT(l)   AT(l, 0x4, int16_t)
#define GBL_TOP(l)    AT(l, 0x6, int16_t)
#define GBL_WIDTH(l)  AT(l, 0x8, uint16_t)
#define GBL_HEIGHT(l) AT(l, 0xa, uint16_t)
#define GBL_ID(l)     AT(l, 0xc, uint32_t)

/* ---------------------------------------------------------------------------------------- */
/* GlyphRunAnalysis fields (DWriteGlyphRunAnalysis = vtable + GlyphRunAnalysis at +8)         */
/* ---------------------------------------------------------------------------------------- */
#define GRA_COUNT(g)     AT(g, 0x28, uint32_t)
#define GRA_LAYOUTS(g)   AT(g, 0x30, uint8_t **)      /* GlyphBitmapLayout const *[count] */
#define GRA_ORIGINS(g)   AT(g, 0x38, int32_t *)       /* PointF[count], then int pixel positions */
#define GRA_INDICES(g)   AT(g, 0x40, uint16_t *)      /* copy of the glyph indices (2x2 only) */
#define GRA_NOUTLINE(g)  AT(g, 0x48, uint32_t)        /* glyphs [0, n) merged, [n, count) aliased */
#define GRA_RECT(g)      ((int32_t *)((uint8_t *)(g) + 0x4c)) /* overscaled run bounds */
#define GRA_ARECT(g)     ((int32_t *)((uint8_t *)(g) + 0x5c)) /* aliased-glyph bounds, pixels */
#define GRA_ATTR(g)      AT(g, 0x88, uint32_t)        /* rendering mode attributes */
#define GRA_AA(g)        AT(g, 0x8c, uint32_t)
#define GRA_GRAY(g)      AT(g, 0x90, uint8_t)
#define GRA_FACEBIT2(g)  AT(g, 0x91, uint8_t)

/* ---------------------------------------------------------------------------------------- */
/* ComputeGlyphBitmapPositionsInternal<1> 0x1800c6cac                                         */
/* Horizontal run: every glyph takes the first glyph's y.                                     */
/* ---------------------------------------------------------------------------------------- */
TT_EXPORT void tt_ComputeGlyphBitmapPositionsInternal1(uint8_t **layouts, uint32_t count, float scale,
                                                       uint32_t ox, uint32_t oy, int32_t *origins,
                                                       int32_t *rect) {
    float sx = (float)(int64_t)ox * scale;
    float sy = (float)(int64_t)oy * scale;
    float y0f = sy * ((float *)origins)[1];
    int32_t y0 = round_half_up(y0f);
    int32_t left = 0x7fffffff, top = 0x7fffffff, right = (int32_t)0x80000000, bottom = (int32_t)0x80000000;
    for (uint64_t i = 0; i < count; i++) {
        uint8_t *l = layouts[i];
        int32_t x = round_half_up(sx * ((float *)origins)[2 * i]) + GBL_LEFT(l);
        int32_t y = y0 - GBL_TOP(l);
        origins[2 * i] = x;
        origins[2 * i + 1] = y;
        if (GBL_WIDTH(l) != 0 && GBL_HEIGHT(l) != 0) {
            int32_t r = GBL_WIDTH(l) + x, b = GBL_HEIGHT(l) + y;
            if (x < left) left = x;
            if (y < top) top = y;
            if (r > right) right = r;
            if (b > bottom) bottom = b;
        }
    }
    rect[0] = left; rect[1] = top; rect[2] = right; rect[3] = bottom;
}

/* ComputeGlyphBitmapPositions 0x1800c6b08: horizontal -> Internal<1>, else per-glyph y */
TT_EXPORT void tt_ComputeGlyphBitmapPositions(uint8_t **layouts, uint32_t count, float scale, uint32_t ox,
                                              uint32_t oy, uint8_t horizontal, int32_t *origins, int32_t *rect) {
    if (horizontal) {
        ORIG(void (*)(uint8_t **, uint32_t, float, uint32_t, uint32_t, int32_t *, int32_t *),
             RVA_ComputeGlyphBitmapPositionsInternal1)(layouts, count, scale, ox, oy, origins, rect);
        return;
    }
    float sx = (float)(int64_t)ox * scale;
    float sy = (float)(int64_t)oy * scale;
    int32_t left = 0x7fffffff, top = 0x7fffffff, right = (int32_t)0x80000000, bottom = (int32_t)0x80000000;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *l = layouts[i];
        int32_t x = round_half_up(sx * ((float *)origins)[2 * i]);
        int32_t y = round_half_up(sy * ((float *)origins)[2 * i + 1]);
        x += GBL_LEFT(l);
        y -= GBL_TOP(l);
        origins[2 * i] = x;
        origins[2 * i + 1] = y;
        if (GBL_WIDTH(l) != 0 && GBL_HEIGHT(l) != 0) {
            int32_t r = GBL_WIDTH(l) + x, b = GBL_HEIGHT(l) + y;
            if (x < left) left = x;
            if (y < top) top = y;
            if (r > right) right = r;
            if (b > bottom) bottom = b;
        }
    }
    rect[0] = left; rect[1] = top; rect[2] = right; rect[3] = bottom;
}

/* ---------------------------------------------------------------------------------------- */
/* Bounds                                                                                     */
/* ---------------------------------------------------------------------------------------- */

/* floor(v / d) and ceil(v / d) as the original writes them (IDIV, d > 0) */
static inline int32_t floor_div(int32_t v, int32_t d) {
    int32_t r = v % d;
    if (r < 0) v -= d;
    return (v - r) / d;
}
/* RoundUpToMultiple 0x180014844 */
static inline int32_t round_up_multiple(int32_t v, uint16_t d) {
    int32_t r = v % (int32_t)d;
    if (r > 0) return ((int32_t)d - r) + v;
    return v - r;
}

/* Overscaled run rect (+0x54 of DWriteGlyphRunAnalysis) to pixels: ClearType filter margin,
 * then floor/ceil by the overscale (shifts when the log2 byte is set). */
static void run_rect_to_pixels(uint32_t attr, int32_t *r) {
    uint32_t lx = attr >> 8 & 0xf;
    if (lx != 0) {
        r[0] >>= lx;
        r[2] = (int32_t)(r[2] + ((attr & 0xf) - 1)) >> lx;
        uint32_t oy = attr >> 4 & 0xf;
        if (oy > 1) {
            uint32_t ly = attr >> 12 & 0xf;
            r[1] >>= ly;
            r[3] = (int32_t)(oy + r[3] - 1) >> ly;
        }
    } else {
        int32_t ox = attr & 0xf;
        r[0] = floor_div(r[0], ox);
        r[2] = round_up_multiple(r[2], (uint16_t)ox) / ox;
        int32_t oy = attr >> 4 & 0xf;
        if (oy > 1) {
            r[1] = floor_div(r[1], oy);
            r[3] = round_up_multiple(r[3], (uint16_t)oy) / oy;
        }
    }
}

/* DWriteGlyphRunAnalysis::GetAlphaTextureBounds 0x1800c6100 */
TT_EXPORT int32_t tt_GetAlphaTextureBounds(uint8_t *self, int32_t type, int32_t *out) {
    if (type != 0 && type != 1) {
        memset(out, 0, 16);
        return HR_E_INVALIDARG;
    }
    uint32_t attr = AT(self, 0x90, uint32_t);
    if ((uint32_t)type != (attr >> 16 & 1)) {   /* texture type must match ClearType-ness */
        memset(out, 0, 16);
        return 0;
    }
    int32_t r[4];
    memcpy(r, self + 0x54, 16);
    if ((attr >> 16 & 1) && r[2] > r[0] && r[3] > r[1]) {
        uint32_t margin = attr >> 24 & 3;
        r[0] -= margin;
        r[2] += margin;
    }
    run_rect_to_pixels(attr, r);
    const int32_t *a = (const int32_t *)(self + 0x64);
    if (r[2] > r[0] && r[3] > r[1]) {
        if (a[2] > a[0] && a[3] > a[1]) {   /* union with the aliased glyphs' rect */
            if (a[3] >= r[3]) r[3] = a[3];
            if (r[1] >= a[1]) r[1] = a[1];
            if (r[0] >= a[0]) r[0] = a[0];
            if (a[2] >= r[2]) r[2] = a[2];
        }
        memcpy(out, r, 16);
    } else {
        memcpy(out, a, 16);
    }
    return 0;
}

/* DWriteGlyphRunAnalysis::GetRunBitmapBounds 0x1801233d0 (raster type 0 = aliased glyphs) */
TT_EXPORT int32_t tt_GetRunBitmapBounds(uint8_t *self, int32_t type, int32_t *out) {
    if (type == 0) {
        memcpy(out, self + 0x64, 16);
        return 0;
    }
    uint32_t attr = AT(self, 0x90, uint32_t);
    if (type != (int32_t)attr >> 26) {
        memset(out, 0, 16);
        return (uint32_t)type < 6 ? 0 : HR_E_INVALIDARG;
    }
    int32_t r[4];
    memcpy(r, self + 0x54, 16);
    if ((attr >> 16 & 1) && !(r[2] <= r[0] || r[3] <= r[1])) {   /* IsRectEmpty */
        uint32_t margin = attr >> 24 & 3;
        r[0] -= margin;
        r[2] += margin;
    }
    run_rect_to_pixels(attr, r);
    memcpy(out, r, 16);
    return 0;
}

/* ---------------------------------------------------------------------------------------- */
/* GlyphRunAnalysis::GlyphRunAnalysis 0x1800fa644                                            */
/* ---------------------------------------------------------------------------------------- */

/* GlyphBitmapRasterizationParameters, 0x20 bytes: the cache key of a glyph bitmap set */
typedef struct {
    int32_t m[4];        /* 16.16 transform: m00, m01, m10, m11 (y down) */
    int32_t point_size;  /* round half up (em * 0.75) */
    uint16_t ox, oy;     /* overscale */
    uint32_t flags;      /* FontRasterizer::RenderingFlags */
    uint8_t ppem;        /* strike ppem byte (0 when unknown or too large) */
    uint8_t pad[3];
} RasterParams;

TT_EXPORT uint8_t *tt_GlyphRunAnalysis_ctor(uint8_t *self, void *ctx, uint8_t *face, uint32_t simulations,
                                            const uint8_t *run, float scale, const float *matrix,
                                            int32_t mode, uint32_t keep_mode, int32_t measuring,
                                            int32_t gridfit, int32_t aa, float x, float y, uint8_t *buf) {
    memset(self, 0, 0x28);
    uint32_t count = AT(run, 0xc, uint32_t);
    GRA_COUNT(self) = count;
    AT(self, 0x30, uint8_t *) = buf;
    AT(self, 0x38, uint8_t *) = buf + (uint64_t)count * 8;
    AT(self, 0x40, uint8_t *) = buf + (uint64_t)count * 16;
    GRA_NOUTLINE(self) = 0;
    memset(self + 0x4c, 0, 0x20);
    memset(self + 0x70, 0, 0x18);
    GRA_AA(self) = (uint32_t)aa;
    GRA_GRAY(self) = aa == 1;
    uint8_t face_flags = AT(AT(face, 0, uint8_t *), 0x60, uint8_t);
    GRA_FACEBIT2(self) = face_flags >> 2 & 1;

    /* a transform that is only a uniform scale folds into the scale and the origin */
    if (matrix != NULL) {
        const float t = 1.52587890625e-05f;
        if (t > matrix[1] && matrix[1] > -t && t > matrix[2] && matrix[2] > -t) {
            float d = matrix[3] - matrix[0];
            if (t > d && d > -t) {
                x = x + matrix[4] / matrix[0];
                y = y + matrix[5] / matrix[0];
                scale = scale * matrix[0];
                matrix = NULL;
            }
        }
    }
    if ((uint32_t)mode >= 8)
        throw_hr(RVA_LogAndThrow_Argument, HR_E_INVALIDARG, 0x727474615f72ull, 0xae);
    if ((uint32_t)aa >= 2)
        throw_hr(RVA_LogAndThrow_Argument, HR_E_INVALIDARG, 0x727474615f72ull, 0xb3);
    const uint32_t *table = (const uint32_t *)((uint8_t *)idwr_orig(RVA_ModeAttributeTable));
    uint32_t attr = table[(int64_t)mode + (int64_t)aa * 8];
    GRA_ATTR(self) = attr;
    if ((attr & 0xf) == 0)
        throw_hr(RVA_LogAndThrow_Argument, HR_E_INVALIDARG, 0x6e75725f67ull, 0xee);

    int32_t sideways = AT(run, 0x28, int32_t);
    uint8_t rtl = AT(run, 0x2c, uint8_t) & 1;
    /* GDI-compatible modes and head.flags bit 3 round the em to whole pixels */
    int round_em = (attr >> 19 & 1) || (face_flags & 0x10);
    float run_em = AT(run, 0x8, float);
    float s = run_em * scale;
    int32_t m[4] = {0, 0, 0, 0};
    uint8_t ppem = 0;
    if (matrix == NULL) {
        int32_t f = round_em ? (int32_t)((uint32_t)round_half_up(s) << 16) : float_to_fixed(s);
        if (!sideways) { m[0] = f; m[3] = f; }
        else { m[1] = (int32_t)(0u - (uint32_t)f); m[2] = f; }
        int32_t a = (int32_t)(0u - (uint32_t)f);
        if (a < 0) a = f;                     /* neg; cmovs */
        if (a <= 0xffffff) ppem = (uint8_t)(a >> 16);   /* low byte of ppem, not clamped */
    } else {
        float r0x, r0y, r1x, r1y;             /* em matrix rows before the transform */
        if (!sideways) { r0x = s; r0y = 0.0f; r1x = 0.0f; r1y = s; }
        else { r0x = 0.0f; r0y = -run_em * scale; r1x = s; r1y = 0.0f; }
        float a = matrix[2] * r0y + matrix[0] * r0x;
        float b = matrix[3] * r0y + matrix[1] * r0x;
        float c = matrix[2] * r1y + matrix[0] * r1x;
        float d = matrix[3] * r1y + matrix[1] * r1x;
        int axis = 0;
        float p = 0, q = 0;
        if (is_zero_tol(b)) {
            if (is_zero_tol(c)) { axis = 1; p = a; q = d; }
        } else if (is_zero_tol(a) && is_zero_tol(d)) {
            axis = 1; p = b; q = c;
        }
        if (axis) {
            p = fabs_bits(p);
            q = fabs_bits(q);
            if (is_zero_tol(p - q)) {   /* uniform scale (with rotation by a multiple of 90) */
                if (0.0f > p || p >= 32768.0f)
                    throw_hr(RVA_LogAndThrow_ArgOutOfRange, HR_E_INVALIDARG, 0x736e617274ull, 0xf7);
                int32_t ip = round_half_up(p);
                ppem = (uint32_t)ip > 0xff ? 0 : (uint8_t)ip;
                if (round_em && !is_zero_tol(p)) {
                    float k = (float)(uint32_t)ip / p;
                    a = k * a; b = k * b; c = k * c; d = k * d;
                }
            }
        }
        m[0] = float_to_fixed(a);
        m[2] = float_to_fixed(-b);
        m[1] = float_to_fixed(-c);
        m[3] = float_to_fixed(d);
    }
    int32_t tr[4] = { m[0], m[1], m[2], m[3] };   /* [rbp-0x30], for GetGlyphBitmaps */

    /* natural mode with a rotation off the axes switches to natural symmetric (or its
     * downsampled variant above 172.8 dpi) */
    if ((m[0] != 0 || m[3] != 0) && (m[2] != 0 || m[1] != 0) && mode == 4 && keep_mode == 0) {
        float dpi = scale * 96.0f;
        dpi = dpi * 100.0f;
        int32_t m2 = !(dpi >= 17280.0f) ? 5 : 7;
        attr = ORIG(uint32_t (*)(int32_t, int32_t), RVA_GetRenderingModeAttributes)(m2, aa);
        GRA_ATTR(self) = attr;
    }

    /* grid fit DEFAULT: DISABLED (1) or ENABLED (2) */
    if (gridfit == 0) {
        float dpi = scale * 96.0f;
        float sc = dpi / 96.0f;
        int kind;   /* 1 axis aligned, 3 rotated 90, ^2 when sideways; 4, 6 skewed; 0 general */
        if (dpi == dpi && matrix == NULL) {
            kind = sideways ? 3 : 1;
        } else {
            float M[6] = { sc, 0.0f, 0.0f, sc, 0.0f, 0.0f };
            if (matrix != NULL) {
                M[0] = sc * matrix[0];
                M[1] = sc * matrix[1];
                M[2] = sc * matrix[2];
                M[3] = sc * matrix[3];
            }
            int k = -1;
            float u = 0, v = 0;
            if (is_zero_tol(M[1])) {
                if (is_zero_tol(M[2])) { k = 1; u = M[3]; v = M[0]; }
                else k = 4;
            } else if (is_zero_tol(M[0])) {
                if (is_zero_tol(M[3])) { k = 3; u = M[2]; v = M[1]; }
                else k = 6;
            } else if (is_zero_tol(M[2])) {
                k = 4;
            } else {
                k = is_zero_tol(M[3]) ? 6 : 0;
            }
            if (k == 1 || k == 3) {
                sc = maxss(fabs_bits(u), fabs_bits(v));
                kind = sideways ? (k ^ 2) : k;
            } else {
                kind = k;
                sc = ORIG(float (*)(const float *), RVA_GetTransformMaxScaleFactor)(M);
            }
        }
        float pf = sc * run_em;
        uint32_t pp = (uint32_t)round_half_up(pf);
        gridfit = 1;
        if (kind & 5) {
            if (attr >> 19 & 1) {
                gridfit = 2;
            } else if (attr >> 20 & 1) {
                const uint16_t *base = AT(face, 0x128, const uint16_t *);
                if (base == NULL) {
                    gridfit = 2;
                } else {
                    if (pp > 0xffff) pp = 0xffff;
                    else if (pp < 1) pp = 1;
                    const uint16_t *list = base;
                    int64_t n = AT(AT(face, 0x108, uint8_t *), 0x34, uint32_t);
                    while (n > 0) {   /* lower bound of the ppem */
                        int64_t half = n / 2;
                        if (list[half] < (uint16_t)pp) {
                            list = list + half + 1;
                            n = n - half - 1;
                        } else {
                            n = half;
                        }
                    }
                    if ((uint8_t)((uintptr_t)list - (uintptr_t)base) & 2) gridfit = 2;   /* odd index */
                }
            }
        }
    }

    const uint16_t *indices = AT(run, 0x10, const uint16_t *);
    if ((attr & 0xf) == 2) {   /* 2x2: the indices are reordered with the glyphs later */
        memcpy(GRA_INDICES(self), indices, (uint64_t)count * 2);
        indices = GRA_INDICES(self);
    } else {
        GRA_INDICES(self) = NULL;
    }

    uint32_t flags = 0;
    if (GRA_COUNT(self) != 0) {
        if ((AT(AT(face, 0x108, uint8_t *), 0x14, uint8_t) & 1) == 0)   /* not legacy East Asian */
            GRA_ATTR(self) &= ~0x800000u;
        attr = GRA_ATTR(self);
        flags = (simulations & 1) * 2;
        if (simulations & 2) flags |= (sideways ? 8u : 0u) + 4;
        if (gridfit == 2) flags |= 1;
        if (flags & 1) {
            if (attr >> 19 & 1) flags |= (attr >> 21 & 1) ? 0x10 : 0x20;
            if ((attr >> 23 & 1) || ((attr & 0xf) == 1 && (attr & 0xf0) == 0x10)) flags |= 0x40;
        }
        if (attr >> 17 & 1) flags |= 0x80;
        memset(GRA_LAYOUTS(self), 0, (uint64_t)GRA_COUNT(self) * 8);

        RasterParams rp;
        memset(&rp, 0, sizeof rp);
        memcpy(rp.m, m, 16);
        rp.point_size = round_half_up(run_em * 0.75f);
        rp.ox = attr & 0xf;
        rp.oy = attr >> 4 & 0xf;
        rp.flags = flags;
        rp.ppem = ppem;
        ORIG(void (*)(void *, void *, uint8_t *, RasterParams *, uint32_t, const uint16_t *, uint8_t **),
             RVA_GetGlyphData)(ctx, self, face, &rp, GRA_COUNT(self), indices, GRA_LAYOUTS(self));

        uint32_t any = 0, all = 0xffffffff;
        for (uint32_t i = 0; i < GRA_COUNT(self); i++) {
            uint32_t f = GBL_HDR(GRA_LAYOUTS(self)[i]) >> 26;
            any |= f;
            all &= f;
        }
        if (GRA_COUNT(self) != 0 && (any & 4)) GRA_GRAY(self) = 1;

        int32_t *origins = GRA_ORIGINS(self);
        uint8_t horizontal = 0;
        ORIG(void (*)(void *, uint8_t *, float, float, const float *, int32_t, const uint16_t *, const float *,
                      const void *, int32_t *, uint32_t, uint8_t, uint8_t, uint8_t, float, float, uint8_t *),
             RVA_ComputeGlyphOrigins)(ctx, face, run_em, scale, matrix, measuring, indices,
                                      AT(run, 0x18, const float *), AT(run, 0x20, const void *), origins,
                                      GRA_COUNT(self), sideways != 0, rtl, 1, x, y, &horizontal);

        /* glyphs whose layout has flag bit 0 (embedded bitmaps) go last, drawn aliased */
        uint32_t n;
        if ((any & 1) == 0) n = GRA_COUNT(self);
        else if (all & 1) n = 0;
        else {
            void *lctx[2] = { self, &origins };
            n = ORIG(uint32_t (*)(uint32_t, uint8_t *, void **), RVA_PartitionAliased)(GRA_COUNT(self), self, lctx);
        }
        GRA_NOUTLINE(self) = n;
        if (n != 0) {
            /* flag bit 1 (or GDI-compatible modes): whole-pixel glyph origins */
            if (!((any & 2) == 0 && (GRA_ATTR(self) & 0x80000) == 0)) {
                float rs = 1.0f / scale;
                for (uint32_t i = 0; i < GRA_NOUTLINE(self); i++) {
                    if ((GBL_HDR(GRA_LAYOUTS(self)[i]) & 0x8000000) || (GRA_ATTR(self) & 0x80000)) {
                        float v = scale * ((float *)origins)[2 * i];
                        if (8388608.0f > fabs_bits(v)) v = (float)round_half_up(v);
                        ((float *)origins)[2 * i] = v * rs;
                    }
                }
                n = GRA_NOUTLINE(self);
            }
            uint32_t ox = GRA_ATTR(self) & 0xf, oy = GRA_ATTR(self) >> 4 & 0xf;
            uint8_t **layouts = GRA_LAYOUTS(self);
            if (horizontal) {
                ORIG(void (*)(uint8_t **, uint32_t, float, uint32_t, uint32_t, int32_t *, int32_t *),
                     RVA_ComputeGlyphBitmapPositionsInternal1)(layouts, n, scale, ox, oy, origins, GRA_RECT(self));
            } else {
                int32_t left = 0x7fffffff, top = 0x7fffffff, right = (int32_t)0x80000000, bottom = (int32_t)0x80000000;
                float sx = (float)(int64_t)ox * scale, sy = (float)(int64_t)oy * scale;
                for (uint64_t i = 0; i < n; i++) {
                    uint8_t *l = layouts[i];
                    int32_t px = round_half_up(sx * ((float *)origins)[2 * i]);
                    int32_t py = round_half_up(sy * ((float *)origins)[2 * i + 1]);
                    px = GBL_LEFT(l) + px;
                    py = py - GBL_TOP(l);
                    origins[2 * i] = px;
                    origins[2 * i + 1] = py;
                    if (GBL_WIDTH(l) != 0 && GBL_HEIGHT(l) != 0) {
                        int32_t r = GBL_WIDTH(l) + px, b = GBL_HEIGHT(l) + py;
                        if (left > px) left = px;
                        if (top > py) top = py;
                        if (right < r) right = r;
                        if (bottom < b) bottom = b;
                    }
                }
                int32_t *R = GRA_RECT(self);
                R[0] = left; R[1] = top; R[2] = right; R[3] = bottom;
            }
            /* vertical overscale: the run rows start and end on a whole output pixel */
            uint16_t voy = (uint16_t)(GRA_ATTR(self) >> 4) & 0xf;
            if (voy > 1) {
                int32_t *R = GRA_RECT(self);
                int32_t d = voy;
                int32_t r = R[1] % d;
                int32_t t = R[1] - r;
                if (r < 0) t -= d;
                R[1] = t;
                r = R[3] % d;
                R[3] = r > 0 ? (d - r) + R[3] : R[3] - r;
            }
        }
        if (GRA_NOUTLINE(self) < GRA_COUNT(self)) {
            uint32_t k = GRA_NOUTLINE(self);
            ORIG(void (*)(uint8_t **, uint32_t, float, uint32_t, uint32_t, uint8_t, int32_t *, int32_t *),
                 RVA_ComputeGlyphBitmapPositions)(GRA_LAYOUTS(self) + k, GRA_COUNT(self) - k, scale, 1, 1,
                                                  horizontal, origins + 2 * (uint64_t)k, GRA_ARECT(self));
        }
    }

    /* 2x2 (natural symmetric downsampled): glyphs at an odd (x ^ y) subpixel position are
     * rasterized again with the odd 2x2 phase (flag 0x100). Not for single-glyph runs. */
    if ((GRA_ATTR(self) & 0xf) == 2 && GRA_COUNT(self) > 1) {
        uint32_t n = ORIG(uint32_t (*)(uint32_t, uint8_t *, uint8_t *), RVA_PartitionOddPhase)(GRA_COUNT(self), self, self);
        if (n != 0) {
            memset(GRA_LAYOUTS(self), 0, (uint64_t)n * 8);
            ORIG(void (*)(void *, uint8_t *, int32_t *, uint8_t, float, uint16_t, uint16_t, uint32_t,
                          const uint16_t *, uint32_t, uint8_t **, uint8_t *),
                 RVA_GetGlyphBitmaps)(ctx, face, tr, ppem, run_em, (uint16_t)(GRA_ATTR(self) & 0xf),
                                      (uint16_t)(GRA_ATTR(self) >> 4) & 0xf, flags | 0x100, indices, n,
                                      GRA_LAYOUTS(self), self);
        }
    }

    /* empty runs: both rects collapse to the (transformed) origin */
    if (matrix != NULL) {
        float nx = y * matrix[2] + x * matrix[0];
        nx = nx + matrix[4];
        float ny = x * matrix[1];
        ny = ny + y * matrix[3];
        ny = ny + matrix[5];
        x = nx;
        y = ny;
    }
    int32_t *R = GRA_RECT(self);
    if (R[2] <= R[0] || R[3] <= R[1]) {
        int32_t v = round_half_up(x * scale) * (int32_t)(GRA_ATTR(self) & 0xf);
        R[0] = v; R[2] = v;
        v = round_half_up(y * scale) * (int32_t)(GRA_ATTR(self) >> 4 & 0xf);
        R[1] = v; R[3] = v;
    }
    int32_t *A = GRA_ARECT(self);
    if (A[2] <= A[0] || A[3] <= A[1]) {
        int32_t v = round_half_up(x * scale);
        A[0] = v; A[2] = v;
        v = round_half_up(y * scale);
        A[1] = v; A[3] = v;
    }
    return self;
}

/* ---------------------------------------------------------------------------------------- */
/* Glyph data cache: GlyphDataElement<GlyphBitmapLayout, GlyphBitmapRasterizationParameters,  */
/* GlyphBitmapRasterizationState>                                                           */
/* ---------------------------------------------------------------------------------------- */

static void out_of_range(const void *p) { ORIG(void (*)(const void *), RVA_OnOutOfRange)(p); IDWR_FATAL("glyphrun_layout.c:793"); }

/* CaptureStack, then LogAndThrow<InvalidCacheDataException>({0x88985007, value}) */
static void throw_invalid_cache(void *value, uint32_t line) {
    struct { int32_t hr; int32_t pad; void *p; } ex = { (int32_t)0x88985007, 0, value };
    ORIG(void (*)(int32_t, uint32_t), RVA_CaptureStack)((int32_t)0x88985007, 0);
    ORIG(void (*)(void *, uint64_t, uint32_t), RVA_LogAndThrow_InvalidCache)(&ex, 0x6b636f6c6267ull, line);
    IDWR_FATAL("glyphrun_layout.c:800");
}

static void throw_int_overflow(uint64_t tag, uint32_t line) {
    uint8_t ex[64];
    void *e = ORIG(void *(*)(void *), RVA_IntegerOverflowException)(ex);
    ORIG(void (*)(void *, uint64_t, uint32_t), RVA_LogAndThrow_IntOverflow)(e, tag, line);
    IDWR_FATAL("glyphrun_layout.c:807");
}

static void fail_assert(uint32_t line, const void *p) {
    ORIG(void (*)(uint32_t, const void *), RVA_FailAssert)(line, p);
    IDWR_FATAL("glyphrun_layout.c:812");
}

/* std::vector<uint16_t> {begin, end, capacity}: grown and freed by the original's code */
typedef struct { uint16_t *begin, *end, *cap; } VecU16;
static void vec_push(VecU16 *v, uint16_t value) {
    if (v->end == v->cap)
        ORIG(uint16_t *(*)(VecU16 *, uint16_t *, const uint16_t *), RVA_VecU16_EmplaceReallocate)(v, v->end, &value);
    else
        *v->end++ = value;
}

static inline uint32_t rotl32(uint32_t x, int k) { return x << k | x >> (32 - k); }

/* Jenkins lookup3 mix() and final() */
#define MIX(a, b, c)                               \
    do {                                           \
        a -= c; a ^= rotl32(c, 4);  c += b;        \
        b -= a; b ^= rotl32(a, 6);  a += c;        \
        c -= b; c ^= rotl32(b, 8);  b += a;        \
        a -= c; a ^= rotl32(c, 16); c += b;        \
        b -= a; b ^= rotl32(a, 19); a += c;        \
        c -= b; c ^= rotl32(b, 4);  b += a;        \
    } while (0)
#define FINAL(a, b, c)                             \
    do {                                           \
        c ^= b; c -= rotl32(b, 14);                \
        a ^= c; a -= rotl32(c, 11);                \
        b ^= a; b -= rotl32(a, 25);                \
        c ^= b; c -= rotl32(b, 16);                \
        a ^= c; a -= rotl32(c, 4);                 \
        b ^= a; b -= rotl32(a, 14);                \
        c ^= b; c -= rotl32(b, 24);                \
    } while (0)

/* Hash of (FontFaceKey, GlyphBitmapRasterizationParameters), inlined in GetGlyphData,
 * GetGlyphBitmaps, the key constructor and ComputeHashCode. FontFaceKey: 6 dwords; the 4th
 * is also the byte length of the blob at +0x18, the low word of the 5th the count of
 * 8-byte axis values at +0x20. */
static uint32_t glyph_key_hash(const uint8_t *fk, const uint32_t *p) {
    const uint32_t *k = (const uint32_t *)fk;
    uint32_t a, b, c;
    a = b = c = 0xdeadbf09u;
    a += k[0]; b += k[1]; c += k[2];
    MIX(a, b, c);
    a += k[3]; b += k[4]; c += k[5];
    FINAL(a, b, c);
    uint32_t h = ORIG(uint32_t (*)(const void *, uint64_t, uint32_t), RVA_HashBlob)(
        AT(fk, 0x18, const void *), AT(fk, 0xc, uint32_t), c);
    if (AT(fk, 0x10, uint16_t) != 0)
        h = ORIG(uint32_t (*)(const void *, uint64_t, uint32_t), RVA_HashBlob)(
            AT(fk, 0x20, const void *), (uint64_t)AT(fk, 0x10, uint16_t) << 3, h);
    a = b = c = h + 0xdeaebf0fu;
    a += p[0]; b += p[1]; c += p[2];
    MIX(a, b, c);
    a += p[3]; b += p[4]; c += p[5];
    MIX(a, b, c);
    a += p[6]; b += p[7];
    FINAL(a, b, c);
    return c;
}

/* GlyphDataElementKey<GlyphBitmapLayout,...> (0x28 bytes) */
typedef struct {
    void *vftable;
    uint32_t hash;
    uint32_t one;            /* 1 */
    const uint8_t *fontkey;  /* FontFaceKey (FontFace + 0x20) */
    const void *params;      /* GlyphBitmapRasterizationParameters */
    uint32_t glyph_count;    /* glyphs in the font */
    uint8_t shared;          /* starts as (fontkey qword & INT64_MAX) >= 3 */
    uint8_t pad[3];
} GlyphKey;

/* CachedElementData, filled by IBaseCacheContext::FindElement (vtable + 0x38) */
typedef struct {
    uint32_t id;      uint32_t pad0;
    uint64_t q8;
    const uint8_t *data;     /* element header: +4 glyph count, +8 page table offset */
    uint32_t size;    uint32_t pad1;
    uint64_t q20;
    uint32_t d28;     uint32_t pad2;
    uint8_t *ref;            /* ICacheReference (refcount at +8, lock at +0x20) */
    uint16_t created; uint8_t pad3[6];
} ElementData;

/* GlyphDataElement as AddGlyphs builds it on its stack (0x28 bytes) */
typedef struct {
    const uint32_t *pages;   /* per 64 glyphs: offset of a 64-entry offset table, 0 = none */
    uint32_t glyph_count;
    uint32_t page_count;     /* (glyph_count + 63) >> 6 */
    uint32_t id;
    uint32_t junk;
    const uint8_t *base;     /* from ICacheReference vtable[1] */
    uint32_t size;
    uint32_t junk2;
} ElementView;

static void add_cache_reference(uint8_t *reflist, uint8_t *ref) {
    if (AT(reflist, 0, uint8_t *) == NULL) {
        AT(reflist, 0, uint8_t *) = ref;
        if (ref) InterlockedIncrement((volatile LONG *)(ref + 8));
    } else {
        ORIG(void (*)(uint8_t *, uint8_t *), RVA_PtrArray_push_back)(reflist + 8, ref);
    }
}

typedef struct { const uint8_t *base; uint32_t size; uint32_t pad; } CacheBlock;
static void cache_ref_data(uint8_t *ref, CacheBlock *out) {
    void **vt = AT(ref, 0, void **);
    ((void (*)(uint8_t *, CacheBlock *))vt[1])(ref, out);
}

/* GetExistingGlyphs 0x1800534e8: fills null slots from the cached element; returns
 * start + the number found. */
TT_EXPORT uint32_t tt_GetExistingGlyphs(void *ctx, ElementData *d, uint8_t *reflist, uint32_t start, uint32_t count,
                                        const uint16_t *indices, const uint8_t **layouts) {
    (void)ctx;
    if (d->size < 0x34 || ((uintptr_t)d->data & 3)) out_of_range(d->data);
    const uint8_t *hdr = d->data;
    uint32_t nglyphs = AT(hdr, 4, uint32_t);
    uint32_t id = d->id;
    uint64_t npages = (uint32_t)(nglyphs + 0x3f) >> 6;
    uint32_t off = AT(hdr, 8, uint32_t);
    if ((uint64_t)off > d->size || ((uint64_t)d->size - off) >> 2 < npages || ((uintptr_t)(hdr + off) & 3))
        out_of_range(d->data);
    const uint32_t *pt = (const uint32_t *)(hdr + off);
    CacheBlock blk;
    cache_ref_data(d->ref, &blk);
    uint32_t found = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (layouts[i] != NULL) continue;
        uint16_t g = indices[i];
        if (g >= (uint16_t)nglyphs) {
            if ((uint16_t)nglyphs == 0)
                throw_hr(RVA_LogAndThrow_ArgOutOfRange, HR_E_INVALIDARG, 0x64696763ull, 0x31f);
            g = 0;
        }
        uint32_t po = pt[g >> 6];
        if (po == 0) continue;
        uint64_t size = blk.size;
        if (po > size || size - po < 0x100 || ((uintptr_t)(blk.base + po) & 3)) out_of_range(blk.base);
        const uint8_t *page = blk.base + po;
        uint32_t so = ((const uint32_t *)page)[g & 0x3f];
        if (so == 0) continue;
        if (so > size || size - so < 0x10 || ((uintptr_t)(blk.base + so) & 3)) out_of_range(blk.base);
        const uint8_t *l = blk.base + so;
        uint64_t len = (uint64_t)(GBL_HDR(l) & 0x3ffffff) + 0x10;
        if (size - so < len || so > size) out_of_range(blk.base);
        if (GBL_ID(l) != id) throw_invalid_cache(&page, 0x1a9);
        if (l != NULL) {
            layouts[i] = l;
            found++;
        }
    }
    if (found != 0) {
        uint8_t *ref = d->ref;
        add_cache_reference(reflist, ref);
        AT(reflist, 0x20, uint8_t *) = ref;
    }
    return found + start;
}

/* CheckedPtr<...>::ReadAt<Header> 0x180052208 (identical code folded with the metrics one) */
static const uint8_t *read_header(const ElementData *d) {
    if (d->size < 0x34 || ((uintptr_t)d->data & 3)) out_of_range(d->data);
    return d->data;
}

static void load_element_view(ElementView *e, ElementData *d) {
    const uint8_t *hdr = read_header(d);
    uint32_t nglyphs = AT(hdr, 4, uint32_t);
    uint32_t npages = (nglyphs + 0x3f) >> 6;
    uint32_t off = AT(hdr, 8, uint32_t);
    if ((uint64_t)off > d->size || ((uint64_t)d->size - off) >> 2 < npages || ((uintptr_t)(d->data + off) & 3))
        out_of_range(d->data);
    CacheBlock blk;
    cache_ref_data(d->ref, &blk);
    e->pages = (const uint32_t *)(d->data + off);
    e->glyph_count = nglyphs;
    e->page_count = npages;
    e->id = d->id;
    e->base = blk.base;
    e->size = blk.size;
    e->junk2 = blk.pad;
}

/* AddGlyphs 0x1800539bc: rasterizes every glyph whose slot is still null, stores it in the
 * cache and gives later glyphs with the same (constrained) index the same layout. Returns the
 * number of glyphs added. */
TT_EXPORT uint32_t tt_AddGlyphs(uint8_t *state, ElementData *d, void *ctx, uint8_t *reflist, uint8_t *face, GlyphKey *key,
                                uint32_t start, uint32_t count, const uint16_t *indices, const uint8_t **layouts) {
    VecU16 added = { 0, 0, 0 };
    uint64_t reserve = count - start;
    if (count - start != 0) ORIG(void (*)(VecU16 *, uint64_t *), RVA_VecU16_Reallocate)(&added, &reserve);
    ElementView e;
    memset(&e, 0, sizeof e);
    if ((uint8_t)d->created != 0) load_element_view(&e, d);
    uint16_t nfont = AT(AT(face, 0, uint8_t *), 0x62, uint16_t);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *l = layouts[i];
        if (l != NULL) continue;
        uint16_t g = indices[i];
        if (g >= nfont) {
            if (nfont == 0) throw_hr(RVA_LogAndThrow_ArgOutOfRange, HR_E_INVALIDARG, 0x64696763ull, 0x31f);
            g = 0;
        }
        /* glyphs flagged in the face's bit set at +0x150 (FontFace::IsCompatibleFullWidthGlyph) */
        uint8_t fw = 0;
        const uint8_t *bits = AT(face, 0x150, const uint8_t *);
        if (bits != NULL && g < AT(AT(face, 0, uint8_t *), 0x62, uint16_t)) fw = bits[g >> 3] >> (g & 7) & 1;
        ORIG(void (*)(uint8_t *, uint32_t, uint8_t), RVA_State_RasterizeGlyph)(state, g, fw);
        if (e.pages == NULL) {
            void **vt = *(void ***)ctx;
            ((void (*)(void *, GlyphKey *, ElementData *))vt[3])(ctx, key, d);   /* create the element */
            load_element_view(&e, d);
        }
        l = ORIG(const uint8_t *(*)(ElementView *, uint8_t *, uint32_t, uint8_t *), RVA_AddGlyph)(&e, state, g, d->ref);
        if (l == NULL) {
            l = (const uint8_t *)idwr_orig(RVA_DefaultLayout);
        } else {
            uint8_t *ref = d->ref;
            if (ref != AT(reflist, 0x20, uint8_t *)) {
                add_cache_reference(reflist, ref);
                AT(reflist, 0x20, uint8_t *) = ref;
            }
            vec_push(&added, g);
        }
        layouts[i] = l;
        for (uint32_t j = i + 1; j < count; j++) {
            if (layouts[j] == NULL &&
                ORIG(uint16_t (*)(uint16_t, uint16_t), RVA_ConstrainGlyphIndex)(indices[j], nfont) == g)
                layouts[j] = l;
        }
    }
    uint32_t n = (uint32_t)(added.end - added.begin);
    if (n != 0 && (AT(key->fontkey, 0, uint64_t) & 0x7fffffffffffffffull) < 3) {
        void **vt = *(void ***)ctx;
        ((void (*)(void *, ElementData *, uint16_t *, uint32_t))vt[8])(ctx, d, added.begin, n);
    }
    ORIG(void (*)(VecU16 *), RVA_VecU16_Tidy)(&added);
    return n;
}

/* GetGlyphData 0x180054314: the bitmaps of `count` glyphs for one set of rasterization
 * parameters, from the cache or rasterized now. */
TT_EXPORT void tt_GetGlyphData(void *ctx, uint8_t *reflist, uint8_t *face, const uint32_t *params, uint32_t count,
                               const uint16_t *indices, const uint8_t **layouts) {
    uint16_t nfont = AT(AT(face, 0, uint8_t *), 0x62, uint16_t);
    ElementData d;
    memset(&d, 0, sizeof d);
    const uint8_t *fk = face + 0x20;
    GlyphKey key;
    memset(&key, 0, sizeof key);
    key.hash = glyph_key_hash(fk, params);
    key.one = 1;
    key.vftable = (uint8_t *)idwr_orig(RVA_GlyphKey_vftable);
    key.fontkey = fk;
    key.params = params;
    key.glyph_count = nfont;
    key.shared = (AT(fk, 0, uint64_t) & 0x7fffffffffffffffull) >= 3;
    void **vt = *(void ***)ctx;
    typedef uint8_t (*find_fn)(void *, GlyphKey *, ElementData *);
    typedef uint32_t (*existing_fn)(void *, ElementData *, uint8_t *, uint32_t, uint32_t, const uint16_t *,
                                    const uint8_t **);
    uint32_t have = 0;
    if (((find_fn)vt[7])(ctx, &key, &d) && (uint8_t)d.created == 0)
        have = ORIG(existing_fn, RVA_GetExistingGlyphs)(ctx, &d, reflist, 0, count, indices, layouts);
    if (have < count) {
        key.shared = 1;
        if (d.ref != NULL && ((uint8_t)d.created != 0 || ((find_fn)vt[7])(ctx, &key, &d))) {
            CRITICAL_SECTION *cs = (CRITICAL_SECTION *)(d.ref + 0x20);
            EnterCriticalSection(cs);
            have = ORIG(existing_fn, RVA_GetExistingGlyphs)(ctx, &d, reflist, have, count, indices, layouts);
            LeaveCriticalSection(cs);
        }
        if (have < count) {
            uint32_t pcopy[8];   /* by value: the state constructor may change the overscale */
            memcpy(pcopy, params, 32);
            uint8_t state[0x60];
            ORIG(void (*)(uint8_t *, void *, uint8_t *, uint32_t *), RVA_State_ctor)(state, NULL, face, pcopy);
            ORIG(uint32_t (*)(uint8_t *, ElementData *, void *, uint8_t *, uint8_t *, GlyphKey *, uint32_t, uint32_t,
                              const uint16_t *, const uint8_t **),
                 RVA_AddGlyphs)(state, &d, ctx, reflist, face, &key, have, count, indices, layouts);
            /* ~GlyphBitmapRasterizationState: the writer's vector, then the rasterizer */
            ORIG(void (*)(void *), RVA_VecU16_Tidy)(state + 0x10);
            uint8_t *rast = AT(state, 0, uint8_t *);
            if (rast != NULL) {
                void **rvt = AT(rast, 0, void **);
                ((void (*)(uint8_t *, uint32_t))rvt[0])(rast, 1);
            }
        }
    }
    if (d.ref != NULL && InterlockedDecrement((volatile LONG *)(d.ref + 8)) == 0) {
        void **rvt = AT(d.ref, 0, void **);
        ((void (*)(uint8_t *, uint32_t))rvt[0])(d.ref, 1);
    }
}

/* GlyphBitmapLayout::ReadFromCache 0x1800c9590: the layout at `offset` in a checked block */
TT_EXPORT const uint8_t *tt_ReadFromCache(const CacheBlock *blk, uint32_t offset) {
    uint64_t size = blk->size;
    if ((uint64_t)offset > size || size - offset < 0x10 || ((uintptr_t)(blk->base + offset) & 3))
        out_of_range(blk->base);
    const uint8_t *l = blk->base + offset;
    uint64_t len = (uint64_t)(GBL_HDR(l) & 0x3ffffff) + 0x10;   /* the SafeInt check (< 0x10) cannot fire */
    if (size - offset < len) out_of_range(blk->base);
    return l;
}

/* GlyphDataElement::AddGlyph 0x180019624: stores the state's packed glyph in the cache file
 * (CacheWriter) under the element's two-level table and returns it; a glyph already there is
 * returned as is. */
TT_EXPORT const uint8_t *tt_AddGlyph(ElementView *e, uint8_t *state, uint32_t glyph, uint8_t *writer) {
    uint32_t page = glyph >> 6, slot = glyph & 0x3f;
    if (page >= e->page_count) return (const uint8_t *)idwr_orig(RVA_DefaultLayout);
    CRITICAL_SECTION *cs = (CRITICAL_SECTION *)(writer + 0x20);
    EnterCriticalSection(cs);
    uint32_t committed = AT(writer, 0x7c, uint32_t);
    uint8_t *base = AT(writer, 0x10, uint8_t *);
    uint32_t off = e->pages[page];
    uint32_t *table;
    if (off == 0) {
        table = ORIG(uint32_t *(*)(uint8_t *, uint32_t, uint32_t, uint32_t *), RVA_CacheWriter_AllocateDataAligned)(
            writer, 0x100, 4, &off);
        _mm_sfence();
        ((uint32_t *)e->pages)[page] = off;
    } else {
        if (off > committed || committed - off < 0x100 || ((uintptr_t)(base + off) & 3)) out_of_range(base);
        table = (uint32_t *)(base + off);
    }
    const uint8_t *l;
    if (table[slot] != 0) {
        CacheBlock blk = { base, committed, 0 };
        l = ORIG(const uint8_t *(*)(CacheBlock *, uint32_t), RVA_ReadFromCache)(&blk, table[slot]);
        if (GBL_ID(l) != e->id) throw_invalid_cache((void *)l, 0x224);
    } else {
        uint32_t need = ORIG(uint32_t (*)(uint8_t *), RVA_State_GetCachedGlyphDataSize)(state);
        EnterCriticalSection(cs);
        uint8_t *hdr = AT(writer, 0x48, uint8_t *);
        uint64_t used = AT(hdr, 0x10, uint32_t);
        uint64_t at4 = used + 3;
        if (at4 > 0xffffffffull) safeint_overflow();
        uint32_t at = (uint32_t)at4 & 0xfffffffcu;
        uint32_t end = need + at;
        if (end < at) throw_int_overflow(0x776568636163ull, 0x1c5);
        if (end > AT(writer, 0x7c, uint32_t)) {   /* commit more of the reserved cache file */
            SYSTEM_INFO si;
            memset(&si, 0, sizeof si);
            GetSystemInfo(&si);
            uint32_t newc = ORIG(uint32_t (*)(uint32_t, uint64_t), RVA_Align)(end, si.dwPageSize);
            if (newc < end || newc > AT(writer, 0x78, uint32_t)) {
                uint8_t ex[0x30];
                memset(ex, 0, sizeof ex);
                ORIG(void (*)(void *, uint8_t *, uint64_t), RVA_CacheOverflowException)(
                    ex, writer, (uint64_t)(end - AT(AT(writer, 0x48, uint8_t *), 0x10, uint32_t)));
                ORIG(void (*)(void *, void *), RVA_CxxThrowException)(ex, (uint8_t *)idwr_orig(RVA_TI_CacheOverflowException));
                IDWR_FATAL("glyphrun_layout.c:1169");
            }
            uint32_t oldc = AT(writer, 0x7c, uint32_t);
            if (!VirtualAlloc(AT(writer, 0x10, uint8_t *) + oldc, newc - oldc, MEM_COMMIT, PAGE_READWRITE)) {
                ORIG(void (*)(void), RVA_ThrowLastError)();
                IDWR_FATAL("glyphrun_layout.c:1174");
            }
            AT(writer, 0x7c, uint32_t) = newc;
        }
        uint8_t *dst = AT(writer, 0x10, uint8_t *) + at;
        AT(AT(writer, 0x48, uint8_t *), 0x10, uint32_t) = end;
        LeaveCriticalSection(cs);
        ORIG(void (*)(uint8_t *, uint8_t *, uint32_t), RVA_State_FillGlyphData)(state, dst, 0);
        GBL_ID(dst) = e->id;
        _mm_sfence();
        table[slot] = at;
        l = dst;
    }
    LeaveCriticalSection(cs);
    return l;
}

/* ---------------------------------------------------------------------------------------- */
/* GlyphBitmapRasterizationState (0x60 bytes)                                                 */
/*   +0x00 IFontRasterizer *   +0x08 u8 spread   +0x10 CachedBitmapWriter (0x28 bytes)        */
/*   +0x38 FontRasterizer::GlyphBitmap {left, top, width, height, flags, stride, bits}        */
/*   +0x58 u8 4x4 layout   +0x59 u8 2x2 by downsampling 4x4   +0x5a u8 odd 2x2 phase          */
/* CachedBitmapWriter: +0 std::vector<uint16_t> runs, +0x18 u32 data size, +0x1c u16 flags,  */
/*   +0x1e u8 spread, +0x20 GlyphBitmap * (set when stored uncompressed)                      */
/* ---------------------------------------------------------------------------------------- */
#define ST_RAST(s)    AT(s, 0x00, uint8_t *)
#define ST_SPREAD(s)  AT(s, 0x08, uint8_t)
#define ST_WRITER(s)  ((s) + 0x10)
#define ST_BITMAP(s)  ((s) + 0x38)
#define ST_IS4X4(s)   AT(s, 0x58, uint8_t)
#define ST_DOWN2(s)   AT(s, 0x59, uint8_t)
#define ST_ODD(s)     AT(s, 0x5a, uint8_t)
#define BM_LEFT(b)    AT(b, 0x00, int32_t)
#define BM_TOP(b)     AT(b, 0x04, int32_t)
#define BM_WIDTH(b)   AT(b, 0x08, uint32_t)
#define BM_HEIGHT(b)  AT(b, 0x0c, uint32_t)
#define BM_FLAGS(b)   AT(b, 0x10, uint32_t)
#define BM_STRIDE(b)  AT(b, 0x14, int32_t)
#define BM_BITS(b)    AT(b, 0x18, uint32_t *)
#define WR_VEC(w)     ((VecU16 *)(w))
#define WR_SIZE(w)    AT(w, 0x18, uint32_t)
#define WR_FLAGS(w)   AT(w, 0x1c, uint16_t)
#define WR_SPREAD(w)  AT(w, 0x1e, uint8_t)
#define WR_SRC(w)     AT(w, 0x20, uint8_t *)

/* GlyphBitmapRasterizationState::GlyphBitmapRasterizationState 0x180053824. `p` is the
 * caller's by-value copy of the parameters: 2x2 is rasterized at 4x4 and downsampled. */
TT_EXPORT uint8_t *tt_GlyphBitmapRasterizationState_ctor(uint8_t *s, void *unused, uint8_t *face, uint8_t *p) {
    (void)unused;
    AT(s, 0x00, uint64_t) = 0;
    AT(s, 0x10, uint64_t) = 0;
    AT(s, 0x18, uint64_t) = 0;
    AT(s, 0x20, uint64_t) = 0;
    memset(s + 0x38, 0, 0x20);
    uint16_t *pox = (uint16_t *)(p + 0x14), *poy = (uint16_t *)(p + 0x16);
    uint8_t spread = 0;
    /* face flag bit 2 (thin-stem emboldening): one extra subpixel per run for light fonts */
    if ((AT(AT(face, 0, uint8_t *), 0x60, uint8_t) & 4) && *pox > 1 &&
        ORIG(int32_t (*)(uint8_t *), RVA_GetFontWeight)(face) <= 0x1f4 && (AT(p, 0x18, uint32_t) & 0x42) == 0)
        spread = 1;
    uint16_t ox = *pox;
    uint32_t flags = AT(p, 0x18, uint32_t);
    ST_SPREAD(s) = spread;
    ST_IS4X4(s) = *poy == 4 && ox == 4;
    uint8_t down = ox == 2 && *poy == 2;
    ST_DOWN2(s) = down;
    ST_ODD(s) = flags >> 8 & 1;
    if (down) { *pox = 4; *poy = 4; }
#if defined(IDWR_STANDALONE) && !defined(IDWR_NEGATIVE_CONTROL_NOCATCH)
    idwr_catch c;
    idwr_catch_push(&c);
    if (IDWR_SETJMP(c.buf)) {
        idwr_catch_pop(&c);
        if (c.code != TT_HR_FILEFORMAT) idwr_raise(c.code);
        uint8_t *r = ST_RAST(s);   /* ScopedPtr<IFontRasterizer>::Clear */
        ST_RAST(s) = NULL;
        if (r != NULL) ((void (*)(uint8_t *, uint32_t))AT(r, 0, void **)[0])(r, 1);
        return s;
    }
#endif
    uint8_t *rast = ORIG(uint8_t *(*)(uint8_t *), RVA_CreateRasterizer)(face + 0x98);
    uint8_t *old = ST_RAST(s);
    ST_RAST(s) = rast;
    if (old != NULL) ((void (*)(uint8_t *, uint32_t))AT(old, 0, void **)[0])(old, 1);
    rast = ST_RAST(s);
    /* IFontRasterizer::NewTransform(point size, params, ox, oy, flags, ppem) */
    ((void (*)(uint8_t *, int32_t, uint8_t *, uint16_t, uint16_t, uint32_t, uint8_t))AT(rast, 0, void **)[1])(
        rast, AT(p, 0x10, int32_t), p, *pox, *poy, AT(p, 0x18, uint32_t), AT(p, 0x1c, uint8_t));
#if defined(IDWR_STANDALONE) && !defined(IDWR_NEGATIVE_CONTROL_NOCATCH)
    idwr_catch_pop(&c);
#endif
    return s;
}

/* GlyphBitmapRasterizationState::RasterizeGlyph 0x18005225c */
TT_EXPORT void tt_State_RasterizeGlyph(uint8_t *s, uint32_t glyph, uint8_t fullwidth) {
    uint8_t *bm = ST_BITMAP(s);
    memset(bm, 0, 0x20);
    uint8_t *rast = ST_RAST(s);
    if (rast != NULL) {
#if defined(IDWR_STANDALONE) && !defined(IDWR_NEGATIVE_CONTROL_NOCATCH)
        idwr_catch c;
        idwr_catch_push(&c);
        if (IDWR_SETJMP(c.buf)) {
            idwr_catch_pop(&c);
            if (c.code != TT_HR_FILEFORMAT) idwr_raise(c.code);
            memset(bm, 0, 0x20);
            goto packed;
        }
#endif
        void **vt = AT(rast, 0, void **);
        ((void (*)(uint8_t *, uint32_t, uint8_t))vt[2])(rast, glyph, fullwidth);   /* NewGlyph */
        ((void (*)(uint8_t *, uint8_t *))AT(ST_RAST(s), 0, void **)[3])(ST_RAST(s), bm);   /* GetBitmap */
        if (BM_WIDTH(bm) == 0 || BM_HEIGHT(bm) == 0) {
            BM_WIDTH(bm) = 0;
            BM_HEIGHT(bm) = 0;
            BM_STRIDE(bm) = 0;
        } else if (ST_DOWN2(s) && (BM_FLAGS(bm) & 1) == 0) {
            ORIG(void (*)(uint8_t *, uint8_t), RVA_DownSample2x2)(bm, ST_ODD(s));
        }
#if defined(IDWR_STANDALONE) && !defined(IDWR_NEGATIVE_CONTROL_NOCATCH)
        idwr_catch_pop(&c);
#endif
    }
#if defined(IDWR_STANDALONE) && !defined(IDWR_NEGATIVE_CONTROL_NOCATCH)
packed:
#endif
    /* too large: an empty glyph */
    if ((int32_t)BM_WIDTH(bm) > (int32_t)(0xffff - ST_SPREAD(s)) || (int32_t)BM_HEIGHT(bm) > 0xffff ||
        BM_HEIGHT(bm) * BM_WIDTH(bm) > 0x800000u)
        memset(bm, 0, 0x20);
    ORIG(void (*)(uint8_t *, uint8_t *, uint8_t, uint8_t), RVA_Writer_Pack)(ST_WRITER(s), bm, ST_SPREAD(s), ST_IS4X4(s));
}

/* GlyphBitmapRasterizationState::GetCachedGlyphDataSize 0x180019910: header + data */
TT_EXPORT uint32_t tt_State_GetCachedGlyphDataSize(uint8_t *s) {
    uint32_t n = WR_SIZE(ST_WRITER(s)) + 0x10;
    if (n < 0x10) safeint_overflow();
    return n;
}

/* GlyphBitmapRasterizationState::FillGlyphData 0x180019934: writes the GlyphBitmapLayout */
TT_EXPORT void tt_State_FillGlyphData(uint8_t *s, uint8_t *l, uint32_t unused) {
    (void)unused;
    uint8_t *w = ST_WRITER(s), *bm = ST_BITMAP(s);
    uint32_t size = WR_SIZE(w);
    if (size > 0x3ffffff) throw_int_overflow(0x6b636f6c6267ull, 0x4c7);
    GBL_HDR(l) ^= (GBL_HDR(l) ^ size) & 0x3ffffff;
    GBL_HDR(l) = ((uint32_t)WR_FLAGS(w) | BM_FLAGS(bm)) << 26 | (size & 0x3ffffff);
    if ((uint32_t)BM_LEFT(bm) + 0x8000u > 0xffff) safeint_overflow();
    GBL_LEFT(l) = (int16_t)BM_LEFT(bm);
    if ((uint32_t)BM_TOP(bm) + 0x8000u > 0xffff) safeint_overflow();
    GBL_TOP(l) = (int16_t)BM_TOP(bm);
    if (BM_WIDTH(bm) > 0xffff) safeint_overflow();
    uint32_t wsp = (uint32_t)WR_SPREAD(w) + (uint16_t)BM_WIDTH(bm);   /* width includes the spread */
    if (wsp > 0xffff) safeint_overflow();
    GBL_WIDTH(l) = (uint16_t)wsp;
    if (BM_HEIGHT(bm) > 0xffff) safeint_overflow();
    GBL_HEIGHT(l) = (uint16_t)BM_HEIGHT(bm);
    if (size == 0) return;
    uint8_t *dst = l + 0x10;
    if (WR_SRC(w) != NULL) {
        if (WR_FLAGS(w) & 0x20)
            ORIG(void (*)(uint8_t *, uint8_t *, uint64_t), RVA_Writer_StoreUncompressed4x4)(w, dst, size);
        else
            ORIG(void (*)(uint8_t *, uint8_t *, uint64_t), RVA_Writer_StoreUncompressed)(w, dst, size);
        return;
    }
    VecU16 *v = WR_VEC(w);
    uint64_t n = (uint64_t)(v->end - v->begin);
    if (WR_FLAGS(w) & 8) {   /* 16-bit run entries */
        if (n * 2 != size) fail_assert(0x1f0, (void *)n);
        for (uint64_t i = 0; i < n; i++) ((uint16_t *)dst)[i] = v->begin[i];
    } else {
        if (n != size) fail_assert(0x1fd, (void *)n);
        for (uint64_t i = 0; i < n; i++) dst[i] = (uint8_t)v->begin[i];
    }
}

/* Length of the run of pixels equal to `ones` starting at bit `pos` of a row of `words`
 * dwords (0 when pos is past the row): the loops of CachedBitmapWriter::Pack. */
static uint32_t pack_run(const uint32_t *row, uint32_t words, uint32_t pos, int ones) {
    uint32_t n = 0;
    uint16_t w = (uint16_t)(pos >> 5), b = (uint16_t)(pos & 0x1f);
    if ((uint32_t)w >= words) return 0;
    uint32_t v = ones ? row[w] : ~row[w];
    for (; b < 0x20; b++) {
        if (!(v >> b & 1)) break;
        n++;
    }
    while (b == 0x20) {
        if ((uint32_t)w + 1 >= words) break;
        w++;
        v = ones ? row[w] : ~row[w];
        if (v == 0xffffffffu) { n += 0x20; continue; }
        if ((v & 0xffff) == 0xffff) n += 0x10;
        b = (v & 0xffff) == 0xffff ? 0x10 : 0;
        for (; b < 0x20; b++) {
            if (!(v >> b & 1)) break;
            n++;
        }
    }
    return n;
}

/* CachedBitmapWriter::Pack 0x180087210: run-length codes the 1-bit glyph bitmap (see the
 * format at the top), then keeps the uncompressed form when it is not larger than 3x the
 * coded size. `spread` widens every run of set pixels by that many pixels. */
TT_EXPORT void tt_CachedBitmapWriter_Pack(uint8_t *w, uint8_t *bm, uint8_t spread, uint8_t is4x4) {
    if (BM_WIDTH(bm) == 0) spread = 0;
    WR_SPREAD(w) = spread;
    VecU16 *v = WR_VEC(w);
    if (v->begin != v->end) v->end = v->begin;
    uint64_t want = 0x100;
    if ((uint64_t)(v->cap - v->begin) < 0x100) ORIG(void (*)(VecU16 *, uint64_t *), RVA_VecU16_Reallocate)(v, &want);
    WR_FLAGS(w) = 0;
    WR_SIZE(w) = 0;
    WR_SRC(w) = NULL;
    uint32_t words = (uint32_t)BM_STRIDE(bm) >> 2;
    uint32_t height = BM_HEIGHT(bm);
    const uint32_t *row = BM_BITS(bm);
    uint32_t limit = (uint32_t)BM_STRIDE(bm) * 8;
    if (limit >= BM_WIDTH(bm)) limit = BM_WIDTH(bm);
    for (uint32_t r = 0; r < height; r++) {
        /* identical following rows: one odd entry (count * 2 + 1) before the row */
        uint32_t rep = 0;
        for (uint32_t k = r + 1; k < height; k++) {
            const uint32_t *next = (const uint32_t *)((const uint8_t *)row + (uint64_t)words * 4);
            if (memcmp(row, next, (size_t)(int64_t)BM_STRIDE(bm)) != 0) break;
            rep++;
            r++;
            row = next;
        }
        if (rep != 0) vec_push(v, (uint16_t)(rep * 2 | 1));
        uint64_t head = (uint64_t)(v->end - v->begin);
        vec_push(v, 0);
        uint32_t pos = 0;
        while (limit != 0) {
            uint32_t zeros = pack_run(row, words, pos, 0);
            if (zeros >= limit - pos) break;            /* the rest of the row is clear */
            vec_push(v, (uint16_t)zeros);
            pos += zeros;
            uint32_t ones = pack_run(row, words, pos, 1);
            uint32_t sp = WR_SPREAD(w);
            if (sp != 0) {
                ones += sp;
                uint32_t more;
                while ((more = ORIG(uint32_t (*)(const uint32_t *, uint32_t, uint32_t, uint32_t), RVA_CountBits)(
                            row, words, ones + pos, 1)) != 0)
                    ones += more + sp;
            }
            if (ones >= sp - pos + limit) break;        /* runs to the end of the (spread) row */
            vec_push(v, (uint16_t)ones);
            pos += ones;
            if (pos >= limit) break;
        }
        uint64_t n = (uint64_t)(v->end - v->begin) - head - 1;
        if (n > 0xffff) safeint_overflow();
        v->begin[head] = (uint16_t)(n * 2);             /* even entry: entries in the row */
        row = (const uint32_t *)((const uint8_t *)row + (uint64_t)words * 4);
    }
    uint64_t count = (uint64_t)(v->end - v->begin);
    if (count > 0xffffffffull) safeint_overflow();
    uint32_t bytes = (uint32_t)count;
    for (uint64_t i = 0; i < count; i++) {
        if (v->begin[i] > 0xff) {
            WR_FLAGS(w) |= 8;
            bytes = (uint32_t)count * 2;
            if (bytes < (uint32_t)count) safeint_overflow();
            break;
        }
    }
    WR_SIZE(w) = bytes;
    uint32_t bw = (uint32_t)WR_SPREAD(w) + BM_WIDTH(bm);
    uint32_t bh = BM_HEIGHT(bm);
    if (is4x4) {
        bw = ((bw + 3) & 0xfffffffcu) << 2;
        bh = (bh + 3) >> 2;
        WR_FLAGS(w) |= 0x20;
    }
    uint64_t t = (uint64_t)bw + 0x1f;
    if (t > 0xffffffffull) safeint_overflow();
    uint64_t usize = (uint64_t)((uint32_t)(t >> 3) & 0x1ffffffcu) * bh;
    if (usize > 0xffffffffull) safeint_overflow();
    uint64_t coded3 = (uint64_t)bytes * 3;
    if (coded3 > 0xffffffffull) safeint_overflow();
    if ((uint32_t)usize <= (uint32_t)coded3) {
        WR_FLAGS(w) |= 0x10;
        WR_SRC(w) = bm;
        WR_SIZE(w) = (uint32_t)usize;
    }
}

/* CachedBitmapWriter::StoreUncompressed 0x180019ac4: rows of whole dwords, the spread OR-ed in */
TT_EXPORT void tt_CachedBitmapWriter_StoreUncompressed(uint8_t *w, uint32_t *dst, uint64_t size) {
    uint8_t *bm = WR_SRC(w);
    uint64_t out_words = ((uint64_t)(int64_t)(int32_t)BM_WIDTH(bm) + WR_SPREAD(w) + 0x1f) >> 5;
    uint64_t in_words = (uint64_t)(int64_t)BM_STRIDE(bm) >> 2;
    uint64_t copy = out_words >= in_words ? in_words : out_words;
    int64_t rows = (int64_t)(int32_t)BM_HEIGHT(bm);
    if (size != out_words * (uint64_t)rows * 4) fail_assert(0x11c, (void *)in_words);
    const uint32_t *src = BM_BITS(bm);
    for (int64_t r = rows; r != 0; r--) {
        uint32_t carry = 0;
        uint64_t i = 0;
        if (WR_SPREAD(w) == 0) {
            for (; i < copy; i++) dst[i] = src[i];
        } else {
            for (; i < copy; i++) {
                uint32_t x = src[i];
                dst[i] = x << WR_SPREAD(w) | x | carry;
                carry = x >> (32 - WR_SPREAD(w));
            }
        }
        for (; i < out_words; i++) {
            dst[i] = carry;
            carry = 0;
        }
        src += in_words;
        dst += out_words;
    }
}

/* CachedBitmapWriter::StoreUncompressed4x4 0x1800c78fc: 4x4 cells of 16 bits, two per dword
 * (see the format at the top) */
TT_EXPORT void tt_CachedBitmapWriter_StoreUncompressed4x4(uint8_t *w, uint8_t *dst, uint64_t size) {
    uint8_t *bm = WR_SRC(w);
    uint32_t height = BM_HEIGHT(bm);
    uint32_t stride = (uint32_t)BM_STRIDE(bm);
    uint32_t in_words = stride >> 2;
    uint32_t bits = ORIG(uint32_t (*)(uint32_t, uint64_t), RVA_Align)(
        (((uint32_t)WR_SPREAD(w) + 3 + BM_WIDTH(bm)) & 0xfffffffcu) << 2, 0x20);
    uint32_t row_bytes = bits >> 3, row_words = row_bytes >> 2;
    if (size != (uint64_t)(((height + 3) >> 2) * row_bytes)) fail_assert(0x160, (void *)0x20);
    memset(dst, 0, size);
    const uint8_t *src = (const uint8_t *)BM_BITS(bm);
    for (uint32_t r = 0; r < height; r++) {
        uint32_t sub = r & 3, shift = (sub << 2) & 0xffff;
        uint32_t o = 0, carry = 0;
        const uint32_t *in = (const uint32_t *)src;
        for (uint32_t k = 0; k < in_words; k++) {
            uint32_t x = in[k];
            uint8_t sp = WR_SPREAD(w);
            if (sp != 0) {
                uint32_t t = x << sp | carry;
                carry = x >> (32 - sp);
                x |= t;
            }
            for (int q = 0; q < 4 && o < row_words; q++) {
                ((uint32_t *)dst)[o] |= ((x & 0xf0) << 12 | (x & 0xf)) << shift;
                x >>= 8;
                o++;
            }
        }
        for (; o < row_words; o++) {
            ((uint32_t *)dst)[o] |= ((carry & 0xf0) << 12 | (carry & 0xf)) << shift;
            carry = 0;
        }
        if (sub == 3) dst += row_bytes;
        src += stride;
    }
}

/* ---------------------------------------------------------------------------------------- */
/* Cache keys                                                                                 */
/* ---------------------------------------------------------------------------------------- */

/* GlyphDataElementKey<GlyphBitmapLayout,...>::ComputeHashCode 0x1800c4a10 */
TT_EXPORT uint32_t tt_GlyphKey_ComputeHashCode(const uint8_t *fontkey, const uint32_t *params) {
    return glyph_key_hash(fontkey, params);
}

/* GlyphDataElementKey<GlyphBitmapLayout,...>::GlyphDataElementKey 0x180019e14 */
TT_EXPORT GlyphKey *tt_GlyphKey_ctor(GlyphKey *k, const uint8_t *fontkey, const uint32_t *params, uint32_t glyph_count) {
    uint32_t h = glyph_key_hash(fontkey, params);
    k->fontkey = fontkey;
    k->params = params;
    k->one = 1;
    k->glyph_count = glyph_count;
    k->vftable = (uint8_t *)idwr_orig(RVA_GlyphKey_vftable);
    k->hash = h;
    k->shared = (AT(fontkey, 0, uint64_t) & 0x7fffffffffffffffull) >= 3;
    return k;
}

/* GlyphDataElementKey<GlyphBitmapLayout,...>::IsEqualKey 0x1800c2e60. A stored key is
 * {u32 ?, u32 ?, u32 ?, params[0x20], u32 n, FontFaceKey header[0x18], blob, axis values}. */
TT_EXPORT uint8_t tt_GlyphKey_IsEqualKey(const GlyphKey *k, const CacheBlock *stored, CacheBlock *out) {
    uint32_t size = stored->size;
    if (size < 0xc || (uint64_t)size - 0xc < 0x24) out_of_range(stored->base);
    const uint8_t *s = stored->base;
    const uint8_t *body = s + 0xc;
    if ((uintptr_t)body & 3) out_of_range(stored->base);
    if (memcmp(body, k->params, 0x20) != 0) return 0;
    uint32_t n = AT(body, 0x20, uint32_t);
    if (size < 0x30 || (uint64_t)size - 0x30 < n) out_of_range(s);
    const uint8_t *sk = s + 0x30;
    const uint8_t *fk = k->fontkey;
    if (n < 0x18) return 0;
    uint32_t len = AT(fk, 0xc, uint32_t);
    if (len != AT(sk, 0xc, uint32_t) || AT(fk, 0, uint64_t) != AT(sk, 0, uint64_t) ||
        AT(fk, 8, uint32_t) != AT(sk, 8, uint32_t) || AT(fk, 0x10, uint16_t) != AT(sk, 0x10, uint16_t))
        return 0;
    uint8_t a = AT(sk, 0x14, uint8_t), b = AT(fk, 0x14, uint8_t);
    /* face type 6 matches 0, 1 and 2 either way */
    if (a != b && !(b == 6 && a <= 2) && !(a == 6 && b <= 2)) return 0;
    if (AT(fk, 0x16, uint16_t) != AT(sk, 0x16, uint16_t)) return 0;
    if (memcmp(sk + 0x18, AT(fk, 0x18, const void *), len) != 0) return 0;
    if (AT(fk, 0x10, uint16_t) != 0) {
        uint64_t off = (uint64_t)(uint32_t)(len + 0x18) + 3;
        if (off > 0xffffffffull) safeint_overflow();
        off &= 0xfffffffcu;
        uint16_t na = AT(sk, 0x10, uint16_t);
        if (off > n || ((uint64_t)n - off) >> 3 < na || ((uintptr_t)(sk + off) & 3)) out_of_range(sk);
        if (memcmp(AT(fk, 0x20, const void *), sk + off, (uint64_t)na << 3) != 0) return 0;
    }
    out->base = body;
    out->size = (uint32_t)(sk - body) + n;
    return 1;
}

/* GlyphDataElement<GlyphBitmapLayout,...>::TryGetExistingGlyph 0x18001d7e0 (per-user cache) */
TT_EXPORT const uint8_t *tt_TryGetExistingGlyph(const ElementView *e, uint32_t glyph) {
    uint16_t g = (uint16_t)glyph;
    if (g >= (uint16_t)e->glyph_count) {
        if ((uint16_t)e->glyph_count == 0)
            throw_hr(RVA_LogAndThrow_ArgOutOfRange, HR_E_INVALIDARG, 0x64696763ull, 0x31f);
        g = 0;
    }
    uint64_t po = e->pages[g >> 6];
    if (po == 0) return NULL;
    uint64_t size = e->size;
    if (po > size || size - po < 0x100) out_of_range(e->base);
    const uint8_t *page = e->base + po;
    if ((uintptr_t)page & 3) out_of_range(e->base);
    uint64_t so = ((const uint32_t *)page)[g & 0x3f];
    if (so == 0) return NULL;
    if (so > size || size - so < 0x10) out_of_range(e->base);
    const uint8_t *l = e->base + so;
    if ((uintptr_t)l & 3) out_of_range(e->base);
    uint64_t len = (uint64_t)(GBL_HDR(l) & 0x3ffffff) + 0x10;
    if (size - so < len) out_of_range(e->base);
    if (GBL_ID(l) != e->id) throw_invalid_cache(&page, 0x1a9);
    return l;
}

/* ---------------------------------------------------------------------------------------- */
/* IDWriteGlyphLookupCache (DWriteGlyphLookupCache, used by Direct2D)                         */
/* ---------------------------------------------------------------------------------------- */
#define RVA_GetOddDitheringPeer         0xef8b0   /* LookupCacheBitmapEntry::GetOddDitheringPeer */
#define RVA_TryGetCachedGlyphLayout     0x18a60   /* GlyphLookupCache::TryGetCachedGlyphLayout<BitmapTraits> */
#define RVA_LookupRasterizer_ctor       0xa57d4   /* GlyphLookupCache::Rasterizer<BitmapTraits>::Rasterizer */
#define RVA_LookupRasterizer_dtor       0xc4890   /* its scalar deleting destructor */
#define RVA_LookupRasterizeGlyph        0x18e78
#define RVA_IsCompatibleFullWidthGlyph  0x19c14
#define RVA_operator_new                0x117b04
#define RVA_MergeGlyph1Bit              0x16be0
#define RVA_MergeUncompressedGlyph4x4   0x17c18
#define RVA_MergeCompressedGlyph4x4_u8  0x182d0
#define RVA_MergeCompressedGlyph4x4_u16 0x15210
#define RVA_GetFontEntry                0x4b92c   /* GlyphLookupCache::GetFontEntry */
#define RVA_HashWords                   0x4c650
#define RVA_BitmapEntry_IsEqualKey      0x4a8ac   /* HashTable<GlyphBitmapRasterizationParameters,...>::IsEqualKeyWrapper */
#define RVA_BitmapEntry_Create          0x49c44   /* LookupCacheEntry<BitmapTraits>::Create */
#define RVA_HashTableImpl_Add           0x4bd80

static void *op_new(uint64_t n) { return ORIG(void *(*)(uint64_t), RVA_operator_new)(n); }

/* intrusive list node {next, prev, u8 linked}: append before `head` */
static void list_append(uint8_t *head, uint8_t *node) {
    uint8_t *prev = AT(head, 8, uint8_t *);
    AT(node, 0, uint8_t *) = head;
    AT(node, 8, uint8_t *) = prev;
    AT(head, 8, uint8_t *) = node;
    AT(prev, 0, uint8_t *) = node;
}
static void list_remove(uint8_t *node) {
    uint8_t *next = AT(node, 0, uint8_t *), *prev = AT(node, 8, uint8_t *);
    AT(next, 8, uint8_t *) = prev;
    AT(prev, 0, uint8_t *) = next;
}

/* GlyphLookupCache::Rasterizer<BitmapTraits>::RasterizeGlyph 0x180018e78. The Rasterizer
 * starts with a FontFace; its state is at +0x2c8. Each glyph gets its own block: list node
 * (0x18 bytes) + GlyphBitmapLayout. */
TT_EXPORT const uint8_t *tt_LookupRasterizer_RasterizeGlyph(uint8_t *r, uint32_t glyph, uint8_t *page) {
    uint16_t g = (uint16_t)glyph;
    uint8_t *state = r + 0x2c8;
    uint8_t fw = ORIG(uint8_t (*)(uint8_t *, uint32_t), RVA_IsCompatibleFullWidthGlyph)(r, g);
    ORIG(void (*)(uint8_t *, uint32_t, uint8_t), RVA_State_RasterizeGlyph)(state, g, fw);
    uint32_t n = ORIG(uint32_t (*)(uint8_t *), RVA_State_GetCachedGlyphDataSize)(state);
    uint64_t total = (uint64_t)n + 0x18;
    if (total > 0xffffffffull) safeint_overflow();
    uint8_t *node = op_new((uint32_t)total);
    AT(node, 0x10, uint8_t) = 0;
    ORIG(void (*)(uint8_t *, uint8_t *, uint32_t), RVA_State_FillGlyphData)(state, node + 0x18, 0);
    list_append(AT(r, 0x328, uint8_t *), node);
    AT(node, 0x10, uint8_t) = 1;
    AT(page, 0x228, uint64_t) |= 1ull << (g & 0x3f);   /* short-lived */
    vec_push((VecU16 *)(r + 0x340), g);
    return node + 0x18;
}

/* Page of 64 glyph slots of a LookupCacheBitmapEntry (0x238 bytes): list node, slots at
 * +0x18, entry +0x218, page index +0x220, short-lived bits +0x228, long-lived bits +0x230 */
static uint8_t *lookup_new_page(uint8_t *entry, uint32_t index) {
    uint8_t *p = op_new(0x238);
    AT(p, 0x10, uint8_t) = 0;
    AT(p, 0x218, uint8_t *) = entry;
    AT(p, 0x220, uint32_t) = index;
    AT(p, 0x228, uint64_t) = 0;
    AT(p, 0x230, uint64_t) = 0;
    memset(p + 0x18, 0, 0x200);
    list_append(AT(entry, 0x80, uint8_t *), p);
    AT(p, 0x10, uint8_t) = 1;
    AT(entry, 0xe0, uint8_t **)[index] = p;
    return p;
}

/* the layout of one glyph of a lookup entry: page slot, the per-user cache, or rasterized.
 * `cache` is the GlyphLookupCache (DWriteGlyphLookupCache + 0x10). */
static const uint8_t *lookup_glyph(uint8_t *cache, void *ctx, uint8_t *face, uint8_t *entry, uint16_t g,
                                   uint8_t **page_out) {
    uint32_t pi = g >> 6, slot = g & 0x3f;
    uint8_t *page = AT(entry, 0xe0, uint8_t **)[pi];
    const uint8_t *l = NULL;
    if (page == NULL) page = lookup_new_page(entry, pi);
    else l = AT(page, 0x18 + 8 * slot, const uint8_t *);
    if (l == NULL) {
        if (AT(entry, 0x78, uint8_t) == 0) {   /* entry joins the cache's list of used entries */
            uint8_t *node = entry + 8;
            list_append(AT(cache, 0x38, uint8_t *), node);
            AT(node, 0x10, uint8_t) = 1;
            AT(entry, 0x78, uint8_t) = 1;
        }
        l = ORIG(const uint8_t *(*)(uint8_t *, void *, uint8_t *, uint8_t *, uint16_t), RVA_TryGetCachedGlyphLayout)(
            cache, ctx, face, entry, g);
        if (l == NULL) {
            if (AT(entry, 0xb0, uint8_t *) == NULL) {
                uint8_t *r = op_new(0x358);
                r = ORIG(uint8_t *(*)(uint8_t *, void *, uint8_t *, uint8_t *), RVA_LookupRasterizer_ctor)(
                    r, ctx, face, entry + 0x28);
                uint8_t *old = AT(entry, 0xb0, uint8_t *);
                AT(entry, 0xb0, uint8_t *) = r;
                if (old != NULL) ORIG(void (*)(uint8_t *, uint32_t), RVA_LookupRasterizer_dtor)(old, 1);   /* unreachable */
            }
            l = ORIG(const uint8_t *(*)(uint8_t *, uint32_t, uint8_t *), RVA_LookupRasterizeGlyph)(
                AT(entry, 0xb0, uint8_t *), g, page);
        }
        AT(page, 0x18 + 8 * slot, const uint8_t *) = l;
    }
    *page_out = page;
    return l;
}

/* DWRITE_GLYPH_OPTIONS bit 0: move a short-lived glyph to the entry's long-lived list */
static void lookup_make_long_lived(uint8_t *cache, uint8_t *entry, uint8_t *page, const uint8_t *l, uint16_t g) {
    uint64_t bit = 1ull << (g & 0x3f);
    if (!(AT(page, 0x228, uint64_t) & bit)) return;
    uint8_t *node = (uint8_t *)l - 0x18;
    list_remove(node);
    AT(node, 0x10, uint8_t) = 0;
    list_append(AT(entry, 0x98, uint8_t *), node);
    AT(node, 0x10, uint8_t) = 1;
    AT(page, 0x228, uint64_t) &= ~bit;
    AT(page, 0x230, uint64_t) |= bit;
    AT(cache, 0x30, uint64_t) += (uint64_t)(GBL_HDR(l) & 0x3ffffff) + 0x28;
}

static uint16_t lookup_constrain(uint8_t *entry, uint16_t g) {
    if (g >= AT(entry, 0x50, uint16_t)) {
        if (AT(entry, 0x50, uint16_t) == 0)
            throw_hr(RVA_LogAndThrow_ArgOutOfRange, HR_E_INVALIDARG, 0x64696763ull, 0x31f);
        g = 0;
    }
    return g;
}

/* GlyphLookupCache::GetBitmaps 0x180018770 */
TT_EXPORT void tt_GlyphLookupCache_GetBitmaps(uint8_t *cache, void *ctx, uint8_t *face, uint8_t *entry, uint32_t options,
                                              const uint16_t *indices, uint32_t count, const uint8_t **out) {
    if (options & 2)
        entry = ORIG(uint8_t *(*)(uint8_t *, uint8_t *), RVA_GetOddDitheringPeer)(entry, face);
    for (uint32_t i = 0; i < count; i++) {
        uint16_t g = lookup_constrain(entry, indices[i]);
        uint8_t *page;
        const uint8_t *l = lookup_glyph(cache, ctx, face, entry, g, &page);
        if (options & 1) lookup_make_long_lived(cache, entry, page, l, g);
        out[i] = l;
    }
}

typedef void (*merge_fn)(const uint8_t *, int32_t, int32_t, uint32_t, uint32_t, uint32_t, void *);

/* DWriteGlyphLookupCache::FillGlyphBitmap 0x1800176e0: merges one glyph into a 1-bit (or 4x4)
 * target at (x, y) */
TT_EXPORT int32_t tt_DWriteGlyphLookupCache_FillGlyphBitmap(uint8_t *self, uint8_t *fontface, uint8_t *entry, uint32_t glyph,
                                                            uint32_t options, int32_t x, int32_t y, uint32_t rows,
                                                            uint32_t stride, void *buf) {
    uint32_t mx = _mm_getcsr();
    if ((mx & 0xff80) != 0x1f80) _mm_setcsr(0x1f80);
    uint8_t *cache = self + 0x10;
    void *ctx = AT(self, 0x80, uint8_t *) + 8;
    uint8_t *face = fontface + 0x30;
    uint8_t *e = entry;
    if (options & 2) e = ORIG(uint8_t *(*)(uint8_t *, uint8_t *), RVA_GetOddDitheringPeer)(entry, face);
    uint16_t g = lookup_constrain(e, (uint16_t)glyph);
    uint8_t *page;
    const uint8_t *l = lookup_glyph(cache, ctx, face, e, g, &page);
    if (options & 1) lookup_make_long_lived(cache, e, page, l, g);
    if (AT(entry, 0x3e, uint16_t) == 4) {   /* 4x4 entry (the entry passed in, not the peer) */
        uint32_t bits = stride * 8;
        if (x < (int32_t)(bits >> 2) && y < (int32_t)(rows * 4) && (int32_t)(GBL_WIDTH(l) + x) > 0 &&
            (int32_t)(GBL_HEIGHT(l) + y) > 0) {
            uint32_t rva = (GBL_HDR(l) & 0x40000000) ? RVA_MergeUncompressedGlyph4x4
                         : (GBL_HDR(l) & 0x20000000) ? RVA_MergeCompressedGlyph4x4_u16 : RVA_MergeCompressedGlyph4x4_u8;
            ((merge_fn)idwr_orig(rva))(l, x, y, bits, rows, stride, buf);
        }
    } else {
        ((merge_fn)idwr_orig(RVA_MergeGlyph1Bit))(l, x, y, stride * 8, rows, stride, buf);
    }
    if ((mx & 0xff80) != 0x1f80) _mm_setcsr(mx & 0xffffffc0);
    return 0;
}

/* DWriteGlyphLookupCache::GetLookupCacheBitmapEntry 0x18004af80: the cache entry (one set of
 * GlyphBitmapRasterizationParameters) for a face, em size and modes. The parameters are built
 * as in the GlyphRunAnalysis constructor, without pixelsPerDip, transform folding or the
 * DEFAULT grid fit resolution (grid fit must be explicit). */
TT_EXPORT int32_t tt_GetLookupCacheBitmapEntry(uint8_t *self, uint8_t *fontface, float em, int32_t sideways,
                                               const float *matrix, int32_t mode, int32_t gridfit, int32_t aa,
                                               uint8_t **out_entry, int32_t *out_type, void **out_ptr) {
    *out_entry = NULL;
    uint32_t mx = _mm_getcsr();
    if ((mx & 0xff80) != 0x1f80) _mm_setcsr(0x1f80);
    if ((uint32_t)mode >= 8) throw_hr(RVA_LogAndThrow_Argument, HR_E_INVALIDARG, 0x727474615f72ull, 0xae);
    if ((uint32_t)aa >= 2) throw_hr(RVA_LogAndThrow_Argument, HR_E_INVALIDARG, 0x727474615f72ull, 0xb3);
    const uint32_t *table = (const uint32_t *)((uint8_t *)idwr_orig(RVA_ModeAttributeTable));
    uint32_t attr = table[(int64_t)mode + (int64_t)aa * 8];
    if ((attr & 0xf) == 0 || gridfit == 0)
        throw_hr(RVA_LogAndThrow_Argument, HR_E_INVALIDARG, 0x65686361636267ull, 0x56);
    uint8_t *face = fontface + 0x30;
    int round_em = (attr >> 19 & 1) || (AT(AT(face, 0, uint8_t *), 0x60, uint8_t) & 0x10);
    if ((AT(AT(fontface, 0x138, uint8_t *), 0x14, uint8_t) & 1) == 0) attr &= ~0x800000u;
    uint8_t ppem = 0;
    int32_t m[4] = {0, 0, 0, 0};
    if (matrix != NULL) {
        float r0x, r0y, r1x, r1y;
        if (!sideways) { r0x = em; r0y = 0.0f; r1x = 0.0f; r1y = em; }
        else { r0x = 0.0f; r0y = -em; r1x = em; r1y = 0.0f; }
        float a = r0y * matrix[2] + r0x * matrix[0];
        float b = r0y * matrix[3] + r0x * matrix[1];
        float c = r1y * matrix[2] + r1x * matrix[0];
        float d = r1y * matrix[3] + r1x * matrix[1];
        int axis = 0;
        float p = 0, q = 0;
        if (is_zero_tol(b)) {
            if (is_zero_tol(c)) { axis = 1; p = a; q = d; }
        } else if (is_zero_tol(a) && is_zero_tol(d)) {
            axis = 1; p = b; q = c;
        }
        if (axis) {
            p = fabs_bits(p);
            q = fabs_bits(q);
            if (is_zero_tol(p - q)) {
                if (0.0f > p || p >= 32768.0f)
                    throw_hr(RVA_LogAndThrow_ArgOutOfRange, HR_E_INVALIDARG, 0x736e617274ull, 0xf7);
                int32_t ip = round_half_up(p);
                if ((uint32_t)ip <= 0xff) ppem = (uint8_t)ip;
                if (round_em && !is_zero_tol(p)) {
                    float k = (float)(uint32_t)ip / p;
                    a = k * a; b = k * b; c = k * c; d = k * d;
                }
            }
        }
        m[0] = float_to_fixed(a);
        m[2] = float_to_fixed(-b);
        m[1] = float_to_fixed(-c);
        m[3] = float_to_fixed(d);
    } else {
        int32_t f = round_em ? (int32_t)((uint32_t)round_half_up(em) << 16) : float_to_fixed(em);
        if (!sideways) { m[0] = f; m[3] = f; }
        else { m[1] = (int32_t)(0u - (uint32_t)f); m[2] = f; }
        int32_t v = (int32_t)(0u - (uint32_t)f);
        if (v < 0) v = f;
        if (v <= 0xffffff) ppem = (uint8_t)(v >> 16);
    }
    uint16_t sims = AT(fontface, 0x66, uint16_t);
    uint32_t flags = (sims & 1) ? 2 : 0;
    if (sims & 2) flags |= (sideways ? 8u : 0u) + 4;
    if (gridfit == 2) flags |= 1;
    if (flags & 1) {
        if (attr >> 19 & 1) flags |= (attr >> 21 & 1) ? 0x10 : 0x20;
        if ((attr >> 23 & 1) || ((attr & 0xf) == 1 && (attr & 0xf0) == 0x10)) flags |= 0x40;
    }
    if (attr >> 17 & 1) flags |= 0x80;
    RasterParams rp;
    memset(&rp, 0, sizeof rp);
    memcpy(rp.m, m, 16);
    rp.point_size = round_half_up(em * 0.75f);
    rp.ox = attr & 0xf;
    rp.oy = attr >> 4 & 0xf;
    rp.flags = flags;
    rp.ppem = ppem;
    uint8_t *fe = ORIG(uint8_t *(*)(uint8_t *, uint8_t *), RVA_GetFontEntry)(self + 0x10, face);
    uint16_t nglyphs = AT(AT(face, 0, uint8_t *), 0x62, uint16_t);
    uint8_t *entry = AT(fe, 0x58, uint8_t *);   /* last entry used */
    if (entry == NULL || memcmp(&rp, entry + 0x28, 0x20) != 0) {
        /* HashTable<GlyphBitmapRasterizationParameters, LookupCacheBitmapEntry> at +0x30:
         * buckets (u32 index + 1), count, items {u64 hash, value, u32 next} */
        uint32_t h = ORIG(uint32_t (*)(const void *, uint64_t, uint32_t), RVA_HashWords)(&rp, 8, 0);
        uint8_t *items = AT(fe, 0x40, uint8_t *);
        uint8_t *end = items + (uint64_t)AT(fe, 0x48, uint32_t) * 0x18;
        uint8_t *found = end;
        if (AT(fe, 0x38, uint32_t) != 0) {
            uint32_t idx = AT(fe, 0x30, uint32_t *)[(AT(fe, 0x38, uint32_t) - 1) & h];
            while (idx != 0) {
                if (idx > AT(fe, 0x48, uint32_t)) fail_assert(0x53, NULL);
                uint8_t *it = AT(fe, 0x40, uint8_t *) + (uint64_t)(idx - 1) * 0x18;
                if ((uint64_t)h == AT(it, 0, uint64_t) &&
                    ORIG(uint8_t (*)(const void *, void *), RVA_BitmapEntry_IsEqualKey)(&rp, AT(it, 8, void *))) {
                    found = it;
                    break;
                }
                idx = AT(it, 0x10, uint32_t);
            }
        }
        end = AT(fe, 0x40, uint8_t *) + (uint64_t)AT(fe, 0x48, uint32_t) * 0x18;
        if (found == end) {
            uint8_t *ne = ORIG(uint8_t *(*)(uint8_t *, RasterParams *, uint32_t, uint16_t), RVA_BitmapEntry_Create)(
                fe, &rp, h, nglyphs);
            uint8_t *iter[2];
            uint8_t **res = ORIG(uint8_t **(*)(uint8_t *, uint8_t **, uint64_t, void *), RVA_HashTableImpl_Add)(
                fe + 0x30, iter, h, ne);
            entry = AT(*res, 8, uint8_t *);
        } else {
            entry = AT(found, 8, uint8_t *);
        }
        AT(fe, 0x58, uint8_t *) = entry;
    }
    *out_entry = entry;
    *out_type = AT(entry, 0xf8, int32_t);
    *out_ptr = ((void *(*)(uint8_t *))AT(entry, 0, void **)[0])(entry);
    if ((mx & 0xff80) != 0x1f80) _mm_setcsr(mx & 0xffffffc0);
    return 0;
}
