/*
 * Native helpers: the originals the scaler still reaches that are not ports of their own,
 * because they are compiler or runtime support rather than scaler logic: fixed-point leaves,
 * SafeInt checks and their overflow handlers, folded trivial functions. In standalone mode
 * idwr_orig (orig.c) returns these for their RVAs. Each is written from the assembly of
 * DWrite.dll 10.0.26100.9711 and compared with the original on random and edge inputs
 * (reimpl/standalone_check.py --leaves); the exports exist in both modes for that test.
 *
 * Errors: the SafeInt handlers throw in the original (C++ exception, caught by
 * TrueTypeRasterizer::Implementation). Here they jump to the catch frame the driver armed
 * (native.h), which returns IDWR_ERR_SAFEINT_*; with no frame armed they abort.
 */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include "tt_state.h"
#include "native.h"

/* ---- errors ---- */
static _Thread_local idwr_catch *g_catch;

void idwr_catch_push(idwr_catch *c) {
    c->code = 0;
    c->prev = g_catch;
    g_catch = c;
}

void idwr_catch_pop(idwr_catch *c) { g_catch = c->prev; }

_Noreturn void idwr_raise(int code) {
    idwr_catch *c = g_catch;
    if (c) {
        c->code = code;
        IDWR_LONGJMP(c->buf);
    }
    IDWR_DIAG("idwr: unhandled scaler error %#x\n", code);
    IDWR_FATAL("native.c:38");
}

/* SafeIntExceptionHandler<SafeIntRasterizerException>::SafeIntOnOverflow 0x1801090b0 */
TT_EXPORT void idwr_native_SafeIntOnOverflow(void) { idwr_raise(IDWR_ERR_SAFEINT_OVERFLOW); }
/* SafeIntExceptionHandler<SafeIntRasterizerException>::SafeIntOnDivZero 0x18014cd24 */
TT_EXPORT void idwr_native_SafeIntOnDivZero(void) { idwr_raise(IDWR_ERR_SAFEINT_DIVZERO); }
/* SafeIntExceptionHandler<Exception>::SafeIntOnOverflow 0x1800fa5cc (glyph run code) */
TT_EXPORT void idwr_native_SafeIntOnOverflow_Exception(void) { idwr_raise(IDWR_ERR_SAFEINT_OVERFLOW); }

/* ---- folded trivial functions ---- */
/* 0x1800dcb00 (ICacheReference::GetExpirationEvent after identical-code folding): returns 0.
 * The scaler stores it as sfac_ComputeIndex for platform 0xffff (glyph ids used directly). */
TT_EXPORT uint64_t idwr_native_return0(void) { return 0; }
/* 0x1800e5100 (an empty destructor after folding): the default ReleaseSfntFrag */
TT_EXPORT void idwr_native_nop(void *p) { (void)p; }

/* ---- fixed point ---- */
/* DWRITE_FixMul 0x180067b70: (a * b + 0x8000) >> 16, rounding toward +inf on .5 for
 * positive and toward 0 for negative products (the +sign bit), saturated to int32 */
TT_EXPORT int32_t idwr_native_FixMul(int32_t a, int32_t b) {
    int64_t p = (int64_t)a * b;
    int64_t q = (p + (p >> 63) + 0x8000) >> 16;
    if (q > 0x7fffffff) return 0x7fffffff;
    if (q < -0x80000000LL) return INT32_MIN;
    return (int32_t)q;
}

/* DWRITE_FixDiv 0x180069800: (a << 16) / b rounded half away from zero (b / 2 added with the
 * sign of the quotient), saturated; b = 0 gives 0x7fffffff whatever the sign of a */
TT_EXPORT int32_t idwr_native_FixDiv(int32_t a, int32_t b) {
    int64_t n = (int64_t)a * 65536;
    int64_t half = b / 2;
    n = (((uint32_t)b >> 31) == (uint32_t)((uint64_t)n >> 63)) ? n + half : n - half;
    if (b == 0) return 0x7fffffff;
    int64_t q = n / b;
    if (q > 0x7fffffff) return 0x7fffffff;
    if (q < -0x80000000LL) return INT32_MIN;
    return (int32_t)q;
}

/* CHKOVF 0x18000bc10 (ported as tt_CHKOVF): saturate to int32 */
static int32_t chkovf(int64_t v) {
    if (v > 0x7fffffffLL) return 0x7fffffff;
    if (v < -0x80000000LL) return INT32_MIN;
    return (int32_t)v;
}

/* mth_FixXYMul 0x1800687bc: (x, y) through the 3x3 TransMatrix m (16.16, row 2 of the
 * perspective column 2.30), divided by the perspective term unless it is 0 or 1.0 */
TT_EXPORT void idwr_native_FixXYMul(int32_t *x, int32_t *y, const int32_t *m) {
    int64_t x0 = *x, y0 = *y;
    *x = (int32_t)((uint32_t)idwr_native_FixMul(m[3], (int32_t)y0) + (uint32_t)idwr_native_FixMul(m[0], (int32_t)x0));
    *y = (int32_t)((uint32_t)idwr_native_FixMul(m[4], (int32_t)y0) + (uint32_t)idwr_native_FixMul(m[1], (int32_t)x0));
    if (m[2] == 0 && m[5] == 0) return;
    int64_t py = (int64_t)m[5] * y0, px = (int64_t)m[2] * x0;
    uint32_t w = (uint32_t)chkovf((py + 0x20000000 + (py >> 63)) >> 30);
    w += (uint32_t)chkovf((px + 0x20000000 + (px >> 63)) >> 30);
    w += (uint32_t)m[8];
    if ((w & 0xfffeffff) == 0) return;
    *x = idwr_native_FixDiv(*x, (int32_t)w);
    *y = idwr_native_FixDiv(*y, (int32_t)w);
}

/* CompDiv 0x18006da90: n / d rounded half away from zero, as a 64-bit quotient in RAX;
 * d = 0 gives 0x7fffffff, or 0x80000000 (as a positive 64-bit value) for a negative n */
TT_EXPORT int64_t idwr_native_CompDiv(int32_t d, int64_t n) {
    int64_t half = d / 2;
#ifdef IDWR_NEGATIVE_CONTROL_COMPDIV
    n = n + half; /* standalone_check.py --control compdiv: rounding wrong for opposite signs */
#else
    n = ((d < 0) == (n < 0)) ? n + half : n - half;
#endif
    if (d != 0) return n / d;
    return (int64_t)(n < 0) + 0x7fffffff;
}

/* mth_DivShiftLong 0x18010b7fc: a / d, by shifts for 2, 4, 8 (rounding down) and by a
 * division rounding down otherwise; d = 0 or 1 returns a */
TT_EXPORT int32_t idwr_native_DivShiftLong(int32_t a, int16_t d16) {
    int32_t d = d16;
    if (d == 0 || d == 1) return a;
    if (d == 2) return a >> 1;
    if (d == 4) return a >> 2;
    if (d == 8) return a >> 3;
    if (a < 0) a = (int32_t)((uint32_t)a - (uint32_t)d + 1);
    return a / d;
}

/* ---- SafeInt ---- */
/* ROWBYTESLONG<int> 0x1800ed964: bytes per row of a 1-bit bitmap, padded to 4 bytes */
TT_EXPORT uint16_t idwr_native_ROWBYTESLONG(int32_t bits) {
    uint32_t r = ((uint32_t)bits + 0x1f) >> 3 & 0x1ffffffc;
    if (bits < 0 || r >= 0x10000) idwr_native_SafeIntOnOverflow();
    return (uint16_t)r;
}

/* SafeInt<int>::operator*(SafeInt<int>) 0x1800c895c: *out = *self * b, checked */
TT_EXPORT int32_t *idwr_native_SafeIntMul(const int32_t *self, int32_t *out, int32_t b) {
    int64_t p = (int64_t)*self * b;
    if ((uint64_t)(p + 0x80000000LL) >= 0x100000000ULL) idwr_native_SafeIntOnOverflow();
    *out = (int32_t)p;
    return out;
}

/* operator*<int, int>(int, SafeInt<int>) 0x1800d942c: *out = a * b, checked */
TT_EXPORT int32_t *idwr_native_SafeIntMulIntInt(int32_t *out, int32_t a, int32_t b) {
    int64_t p = (int64_t)a * b;
    if ((uint64_t)(p + 0x80000000LL) >= 0x100000000ULL) idwr_native_SafeIntOnOverflow();
    *out = (int32_t)p;
    return out;
}

/* ScalerOverScale 0x1800d6084: the gray overscale (key + 0x488) unless the ClearType
 * compatibility word (input + 0x8c) is 2 */
TT_EXPORT uint16_t idwr_native_ScalerOverScale(const uint8_t *input, const uint8_t *key) {
    if (AT(input, 0x8c, int32_t) != 2) return AT(key, 0x488, uint16_t);
    return 0;
}

/* ---- the glyph run layer's DWrite infrastructure (reimpl/idwr/texture.c) ----
 * The standalone texture path runs GlyphBitmapRasterizationState, CachedBitmapWriter and
 * CreateAlphaTexture (glyphrun_*.c), which reach these originals. */

/* std::vector<T> as MSVC lays it out: first, last, end of storage */
typedef struct { uint8_t *first, *last, *end; } idwr_vec;

/* std::vector<uint16_t>::_Reallocate<0> 0x1800f61e0: new storage of exactly *cap elements,
 * the elements moved, the old storage freed */
TT_EXPORT void idwr_native_VecU16_Reallocate(idwr_vec *v, const uint64_t *cap) {
    size_t n = (size_t)(v->last - v->first);
    uint8_t *p = malloc(*cap * 2 ? *cap * 2 : 1);
    if (!p) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    if (n) memmove(p, v->first, n);
    free(v->first);
    v->first = p;
    v->last = p + n;
    v->end = p + *cap * 2;
}

/* std::vector<uint16_t>::_Emplace_reallocate 0x1800f7dc0: inserts *value at `where` into a
 * full vector, capacity grown by half (at least to size + 1); returns the new element */
TT_EXPORT uint16_t *idwr_native_VecU16_EmplaceReallocate(idwr_vec *v, uint16_t *where, const uint16_t *value) {
    size_t size = (size_t)(v->last - v->first) / 2, cap = (size_t)(v->end - v->first) / 2;
    size_t at = (size_t)((uint8_t *)where - v->first) / 2;
    size_t ncap = cap + cap / 2;
    if (ncap < size + 1) ncap = size + 1;
    uint16_t *p = malloc(ncap * 2);
    if (!p) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    uint16_t *old = (uint16_t *)v->first;
    if (at) memcpy(p, old, at * 2);
    p[at] = *value;
    if (size > at) memcpy(p + at + 1, old + at, (size - at) * 2);
    free(old);
    v->first = (uint8_t *)p;
    v->last = (uint8_t *)(p + size + 1);
    v->end = (uint8_t *)(p + ncap);
    return p + at;
}

/* std::vector<uint32_t>::_Resize_reallocate<_Value_init_tag> 0x1800f7174: resize to n
 * elements, the new ones zero, capacity max(n, 1.5 x the old one) */
TT_EXPORT void idwr_native_VecU32_ResizeReallocate(idwr_vec *v, uint64_t n, const void *tag) {
    (void)tag;
    size_t size = (size_t)(v->last - v->first) / 4, cap = (size_t)(v->end - v->first) / 4;
    size_t ncap = cap + cap / 2;
    if (ncap < n) ncap = (size_t)n;
    uint8_t *p = malloc(ncap * 4 ? ncap * 4 : 1);
    if (!p) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    size_t keep = size < n ? size : (size_t)n;
    if (keep) memcpy(p, v->first, keep * 4);
    if (n > keep) memset(p + keep * 4, 0, ((size_t)n - keep) * 4);
    free(v->first);
    v->first = p;
    v->last = p + n * 4;
    v->end = p + ncap * 4;
}

/* Exception::Exception(this, hr, unused) 0x1800aa7f0: the HRESULT is all the stand-in keeps */
TT_EXPORT void *idwr_native_Exception_ctor(void *e, int32_t hr, uint32_t unused) {
    (void)unused;
    *(int32_t *)e = hr;
    return e;
}
/* IntegerOverflowException::IntegerOverflowException 0x18010c4f8 (HRESULT_FROM_WIN32
 * ERROR_ARITHMETIC_OVERFLOW) */
TT_EXPORT void *idwr_native_IntegerOverflowException_ctor(void *e) {
    *(int32_t *)e = (int32_t)0x80070216;
    return e;
}
/* CaptureStack 0x18001cf60: diagnostics only */
TT_EXPORT void idwr_native_CaptureStack(int32_t hr, uint32_t unused) { (void)hr; (void)unused; }
/* LogAndThrow<ArgumentException, OSException, NotSupportedException, InvalidCacheDataException,
 * ArgumentOutOfRangeException, IntegerOverflowException>(exception, EventTag, line): the
 * original throws, and DWrite's COM entry point returns the exception's HRESULT. Here the
 * call jumps to the caller's catch frame with that HRESULT. */
TT_EXPORT void idwr_native_LogAndThrow(const void *e, uint64_t tag, uint32_t line) {
    (void)tag; (void)line;
    idwr_raise(*(const int32_t *)e);
}
/* FailAssert 0x18010cb50 (an internal consistency check of the original). Its second argument,
 * a diagnostic value, is not read here (the CFF glue passes the line only). */
TT_EXPORT void idwr_native_FailAssert(uint32_t line) {
    (void)line;
    idwr_raise(IDWR_ERR_FAIL_ASSERT);
}

/* FontFaceLight::GetFontWeight 0x180024a54, on the FontFace stand-in of native.h: 700 under
 * bold simulation (face + 0x36 bit 0), else the weight of the face data ([face] + 0x58) */
TT_EXPORT int32_t idwr_native_GetFontWeight(const uint8_t *face) {
    if (AT(face, 0x36, uint8_t) & 1) return 700;
    return AT(AT(face, 0, const uint8_t *), 0x58, uint16_t);
}
/* FontRasterizer::CreateRasterizer(FontFaceReference const &) 0x1800910f0, on the stand-in:
 * the reference (face + 0x98) holds {create function, context} */
TT_EXPORT void *idwr_native_CreateRasterizer(void *const *ref) {
    return ((void *(*)(void *))ref[0])(ref[1]);
}

/* sbit_Embolden 0x18014ef90 (bits, width, height, rowBytes, dx, dy): the bold simulation of a
 * 1-bit bitmap (most significant bit first; the caller reverses DWrite's bit order around
 * it), written from the original's behaviour:
 *   - dx > 0: each row keeps its first width - dx pixels, then a pixel is set when it or one
 *     of the dx pixels to its left is; dx < 0: the mirror image (the first |dx| pixels
 *     dropped, then smeared to the left). Either way nothing is left after `width` pixels in
 *     the row's rowBytes. dx = 0: the rows are left as they are.
 *   - dy > 0: the last dy rows are cleared, then each row from the bottom ORs the dy rows
 *     above it (the first ceil(width / 8) bytes); dy < 0: the first |dy| rows cleared, then
 *     each row from the top ORs the |dy| rows below it.
 * The horizontal pass covers rows [max(-dy, 0), height - max(dy, 0)). */
static int sbit_px(const uint8_t *row, int x) { return row[x >> 3] >> (7 - (x & 7)) & 1; }
TT_EXPORT void idwr_native_sbit_Embolden(uint8_t *bits, uint16_t width, uint16_t height, uint16_t row_bytes,
                                        uint16_t dx16, uint16_t dy16) {
    int16_t dx = (int16_t)dx16, dy = (int16_t)dy16;   /* short in the original; the callers pass words */
    if (height == 0 || bits == NULL) return;
#ifdef IDWR_NEGATIVE_CONTROL_EMBOLDEN
    return;   /* reimpl/texture_check.py --control embolden */
#endif
    int w = width, h = height, rb = row_bytes, bytes = (w + 7) >> 3;
    int r0 = dy < 0 ? -dy : 0, r1 = h - (dy > 0 ? dy : 0);
    uint8_t *tmp = malloc((size_t)bytes + 1);
    if (!tmp) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    for (int y = r0; dx != 0 && y < r1; y++) {
        uint8_t *row = bits + (size_t)y * rb;
        int lo = dx < 0 ? -dx : 0, hi = dx > 0 ? w - dx : w;   /* pixels kept before the smear */
        memset(tmp, 0, (size_t)bytes + 1);
        for (int x = 0; x < w; x++) {
            int on = 0;
            for (int k = 0; k <= (dx > 0 ? dx : -dx) && !on; k++) {
                int s = dx > 0 ? x - k : x + k;
                on = s >= lo && s < hi && sbit_px(row, s);
            }
            if (on) tmp[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        }
        memcpy(row, tmp, (size_t)bytes);
        if (rb > bytes) memset(row + bytes, 0, (size_t)(rb - bytes));
    }
    free(tmp);
    if (dy > 0) {
        for (int y = h - dy; y < h; y++)
            if (y >= 0) memset(bits + (size_t)y * rb, 0, (size_t)rb);
        for (int y = h - 1; y > 0; y--)
            for (int k = 1; k <= dy && y - k >= 0; k++)
                for (int b = 0; b < bytes; b++) bits[(size_t)y * rb + b] |= bits[(size_t)(y - k) * rb + b];
    } else if (dy < 0) {
        for (int y = 0; y < -dy && y < h; y++) memset(bits + (size_t)y * rb, 0, (size_t)rb);
        for (int y = 0; y < h - 1; y++)
            for (int k = 1; k <= -dy && y + k < h; k++)
                for (int b = 0; b < bytes; b++) bits[(size_t)y * rb + b] |= bits[(size_t)(y + k) * rb + b];
    }
}

/* sbit_ExpandSubpixelFromMono 0x1800dad2c (src, dst, width, height, srcRowBytes, dstRowBytes,
 * factor): each pixel of the 1-bit bitmap becomes `factor` pixels (most significant bit
 * first), the rest of each destination row cleared */
TT_EXPORT void idwr_native_sbit_ExpandSubpixelFromMono(const uint8_t *src, uint8_t *dst, uint16_t width,
                                                      uint16_t height, uint16_t src_rb, uint16_t dst_rb,
                                                      uint16_t factor) {
    if (width == 0 || height == 0 || src == NULL || dst == NULL) return;
    uint8_t *row = malloc(dst_rb ? dst_rb : 1);
    if (!row) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    for (int y = height - 1; y >= 0; y--) {   /* bottom up, as the original (in-place safe) */
        const uint8_t *s = src + (size_t)y * src_rb;
        memset(row, 0, dst_rb);
        for (int x = 0; x < width; x++)
            if (s[x >> 3] >> (7 - (x & 7)) & 1)
                for (int k = 0; k < factor; k++) {
                    int o = x * factor + k;
                    if ((o >> 3) < dst_rb) row[o >> 3] |= (uint8_t)(0x80 >> (o & 7));
                }
        memcpy(dst + (size_t)y * dst_rb, row, dst_rb);
    }
    free(row);
}

/* ---- C++ runtime and DWrite infrastructure the CFF glue reaches (reimpl/cff/glue.c) ---- */

/* operator new 0x180117b04 (throws std::bad_alloc: here E_OUTOFMEMORY through the catch frame) */
TT_EXPORT void *idwr_native_operator_new(uint64_t n) {
    void *p = malloc(n ? (size_t)n : 1);
    if (!p) idwr_raise(IDWR_HR_E_OUTOFMEMORY);
    return p;
}
/* operator delete(void *, size_t) 0x180117af8 */
TT_EXPORT void idwr_native_operator_delete(void *p, uint64_t n) {
    (void)n;
    free(p);
}
/* std::_Deallocate<16, 0>(void *, size_t) 0x1800f6274: the vectors here come from malloc */
TT_EXPORT void idwr_native_Deallocate16(void *p, uint64_t n) {
    (void)n;
    free(p);
}
/* std::vector<T>::_Tidy 0x180101918, 0x180101f48 (two ICF-shared instances): frees the
 * storage and empties the vector */
TT_EXPORT void idwr_native_VectorTidy(idwr_vec *v) {
    free(v->first);
    v->first = v->last = v->end = NULL;
}
/* IsInputAssertEnabled 0x180032f40: the input assertions are off in the shipped DLL */
TT_EXPORT uint8_t idwr_native_IsInputAssertEnabled(void) { return 0; }
/* FailAsExceptionPolicy<FileFormatException>::SafeIntOnOverflow 0x18010c5a0 */
TT_EXPORT void idwr_native_SafeIntOnOverflow_FileFormat(void) { idwr_raise((int32_t)0x88985000); }
/* ScopedPtr<FontRasterizer::IFontRasterizer>::~ScopedPtr 0x180090a94 (ICF-shared): the deleting
 * destructor (vtable slot 0) of the object held */
TT_EXPORT void idwr_native_ScopedPtrRasterizer_dtor(void **holder) {
    void *p = *holder;
    if (p) ((void (*)(void *, uint32_t))(*(void ***)p)[0])(p, 1);
}
/* ScopedPtr<FontRasterizer::IFontRasterizer>::Attach 0x180090ff4: the old object deleted, the
 * new one kept */
TT_EXPORT void idwr_native_ScopedPtrRasterizer_Attach(void **holder, void *p) {
    void *old = *holder;
    *holder = p;
    if (old && old != p) ((void (*)(void *, uint32_t))(*(void ***)old)[0])(old, 1);
}
/* ClampSigned<short>(int64) 0x1801346a4 */
TT_EXPORT int16_t idwr_native_ClampSignedShort(int64_t v) {
    return (int16_t)(v < -0x8000 ? -0x8000 : v > 0x7fff ? 0x7fff : v);
}
/* _purecall 0x18010def0, and the originals the standalone path does not model: they are
 * reached only on paths outside its scope (named where they are listed below) and stop the
 * call with IDWR_ERR_UNMODELLED */
TT_EXPORT void idwr_native_purecall(void) { idwr_raise(IDWR_ERR_UNMODELLED); }
/* InitEdgeList 0x1800e45a4 (the QReducer's, but the interpreter calls it for every glyph on two
 * records of its run buffer; not in reimpl/cff/qreduce_swaps.txt): the 7 instructions of the
 * original. An empty edge list: count 1
 * (+0), the head (+0x50) at the sentinel (+0x38), whose bounds (+0x40, +0x44) are 0xd8300001
 * and whose +0x48 is 0; returns the sentinel. The port in reimpl/cff/qreduce.c replaces it once
 * it is listed in reimpl/cff/qreduce_swaps.txt. */
TT_EXPORT void *idwr_native_InitEdgeList(uint8_t *list) {
    *(uint32_t *)(list + 0x48) = 0;
    *(uint32_t *)(list + 0x40) = 0xd8300001u;
    *(uint32_t *)(list + 0x44) = 0xd8300001u;
    *(uint8_t **)(list + 0x50) = list + 0x38;
    *(uint32_t *)list = 1;
    return list + 0x38;
}
TT_EXPORT void idwr_native_unmodelled(void) { idwr_raise(IDWR_ERR_UNMODELLED); }

#ifdef IDWR_CFF
/* the stand-ins of reimpl/idwr/cffraster.c: DWrite objects the CFF glue builds on the font */
void idwr_cffn_OpenTypeFace_ctor(void), idwr_cffn_OpenTypeTables_dtor(void), idwr_cffn_Fragment_ctor(void),
    idwr_cffn_Fragment_Initialize(void), idwr_cffn_Fragment_dtor(void), idwr_cffn_HorizontalMetrics_Initialize(void),
    idwr_cffn_HorizontalMetrics_GetGlyphAdvance(void), idwr_cffn_HorizontalMetrics_dtor(void),
    idwr_cffn_VerticalMetrics_Initialize(void), idwr_cffn_VerticalMetrics_TryGet(void), idwr_cffn_VerticalMetrics_dtor(void),
    idwr_cffn_HvarTable_ctor(void), idwr_cffn_VvarTable_ctor(void), idwr_cffn_ItemVariations_SetInstanceCoordinates(void),
    idwr_cffn_ItemVariations_dtor(void), idwr_cffn_Hvar_GetAdvanceDelta(void), idwr_cffn_Vvar_GetAdvanceHeightDelta(void),
    idwr_cffn_Vvar_GetTsbDelta(void), idwr_cffn_ComputeNormalizedAxisCoordinates(void),
    idwr_cffn_CalculateTypoAscenderDescender(void), idwr_cffn_SetDesignTransform(void);
extern void *idwr_cffn_vft_CffRasterizer[], *idwr_cffn_vft_IFontRasterizer[];
#endif

/* RVA -> helper (orig.c searches it in standalone mode, linearly): the CFF glue's
 * infrastructure first (with -DIDWR_CFF, the stand-ins of reimpl/idwr/cffraster.c), then the
 * TrueType scaler's and the glyph run layer's, sorted by RVA */
const idwr_native_entry idwr_native_table[] = {
    {0x009180, "LogAndThrow<FileFormatException>", (void *)idwr_native_LogAndThrow},
    {0x032f40, "IsInputAssertEnabled", (void *)idwr_native_IsInputAssertEnabled},
    {0x090a94, "ScopedPtr<IFontRasterizer>::~ScopedPtr", (void *)idwr_native_ScopedPtrRasterizer_dtor},
    {0x090ff4, "ScopedPtr<IFontRasterizer>::Attach", (void *)idwr_native_ScopedPtrRasterizer_Attach},
    {0x0e45a4, "InitEdgeList", (void *)idwr_native_InitEdgeList},
    {0x0f6274, "std::_Deallocate<16,0>", (void *)idwr_native_Deallocate16},
    {0x101918, "std::vector<unsigned int *>::_Tidy", (void *)idwr_native_VectorTidy},
    {0x101f48, "std::vector<int>::_Tidy", (void *)idwr_native_VectorTidy},
    {0x10c5a0, "FailAsExceptionPolicy<FileFormatException>::SafeIntOnOverflow", (void *)idwr_native_SafeIntOnOverflow_FileFormat},
    {0x10def0, "_purecall", (void *)idwr_native_purecall},
    {0x117af8, "operator delete", (void *)idwr_native_operator_delete},
    {0x117b04, "operator new", (void *)idwr_native_operator_new},
    {0x1346a4, "ClampSigned<short>", (void *)idwr_native_ClampSignedShort},
#ifdef IDWR_CFF
    {0x033050, "HorizontalMetrics::GetGlyphAdvance", (void *)idwr_cffn_HorizontalMetrics_GetGlyphAdvance},
    {0x033b40, "ItemVariationsTable::SetInstanceCoordinates", (void *)idwr_cffn_ItemVariations_SetInstanceCoordinates},
    {0x057858, "FontFragmentPtr<unsigned char>::~FontFragmentPtr", (void *)idwr_cffn_Fragment_dtor},
    {0x057aa8, "FontFragmentPtr<unsigned char>::Initialize", (void *)idwr_cffn_Fragment_Initialize},
    {0x07af5c, "VerticalMetricVariationsTable::VerticalMetricVariationsTable", (void *)idwr_cffn_VvarTable_ctor},
    {0x07b17c, "HorizontalMetricVariationsTable::HorizontalMetricVariationsTable", (void *)idwr_cffn_HvarTable_ctor},
    {0x07b8e8, "ComputeNormalizedAxisCoordinates", (void *)idwr_cffn_ComputeNormalizedAxisCoordinates},
    {0x08b294, "CalculateTypoAscenderDescender", (void *)idwr_cffn_CalculateTypoAscenderDescender},
    {0x08bab4, "VerticalMetrics::TryGetVerticalMetrics", (void *)idwr_cffn_VerticalMetrics_TryGet},
    {0x090974, "VerticalMetrics::~VerticalMetrics", (void *)idwr_cffn_VerticalMetrics_dtor},
    {0x090990, "OpenTypeTables::~OpenTypeTables", (void *)idwr_cffn_OpenTypeTables_dtor},
    {0x090abc, "HorizontalMetrics::~HorizontalMetrics", (void *)idwr_cffn_HorizontalMetrics_dtor},
    {0x090ad8, "ItemVariationsTable::~ItemVariationsTable", (void *)idwr_cffn_ItemVariations_dtor},
    {0x092f4c, "OpenTypeFace::OpenTypeFace", (void *)idwr_cffn_OpenTypeFace_ctor},
    {0x0ad290, "HorizontalMetricVariationsTable::GetAdvanceDelta", (void *)idwr_cffn_Hvar_GetAdvanceDelta},
    {0x0b4e44, "FontFragmentPtr<unsigned char>::FontFragmentPtr", (void *)idwr_cffn_Fragment_ctor},
    {0x0c381c, "VerticalMetricVariationsTable::GetAdvanceHeightDelta", (void *)idwr_cffn_Vvar_GetAdvanceHeightDelta},
    {0x0c6840, "VerticalMetrics::Initialize", (void *)idwr_cffn_VerticalMetrics_Initialize},
    {0x0cbd5c, "HorizontalMetrics::Initialize", (void *)idwr_cffn_HorizontalMetrics_Initialize},
    {0x0de59c, "OpenTypeFontMetricsBuilder::SetDesignTransform", (void *)idwr_cffn_SetDesignTransform},
    {0x1403f0, "VerticalMetricVariationsTable::GetTsbDelta", (void *)idwr_cffn_Vvar_GetTsbDelta},
    {0x199e60, "FontRasterizer::IFontRasterizer::`vftable'", (void *)idwr_cffn_vft_IFontRasterizer},
    {0x19a780, "CffRasterizer::`vftable'", (void *)idwr_cffn_vft_CffRasterizer},
    /* not modelled: the FileFormatException fallback of OpenTypeCffFontMetricsBuilder::
     * GetGlyphMetrics (black-box metrics, VORG), the metrics builders' own construction (idwr
     * sets the builder up itself), and the raw CFF face kind (7), which no OpenType file has */
    {0x050540, "FontFileReference::~FontFileReference", (void *)idwr_native_unmodelled},
    {0x051af0, "OpenTypeFontMetricsBuilder::OpenTypeFontMetricsBuilder", (void *)idwr_native_unmodelled},
    {0x0785e0, "FontFaceRegionBuilder::~FontFaceRegionBuilder", (void *)idwr_native_unmodelled},
    {0x07d548, "FontFileReference::GetFileSize", (void *)idwr_native_unmodelled},
    {0x08b910, "OpenTypeFontMetricsBuilder::GetGlyphMetricsFromBlackBox", (void *)idwr_native_unmodelled},
    {0x0908ec, "OpenTypeFontMetricsBuilder::~OpenTypeFontMetricsBuilder", (void *)idwr_native_unmodelled},
    {0x0a5ab4, "FontFaceReference::FontFaceReference", (void *)idwr_native_unmodelled},
    {0x0de49c, "VorgTable::VorgTable", (void *)idwr_native_unmodelled},
    {0x1404e8, "VorgTable::TryGetValue", (void *)idwr_native_unmodelled},
    {0x199e08, "IFontMetricsBuilder::`vftable'", (void *)idwr_native_unmodelled},
    {0x19b338, "OpenTypeCffFontMetricsBuilder::`vftable'", (void *)idwr_native_unmodelled},
    {0x1ba588, "RawCffFontMetricsBuilder::`vftable'", (void *)idwr_native_unmodelled},
#endif
    {0x01cf60, "CaptureStack", (void *)idwr_native_CaptureStack},
    {0x024a54, "FontFaceLight::GetFontWeight", (void *)idwr_native_GetFontWeight},
    {0x067b70, "DWRITE_FixMul", (void *)idwr_native_FixMul},
    {0x0687bc, "mth_FixXYMul", (void *)idwr_native_FixXYMul},
    {0x069800, "DWRITE_FixDiv", (void *)idwr_native_FixDiv},
    {0x06da90, "CompDiv", (void *)idwr_native_CompDiv},
    {0x0910f0, "FontRasterizer::CreateRasterizer", (void *)idwr_native_CreateRasterizer},
    {0x0aa7f0, "Exception::Exception", (void *)idwr_native_Exception_ctor},
    {0x0c895c, "SafeInt<int>::operator*", (void *)idwr_native_SafeIntMul},
    {0x0d6084, "ScalerOverScale", (void *)idwr_native_ScalerOverScale},
    {0x0d942c, "operator*<int,int>", (void *)idwr_native_SafeIntMulIntInt},
    {0x0dad2c, "sbit_ExpandSubpixelFromMono", (void *)idwr_native_sbit_ExpandSubpixelFromMono},
    {0x0dcb00, "return 0 (folded)", (void *)idwr_native_return0},
    {0x0e5100, "empty function (folded)", (void *)idwr_native_nop},
    {0x0ed964, "ROWBYTESLONG<int>", (void *)idwr_native_ROWBYTESLONG},
    {0x0f61e0, "std::vector<uint16_t>::_Reallocate<0>", (void *)idwr_native_VecU16_Reallocate},
    {0x0f7174, "std::vector<uint32_t>::_Resize_reallocate", (void *)idwr_native_VecU32_ResizeReallocate},
    {0x0f7dc0, "std::vector<uint16_t>::_Emplace_reallocate", (void *)idwr_native_VecU16_EmplaceReallocate},
    {0x0f9848, "LogAndThrow<OSException>", (void *)idwr_native_LogAndThrow},
    {0x0fa5cc, "SafeIntExceptionHandler<Exception>::SafeIntOnOverflow", (void *)idwr_native_SafeIntOnOverflow_Exception},
    {0x0fa608, "LogAndThrow<IntegerOverflowException>", (void *)idwr_native_LogAndThrow},
    {0x0fb6cc, "LogAndThrow<ArgumentException>", (void *)idwr_native_LogAndThrow},
    {0x1090b0, "SafeIntExceptionHandler<SafeIntRasterizerException>::SafeIntOnOverflow", (void *)idwr_native_SafeIntOnOverflow},
    {0x10b7fc, "mth_DivShiftLong", (void *)idwr_native_DivShiftLong},
    {0x10bedc, "LogAndThrow<NotSupportedException>", (void *)idwr_native_LogAndThrow},
    {0x10bf54, "LogAndThrow<ArgumentOutOfRangeException>", (void *)idwr_native_LogAndThrow},
    {0x10bf90, "LogAndThrow<InvalidCacheDataException>", (void *)idwr_native_LogAndThrow},
    {0x10c4f8, "IntegerOverflowException::IntegerOverflowException", (void *)idwr_native_IntegerOverflowException_ctor},
    {0x10cb50, "FailAssert", (void *)idwr_native_FailAssert},
    {0x14cd24, "SafeIntExceptionHandler<SafeIntRasterizerException>::SafeIntOnDivZero", (void *)idwr_native_SafeIntOnDivZero},
    {0x14ef90, "sbit_Embolden", (void *)idwr_native_sbit_Embolden},
    {0, NULL, NULL},
};
