/* The DWrite glue of the CFF rasterizer (notes/cff.md, group 1 "glue"): CffRasterizer and its
 * Implementation, the error translation, and the CFF metrics builders, 41 functions.
 *
 *   CffRasterizer (IFontRasterizer, vtable 0x19a780)
 *     0xb48d4 ctor, 0xdf8fc dtor, 0xdf8c0 deleting dtor, 0x14cc00 IFontRasterizer deleting dtor
 *     0xdc260 NewTransform, 0xe22c0 NewGlyph, 0x8c3d0 GetBitmap, 0x8bdc0 GetMetrics,
 *     0x8ad00 GetOutline, 0x14cc60 GetGlyphCount, 0x14cc40 GetDesignUnitsPerEm
 *   CffRasterizer::Implementation (0x168 bytes, layout in notes/cff.md)
 *     0xb4984 ctor, 0x8eee0 dtor, 0x8dd08 deleting dtor, 0xb4d78 NewFont,
 *     0xdc2d4 NewTransform, 0x8dad8 GetBitmap, 0x8bfb0 GetMetrics, 0x8ad60 GetOutline,
 *     0x8be20 ConvertAdobeGlyphMetrics, 0xb4e78 HandleAdobeErrorCode,
 *     0x8ace0 GetBitmapMemory (= GetOutlineMemory, ICF), 0xdc450 GetAdobeOverscaleMode,
 *     0x8c4d4 ScopedArray<unsigned char>::Attach
 *   OpenTypeCffFontMetricsBuilder (vtable 0x19b338)
 *     0xde3d0 ctor, 0xdd9dc dtor, 0xdd9a0 deleting dtor, 0xe2340 GetGlyphMetrics
 *   RawCffFontMetricsBuilder (vtable 0x1ba588), RawCffFontFaceBuilder (vtable 0x1ba548)
 *     0x134978 ctor, 0x1349f0 dtor, 0x134b10 deleting dtor, 0x135280 GetGlyphMetrics,
 *     0x134f60 GetDesignUnitsPerEm, 0x135520 RawCffFontFaceBuilder::ReadFontFace,
 *     0xf1c80 RawCffFontFaceBuilder deleting dtor
 *   deleting destructors of members (ICF-shared with the rest of DWrite)
 *     0x8efe4 OpenTypeFace, 0x10c200 HorizontalMetricVariationsTable,
 *     0x10c230 VerticalMetricVariationsTable, 0x134a50 FontFaceRegionBuilder,
 *     0x134a90 IFontMetricsBuilder, 0x134ad0 OpenTypeFontMetricsBuilder
 *
 * Every call out (the Adobe front end, DWrite's OpenType tables, new/delete, exceptions)
 * goes through CFF_ORIG, also to the other functions of this file, so each port swaps alone.
 *
 * C++ exception handling the ports do reproduce: the catch (FileFormatException const &) of
 * both GetGlyphMetrics (glue_try_ffe, a frame with its own language handler), and the noexcept
 * of ScopedArray::Attach (glue_noexcept_call: an exception terminates the process).
 * Exceptions from HandleAdobeErrorCode and the SafeInt checks pass through the C frames,
 * which have unwind data.
 *
 * What a C port cannot reproduce, all on error paths only:
 * - the __try/__except of Implementation::{NewFont, GetBitmap, GetMetrics, GetOutline}
 *   (filter FilterInPageExceptions with FontFileReference::GetPageableRegion, handler
 *   TranslateStructuredException -> PageableException): an in-page I/O error of a mapped font
 *   file stays a structured exception here;
 * - the C++ unwind actions: when a DWrite exception passes through a port, the members
 *   constructed so far (constructors) and the MXCSR guard of the vtable wrappers are not
 *   undone. Rendering a valid font throws nothing.
 *
 * Exports: cff_<name> through CFF_EXPORT (the register-preserving thunk), except the two
 * NewTransform, which take six arguments: their thunk (GLUE_EXPORT6 below) also copies the two
 * stack arguments.
 *
 * Negative controls (-DCFF_CONTROL_<NAME>, cff_check.py --control <name>), see each function:
 * GLUE_MEMORY, GLUE_ERROR, GLUE_OVERSCALE, GLUE_OBLIQUE, GLUE_OUTLINE, GLUE_ADVANCE, GLUE_LSB,
 * GLUE_VMTX, GLUE_HVAR, GLUE_GETMETRICS, GLUE_BITMAP, GLUE_NEWFONT, GLUE_CTOR,
 * GLUE_VT_TRANSFORM, GLUE_GLYPH, GLUE_VT_BITMAP, GLUE_VT_METRICS, GLUE_VT_OUTLINE, GLUE_COUNT,
 * GLUE_BUILDER, GLUE_DESIGN; GLUE_CALLS counts the calls of every port. */
#include "cff_port.h"
#include "../truetype/fatal.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#define GLUE_HAS_MXCSR 1
#endif
#include <stdlib.h>

/* ---------------------------------------------------------------------------------------- */
/* memory accessors (byte offsets into DWrite objects; memcpy keeps them alias-safe) */

static inline uint8_t rd8(const void *p, int o) { return ((const uint8_t *)p)[o]; }
static inline uint16_t rd16(const void *p, int o) { uint16_t v; memcpy(&v, (const uint8_t *)p + o, 2); return v; }
static inline uint32_t rd32(const void *p, int o) { uint32_t v; memcpy(&v, (const uint8_t *)p + o, 4); return v; }
static inline uint64_t rd64(const void *p, int o) { uint64_t v; memcpy(&v, (const uint8_t *)p + o, 8); return v; }
static inline void *rdp(const void *p, int o) { void *v; memcpy(&v, (const uint8_t *)p + o, sizeof v); return v; }
static inline void wr8(void *p, int o, uint8_t v) { ((uint8_t *)p)[o] = v; }
static inline void wr16(void *p, int o, uint16_t v) { memcpy((uint8_t *)p + o, &v, 2); }
static inline void wr32(void *p, int o, uint32_t v) { memcpy((uint8_t *)p + o, &v, 4); }
static inline void wr64(void *p, int o, uint64_t v) { memcpy((uint8_t *)p + o, &v, 8); }
static inline void wrp(void *p, int o, const void *v) { memcpy((uint8_t *)p + o, &v, sizeof v); }
#define AT(p, o) ((void *)((uint8_t *)(p) + (o)))

/* a C++ virtual call: slot `i` of the object's vtable */
#define VCALL(type, obj, i) (((type *)rdp((obj), 0))[i])

/* ---------------------------------------------------------------------------------------- */
/* the originals called (RVAs in DWrite.dll 10.0.26100.9711) */

/* this file */
#define RVA_Impl_GetBitmapMemory 0x8ace0
#define RVA_Impl_GetOutline 0x8ad60
#define RVA_Impl_ConvertAdobeGlyphMetrics 0x8be20
#define RVA_Impl_GetMetrics 0x8bfb0
#define RVA_ScopedArray_Attach 0x8c4d4
#define RVA_Impl_GetBitmap 0x8dad8
#define RVA_Impl_ScalarDtor 0x8dd08
#define RVA_Impl_Dtor 0x8eee0
#define RVA_OpenTypeFace_ScalarDtor 0x8efe4
#define RVA_Impl_Ctor 0xb4984
#define RVA_Impl_NewFont 0xb4d78
#define RVA_Impl_HandleAdobeErrorCode 0xb4e78
#define RVA_Impl_NewTransform 0xdc2d4
#define RVA_GetAdobeOverscaleMode 0xdc450
#define RVA_OTCffMetricsBuilder_Dtor 0xdd9dc
#define RVA_CffRasterizer_Dtor 0xdf8fc
#define RVA_HvarTable_ScalarDtor 0x10c200
#define RVA_VvarTable_ScalarDtor 0x10c230
#define RVA_RawCffMetricsBuilder_Dtor 0x1349f0
/* the Adobe front end and parser */
#define RVA_AdobeInternalGetOutline 0x8aef0
#define RVA_AdobeInternalGetMetrics 0x8c0c0
#define RVA_AdobeInternalGetBitmap 0x8c528
#define RVA_AdobeCFFConvertNDV 0x8dd34
#define RVA_AdobeFreeFontInst 0x8ecdc
#define RVA_AdobeNewFont 0xb45a0
/* DWrite infrastructure */
#define RVA_LogAndThrow_FileFormatException 0x9180
#define RVA_IsInputAssertEnabled 0x32f40
#define RVA_HorizontalMetrics_GetGlyphAdvance 0x33050
#define RVA_ItemVariationsTable_SetInstanceCoordinates 0x33b40
#define RVA_FontFileReference_Dtor 0x50540
#define RVA_OpenTypeFontMetricsBuilder_Ctor 0x51af0
#define RVA_FontFragmentPtr_Dtor 0x57858
#define RVA_FontFragmentPtr_Initialize 0x57aa8
#define RVA_DWRITE_FixMul 0x67b70
#define RVA_FontFaceRegionBuilder_Dtor 0x785e0
#define RVA_VerticalMetricVariationsTable_Ctor 0x7af5c
#define RVA_HorizontalMetricVariationsTable_Ctor 0x7b17c
#define RVA_ComputeNormalizedAxisCoordinates 0x7b8e8
#define RVA_FontFileReference_GetFileSize 0x7d548
#define RVA_CalculateTypoAscenderDescender 0x8b294
#define RVA_VerticalMetrics_TryGetVerticalMetrics 0x8bab4
#define RVA_OpenTypeFontMetricsBuilder_Dtor 0x908ec
#define RVA_VerticalMetrics_Dtor 0x90974
#define RVA_OpenTypeTables_Dtor 0x90990
#define RVA_ScopedPtr_Dtor 0x90a94
#define RVA_HorizontalMetrics_Dtor 0x90abc /* also VorgTable's (ICF) */
#define RVA_ItemVariationsTable_Dtor 0x90ad8
#define RVA_ScopedPtr_IFontRasterizer_Attach 0x90ff4
#define RVA_FontRasterizer_CreateRasterizer 0x910f0
#define RVA_OpenTypeFace_Ctor 0x92f4c
#define RVA_FontFaceReference_CopyCtor 0xa5ab4
#define RVA_Exception_Ctor 0xaa7f0
#define RVA_HorizontalMetricVariationsTable_GetAdvanceDelta 0xad290
#define RVA_FontFragmentPtr_Ctor 0xb4e44
#define RVA_VerticalMetrics_Initialize 0xc6840
#define RVA_HorizontalMetrics_Initialize 0xcbd5c
#define RVA_VerticalMetricVariationsTable_GetAdvanceHeightDelta 0xc381c
#define RVA_VorgTable_Ctor 0xde49c
#define RVA_VorgTable_TryGetValue 0x1404e8
#define RVA_OpenTypeFontMetricsBuilder_GetGlyphMetricsFromBlackBox 0x8b910
#define RVA_FailAsExceptionPolicy_FileFormat_SafeIntOnOverflow 0x10c5a0
#define RVA_OpenTypeFontMetricsBuilder_SetDesignTransform 0xde59c
#define RVA_std_Deallocate16 0xf6274
#define RVA_SafeIntOnOverflow 0xfa5cc
#define RVA_vector_ptr_Tidy 0x101918 /* std::vector<unsigned int *>::_Tidy (ICF) */
#define RVA_vector_int_Tidy 0x101f48 /* std::vector<int>::_Tidy (ICF) */
#define RVA_FailAssert 0x10cb50
#define RVA_ClampSigned_short 0x1346a4
#define RVA_operator_delete 0x117af8 /* sized */
#define RVA_operator_new 0x117b04
#define RVA_terminate 0x10dfc8 /* terminate (-> __imp__o_terminate) */
#define RVA_VerticalMetricVariationsTable_GetTsbDelta 0x1403f0
#define RVA_imp_calloc 0x1be380 /* IAT slots: __imp__o_calloc, __imp__o_free */
#define RVA_imp_free 0x1be3b0
/* vtables */
#define RVA_vft_IFontMetricsBuilder 0x199e08
#define RVA_vft_IFontRasterizer 0x199e60
#define RVA_vft_CffRasterizer 0x19a780
#define RVA_vft_OpenTypeCffFontMetricsBuilder 0x19b338
#define RVA_vft_RawCffFontMetricsBuilder 0x1ba588

#define HR_DWRITE_E_FILEFORMAT ((int32_t)0x88985000)
#define TAG_newcff 0x66666377656eull  /* "newcff": Implementation::Implementation */
#define TAG_cfferr 0x727265666663ull  /* "cfferr": HandleAdobeErrorCode */
#define TAG_cffbldr 0x72646c62666663ull /* "cffbldr": OpenTypeCffFontMetricsBuilder */

typedef void fn_v_p(void *);
typedef void *fn_p_p(void *);
typedef void *fn_p_pp(void *, void *);
typedef void fn_v_pp(void *, void *);

static void *op_new(uint64_t n) { return CFF_ORIG(void *(*)(uint64_t), RVA_operator_new)(n); }
static void op_delete(void *p, uint64_t n) { CFF_ORIG(void (*)(void *, uint64_t), RVA_operator_delete)(p, n); }

/* the CRT DWrite allocates the bitmap and outline buffers with (and frees them with) */
static void *crt_calloc(uint64_t n, uint64_t size) {
#ifdef IDWR_STANDALONE
    return calloc(n, size);
#else
    return (*(void *(**)(uint64_t, uint64_t))idwr_orig(RVA_imp_calloc))(n, size);
#endif
}
static void crt_free(void *p) {
#ifdef IDWR_STANDALONE
    free(p);
#else
    (*(void (**)(void *))idwr_orig(RVA_imp_free))(p);
#endif
}

static void safeint_overflow(void) { CFF_ORIG(void (*)(void), RVA_SafeIntOnOverflow)(); }

/* A call inside a noexcept function: a C++ exception that reaches this frame terminates the
 * process, as __CxxFrameHandler4 does for a function marked NoExcept (after the throw has
 * logged its event). glue_noexcept_call(f) calls f() under such a frame. */
#if defined(IDWR_STANDALONE) || !defined(_WIN32)
static void glue_noexcept_call(void (*f)(void)) {
    f();
    IDWR_FATAL("glue.c:203");
}
#else
typedef struct _EXCEPTION_RECORD EXCEPTION_RECORD_;
void glue_noexcept_call(void (*f)(void));
int glue_noexcept_handler(void *rec, void *frame, void *ctx, void *dispatch);
int glue_noexcept_handler(void *rec, void *frame, void *ctx, void *dispatch) {
    (void)frame, (void)ctx, (void)dispatch;
    uint32_t code = rd32(rec, 0), flags = rd32(rec, 4);
    if (code == 0xe06d7363u && !(flags & 0x66)) /* a C++ exception, search phase */
        CFF_ORIG(void (*)(void), RVA_terminate)();
    return 1; /* ExceptionContinueSearch */
}
__asm__(".text\n"
        ".globl glue_noexcept_call\n"
        ".def glue_noexcept_call; .scl 2; .type 32; .endef\n"
        ".p2align 4\nglue_noexcept_call:\n"
        ".seh_proc glue_noexcept_call\n"
        ".seh_handler glue_noexcept_handler, @except\n"
        "subq $0x28, %rsp\n.seh_stackalloc 0x28\n"
        ".seh_endprologue\n"
        "callq *%rcx\n"
        "nop\n"
        "addq $0x28, %rsp\n"
        "retq\n"
        ".seh_endproc\n");
#endif
static void fail_assert(uint32_t line) { CFF_ORIG(void (*)(uint32_t), RVA_FailAssert)(line); }

/* Exception ex(DWRITE_E_FILEFORMAT, 0); LogAndThrow<FileFormatException>(ex, tag, line) */
static void throw_file_format(uint64_t tag, uint32_t line) {
    _Alignas(16) uint32_t ex[4] = {0};
    CFF_ORIG(void *(*)(void *, int32_t, uint32_t), RVA_Exception_Ctor)(ex, HR_DWRITE_E_FILEFORMAT, 0);
    CFF_ORIG(void (*)(void *, uint64_t, uint32_t), RVA_LogAndThrow_FileFormatException)(ex, tag, line);
}

static void handle_adobe_error(int32_t code) {
    CFF_ORIG(void (*)(int32_t), RVA_Impl_HandleAdobeErrorCode)(code);
}

/* The MXCSR guard of the CffRasterizer vtable methods: round to nearest with every exception
 * masked (0x1f80) while the Adobe code runs. Restored only when it was changed, with the
 * exception flags (bits 0-5) cleared. */
static inline uint32_t mxcsr_enter(void) {
#ifdef GLUE_HAS_MXCSR
    uint32_t m = _mm_getcsr();
    if ((m & 0xff80) != 0x1f80)
        _mm_setcsr(0x1f80);
    return m;
#else
    return 0x1f80;
#endif
}
static inline void mxcsr_leave(uint32_t m) {
#ifdef GLUE_HAS_MXCSR
    if ((m & 0xff80) != 0x1f80)
        _mm_setcsr(m & 0xffffffc0u);
#else
    (void)m;
#endif
}

/* try { body(arg); } catch (FileFormatException const &) { return 1; } return 0;
 * The two CFF metrics builders' GetGlyphMetrics catch FileFormatException (TypeDescriptor
 * .?AVFileFormatException@@ at 0x231518, a trivial destructor: _TI2 0x22d910 has no
 * pmfnUnwind). glue_try_ffe's language handler matches a C++ exception (0xe06d7363) whose
 * catchable types include that descriptor and unwinds to its own frame (RtlUnwindEx), which
 * then returns 1; every other exception passes. */
#define RVA_td_FileFormatException 0x231518
#if defined(IDWR_STANDALONE) || !defined(_WIN32)
static int glue_try_ffe(void (*body)(void *), void *arg) {
    body(arg);
    return 0;
}
#else
#include <windows.h>
int glue_try_ffe(void (*body)(void *), void *arg);
void glue_try_ffe_landing(void);
EXCEPTION_DISPOSITION glue_ffe_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc);
EXCEPTION_DISPOSITION glue_ffe_handler(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, DISPATCHER_CONTEXT *dc) {
    if (rec->ExceptionFlags & EXCEPTION_UNWIND)
        return ExceptionContinueSearch;
    if (rec->ExceptionCode != 0xe06d7363u || rec->NumberParameters < 4)
        return ExceptionContinueSearch;
    ULONG_PTR magic = rec->ExceptionInformation[0];
    if (magic < 0x19930520 || magic > 0x19930522 || !rec->ExceptionInformation[2])
        return ExceptionContinueSearch;
    const uint8_t *throw_info = (const uint8_t *)rec->ExceptionInformation[2];
    const uint8_t *image = (const uint8_t *)rec->ExceptionInformation[3];
    const uint8_t *cta = image + (int32_t)rd32(throw_info, 12);
    int32_t n = (int32_t)rd32(cta, 0);
    const uint8_t *want = idwr_orig(RVA_td_FileFormatException);
    for (int32_t i = 0; i < n; i++) {
        const uint8_t *ct = image + (int32_t)rd32(cta, 4 + 4 * i);
        if (image + (int32_t)rd32(ct, 4) == want) {
            RtlUnwindEx(frame, (void *)glue_try_ffe_landing, rec, (void *)1, ctx, dc->HistoryTable);
            break; /* not reached */
        }
    }
    return ExceptionContinueSearch;
}
__asm__(".text\n"
        ".globl glue_try_ffe\n"
        ".globl glue_try_ffe_landing\n"
        ".def glue_try_ffe; .scl 2; .type 32; .endef\n"
        ".p2align 4\nglue_try_ffe:\n"
        ".seh_proc glue_try_ffe\n"
        ".seh_handler glue_ffe_handler, @except\n"
        "subq $0x28, %rsp\n.seh_stackalloc 0x28\n"
        ".seh_endprologue\n"
        "movq %rcx, %rax\n"
        "movq %rdx, %rcx\n"
        "callq *%rax\n"
        "xorl %eax, %eax\n"
        "glue_try_ffe_landing:\n"
        "addq $0x28, %rsp\n"
        "retq\n"
        ".seh_endproc\n");
#endif

/* -DCFF_CONTROL_GLUE_CALLS: not a break but a probe; every port counts its calls, read back
 * by RVA with cff_glue_calls (shows that a swapped port is the code that runs). */
#ifdef CFF_CONTROL_GLUE_CALLS
static uint32_t g_call_rva[64], g_call_n[64];
static void glue_call(uint32_t rva) {
    for (int i = 0; i < 64; i++) {
        if (g_call_rva[i] == 0)
            g_call_rva[i] = rva;
        if (g_call_rva[i] == rva) {
            g_call_n[i]++;
            return;
        }
    }
}
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
uint32_t cff_glue_calls(uint32_t rva) {
    for (int i = 0; i < 64; i++)
        if (g_call_rva[i] == rva)
            return g_call_n[i];
    return 0;
}
#define GLUE_CALL(rva) glue_call(rva)
#else
#define GLUE_CALL(rva) ((void)0)
#endif

/* ---------------------------------------------------------------------------------------- */
/* exports */

#ifdef IDWR_STANDALONE
#define GLUE_EXPORT6(name, impl) CFF_EXPORT(name, impl)
#define cff_Impl_GetBitmapMemory_impl cff_Impl_GetBitmapMemory
#define cff_CffRasterizer_GetOutline_impl cff_CffRasterizer_GetOutline
#define cff_Impl_GetOutline_impl cff_Impl_GetOutline
#define cff_CffRasterizer_GetMetrics_impl cff_CffRasterizer_GetMetrics
#define cff_Impl_ConvertAdobeGlyphMetrics_impl cff_Impl_ConvertAdobeGlyphMetrics
#define cff_Impl_GetMetrics_impl cff_Impl_GetMetrics
#define cff_CffRasterizer_GetBitmap_impl cff_CffRasterizer_GetBitmap
#define cff_ScopedArray_Attach_impl cff_ScopedArray_Attach
#define cff_Impl_GetBitmap_impl cff_Impl_GetBitmap
#define cff_Impl_ScalarDtor_impl cff_Impl_ScalarDtor
#define cff_Impl_Dtor_impl cff_Impl_Dtor
#define cff_OpenTypeFace_ScalarDtor_impl cff_OpenTypeFace_ScalarDtor
#define cff_CffRasterizer_Ctor_impl cff_CffRasterizer_Ctor
#define cff_Impl_Ctor_impl cff_Impl_Ctor
#define cff_Impl_NewFont_impl cff_Impl_NewFont
#define cff_Impl_HandleAdobeErrorCode_impl cff_Impl_HandleAdobeErrorCode
#define cff_CffRasterizer_NewTransform_impl cff_CffRasterizer_NewTransform
#define cff_Impl_NewTransform_impl cff_Impl_NewTransform
#define cff_GetAdobeOverscaleMode_impl cff_GetAdobeOverscaleMode
#define cff_OTCffMetricsBuilder_DeletingDtor_impl cff_OTCffMetricsBuilder_DeletingDtor
#define cff_OTCffMetricsBuilder_Dtor_impl cff_OTCffMetricsBuilder_Dtor
#define cff_OTCffMetricsBuilder_Ctor_impl cff_OTCffMetricsBuilder_Ctor
#define cff_CffRasterizer_DeletingDtor_impl cff_CffRasterizer_DeletingDtor
#define cff_CffRasterizer_Dtor_impl cff_CffRasterizer_Dtor
#define cff_CffRasterizer_NewGlyph_impl cff_CffRasterizer_NewGlyph
#define cff_OTCffMetricsBuilder_GetGlyphMetrics_impl cff_OTCffMetricsBuilder_GetGlyphMetrics
#define cff_RawCffFaceBuilder_DeletingDtor_impl cff_RawCffFaceBuilder_DeletingDtor
#define cff_HvarTable_ScalarDtor_impl cff_HvarTable_ScalarDtor
#define cff_VvarTable_ScalarDtor_impl cff_VvarTable_ScalarDtor
#define cff_RawCffMetricsBuilder_Ctor_impl cff_RawCffMetricsBuilder_Ctor
#define cff_RawCffMetricsBuilder_Dtor_impl cff_RawCffMetricsBuilder_Dtor
#define cff_FontFaceRegionBuilder_ScalarDtor_impl cff_FontFaceRegionBuilder_ScalarDtor
#define cff_IFontMetricsBuilder_ScalarDtor_impl cff_IFontMetricsBuilder_ScalarDtor
#define cff_OTMetricsBuilder_DeletingDtor_impl cff_OTMetricsBuilder_DeletingDtor
#define cff_RawCffMetricsBuilder_DeletingDtor_impl cff_RawCffMetricsBuilder_DeletingDtor
#define cff_RawCffMetricsBuilder_GetDesignUnitsPerEm_impl cff_RawCffMetricsBuilder_GetDesignUnitsPerEm
#define cff_RawCffMetricsBuilder_GetGlyphMetrics_impl cff_RawCffMetricsBuilder_GetGlyphMetrics
#define cff_RawCffFaceBuilder_ReadFontFace_impl cff_RawCffFaceBuilder_ReadFontFace
#define cff_IFontRasterizer_ScalarDtor_impl cff_IFontRasterizer_ScalarDtor
#define cff_CffRasterizer_GetDesignUnitsPerEm_impl cff_CffRasterizer_GetDesignUnitsPerEm
#define cff_CffRasterizer_GetGlyphCount_impl cff_CffRasterizer_GetGlyphCount
#elif defined(_WIN32)
/* CFF_EXPORT for six arguments: the same register-preserving thunk, plus a copy of the two
 * stack arguments (entry rsp + 0x28 and + 0x30) into the callee's argument area. */
#define GLUE_EXPORT6(name, impl)                                                       \
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
            "movq 0xf0(%rsp), %rax\n"                                                  \
            "movq %rax, 0x20(%rsp)\n"                                                  \
            "movq 0xf8(%rsp), %rax\n"                                                  \
            "movq %rax, 0x28(%rsp)\n"                                                  \
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

/* ---------------------------------------------------------------------------------------- */
/* CffRasterizer::Implementation: memory, errors, transforms */

/* 0x8ace0 GetBitmapMemory (and GetOutlineMemory): the Adobe memory callback, calloc(1, n).
 * Control GLUE_MEMORY: no memory (NULL). */
CFF_API void *cff_Impl_GetBitmapMemory_impl(uint32_t n) {
    GLUE_CALL(0x8ace0);
#ifdef CFF_CONTROL_GLUE_MEMORY
    (void)n;
    return NULL;
#else
    return crt_calloc(1, n);
#endif
}
CFF_EXPORT(cff_Impl_GetBitmapMemory, cff_Impl_GetBitmapMemory_impl);

/* 0xb4e78 HandleAdobeErrorCode: success is a code whose top byte is 0; every other code
 * throws FileFormatException (DWRITE_E_FILEFORMAT, tag "cfferr"), the line by class.
 * Control GLUE_ERROR: throws on success too. */
CFF_API void cff_Impl_HandleAdobeErrorCode_impl(int32_t code) {
    GLUE_CALL(0xb4e78);
    int32_t c = (int32_t)((uint32_t)code & 0xff000000u);
    uint32_t line;
    if (c > (int32_t)0xfb000000) {
        switch ((uint32_t)c) {
        case 0xfc000000u: line = 0xb6; break;
        case 0xfd000000u: line = 0xc9; break;
        case 0xfe000000u:
        case 0xff000000u: line = 0xae; break;
        case 0:
#ifdef CFF_CONTROL_GLUE_ERROR
            line = 0xd3;
            break;
#else
            return;
#endif
        default: line = 0xd3; break;
        }
    } else {
        switch ((uint32_t)c) {
        case 0xfb000000u: line = 0xb6; break;
        case 0xdd000000u: line = 0xc4; break;
        case 0xdf000000u:
        case 0xe0000000u: line = 0xae; break;
        case 0xf9000000u: line = 0xbd; break;
        case 0xfa000000u: line = 0xb6; break;
        default: line = 0xd3; break; /* 0xdc000000 included */
        }
    }
    throw_file_format(TAG_cfferr, line);
}
CFF_EXPORT(cff_Impl_HandleAdobeErrorCode, cff_Impl_HandleAdobeErrorCode_impl);

/* 0xdc450 GetAdobeOverscaleMode(x, y): 0 = 1x1 (or anything else), 1 = 6x1, 2 = 6x5,
 * 3 = 8x1, 4 = 4x4. Only the low 16 bits of each argument count. NewTransform keeps R9, R10
 * and RDX live across the call (the thunk preserves them).
 * Control GLUE_OVERSCALE: 6x5 reported as 6x1. */
CFF_API int32_t cff_GetAdobeOverscaleMode_impl(uint32_t x32, uint32_t y32) {
    GLUE_CALL(0xdc450);
    uint16_t x = (uint16_t)x32, y = (uint16_t)y32;
    if (x == 4)
        return y == 4 ? 4 : 0;
    if (x == 6) {
        if (y == 5)
#ifdef CFF_CONTROL_GLUE_OVERSCALE
            return 1;
#else
            return 2;
#endif
        return y == 1 ? 1 : 0;
    }
    if (x == 8)
        return y == 1 ? 3 : 0;
    return 0;
}
CFF_EXPORT(cff_GetAdobeOverscaleMode, cff_GetAdobeOverscaleMode_impl);

/* 0xdc2d4 Implementation::NewTransform(unused, transform, overscaleX, overscaleY, flags):
 * the device matrix (+0x8c: the transform's 2x2, x column times overscaleX, y column times
 * overscaleY), the design matrix (+0x9c: unitsPerEm << 16 on the diagonal), the overscale
 * mode (+0xb0), the Adobe flags (+0xb4: RenderingFlags bits 0 and 1), and bitmap format 3 at
 * +0xb8 for 1x1. Oblique (flags & 4) shears both matrices by 0x5700 with DWRITE_FixMul;
 * flags & 8 shears the other axis (sideways), with the opposite sign.
 * Control GLUE_OBLIQUE: shear 0x5800. */
CFF_API void cff_Impl_NewTransform_impl(void *impl, uint32_t unused, const int32_t *m, uint32_t ox32, uint32_t oy32,
                                        uint32_t flags) {
    GLUE_CALL(0xdc2d4);
    typedef int32_t fixmul_t(int32_t, int32_t);
    (void)unused;
    uint16_t ox = (uint16_t)ox32, oy = (uint16_t)oy32;
    int32_t mode = CFF_ORIG(int32_t (*)(uint32_t, uint32_t), RVA_GetAdobeOverscaleMode)(ox, oy);
    wr32(impl, 0xb0, (uint32_t)mode);
    wr32(impl, 0xb4, 0);
    uint32_t f = 0;
    if (flags & 1) {
        wr32(impl, 0xb4, 1);
        f = 1;
    }
    if (flags & 2) {
        f |= 2;
        wr32(impl, 0xb4, f);
    }
    if (ox == 1 && oy == 1)
        wr32(impl, 0xb8, 3);
    uint32_t xx = (uint32_t)ox * (uint32_t)m[0], xy = (uint32_t)ox * (uint32_t)m[1];
    uint32_t yx = (uint32_t)oy * (uint32_t)m[2], yy = (uint32_t)oy * (uint32_t)m[3];
    uint32_t em = (uint32_t)rd16(impl, 0xac) << 16;
    uint32_t d0 = em, d1 = 0, d2 = 0, d3 = em;
    if (flags & 4) {
#ifdef CFF_CONTROL_GLUE_OBLIQUE
        const int32_t shear = 0x5800;
#else
        const int32_t shear = 0x5700;
#endif
        fixmul_t *fixmul = CFF_ORIG(fixmul_t *, RVA_DWRITE_FixMul);
        if (!(flags & 8)) {
            yy += (uint32_t)fixmul((int32_t)yx, shear);
            xy += (uint32_t)fixmul((int32_t)xx, shear);
        } else {
            yx -= (uint32_t)fixmul((int32_t)yy, shear);
            xx -= (uint32_t)fixmul((int32_t)xy, shear);
        }
        uint32_t z = (uint32_t)fixmul(0, shear);
        uint32_t s = (uint32_t)fixmul((int32_t)em, shear);
        if (!(flags & 8)) {
            d1 = s;
            d3 = em + z;
        } else {
            d0 = em - z;
            d2 = 0u - s;
        }
    }
    wr32(impl, 0x94, yx);
    wr32(impl, 0x90, xy);
    wr32(impl, 0x8c, xx);
    wr32(impl, 0x98, yy);
    wr32(impl, 0x9c, d0);
    wr32(impl, 0xa4, d2);
    wr32(impl, 0xa0, d1);
    wr32(impl, 0xa8, d3);
}
GLUE_EXPORT6(cff_Impl_NewTransform, cff_Impl_NewTransform_impl);

/* 0x8c4d4 ScopedArray<unsigned char>::Attach(p, n): frees the old array, keeps p, and the
 * count as 32 bits. Attach is noexcept: when n > 0xffffffff the SafeInt overflow logs its
 * IntegerOverflowException and the process terminates. */
CFF_API void cff_ScopedArray_Attach_impl(void *sa, void *p, uint64_t n) {
    GLUE_CALL(0x8c4d4);
    crt_free(rdp(sa, 0));
    wrp(sa, 0, p);
    if (n > 0xffffffffull)
        glue_noexcept_call(safeint_overflow); /* Attach is noexcept: the throw terminates */
    wr32(sa, 8, (uint32_t)n);
}
CFF_EXPORT(cff_ScopedArray_Attach, cff_ScopedArray_Attach_impl);

/* ---------------------------------------------------------------------------------------- */
/* CffRasterizer::Implementation: the three requests */

/* the 16-byte Transform passed by value: a copy, by pointer */
typedef struct {
    _Alignas(16) uint8_t b[16];
} glue_xform;

/* AdobeCFFConvertNDV(inst, count, coords): the F2Dot14 instance coordinates at +0x100 */
static int32_t convert_ndv(void *impl, void *inst) {
    uint8_t *c = rdp(impl, 0x100), *e = rdp(impl, 0x108);
    int32_t n = (int32_t)((int64_t)(e - c) >> 1);
    return CFF_ORIG(int32_t (*)(void *, uint32_t, void *), RVA_AdobeCFFConvertNDV)(inst, (uint32_t)n, c);
}

/* 0x8ad60 Implementation::GetOutline(GlyphOutline *): the outline of the glyph of the last
 * NewGlyph with the device matrix; the buffer is kept at +0xd0 (count at +0xd8) and returned
 * as {count / 4, pointer}. AdobeInternalGetOutline's memory-callback argument is not read by
 * it (RDX is not set by the original); it gets GetOutlineMemory here.
 * The buffer holds int32 commands (2 = move to, x, y; 3 = line to, x, y; ...).
 * Control GLUE_OUTLINE: the first move-to x + 0x10000. */
CFF_API void cff_Impl_GetOutline_impl(void *impl, void *outline) {
    GLUE_CALL(0x8ad60);
    void *buf = NULL;
    int32_t count = 0;
    _Alignas(16) uint8_t metrics[0x28] = {0};
    uint32_t flags = rd32(impl, 0xb4), mode = rd32(impl, 0xb0);
    uint16_t glyph = rd16(impl, 0x8a);
    void *inst = rdp(impl, 0x80);
    glue_xform x;
    memcpy(x.b, AT(impl, 0x8c), 16);
    int32_t err = convert_ndv(impl, inst);
    if (err == 0) {
        typedef int32_t get_outline_t(void *, void *, uint32_t, void *, uint32_t, uint32_t, void *, void **, int32_t *);
        err = CFF_ORIG(get_outline_t *, RVA_AdobeInternalGetOutline)(inst, idwr_orig(RVA_Impl_GetBitmapMemory), glyph,
                                                                     x.b, mode, flags, metrics, &buf, &count);
    }
    crt_free(rdp(impl, 0xd0));
    wrp(impl, 0xd0, buf);
    uint64_t n = (uint64_t)(int64_t)count;
    if (n > 0xffffffffull)
        safeint_overflow();
    wr32(impl, 0xd8, (uint32_t)n);
    handle_adobe_error(err);
#ifdef CFF_CONTROL_GLUE_OUTLINE
    if (buf && n >= 12 && rd32(buf, 0) == 2)
        wr32(buf, 4, rd32(buf, 4) + 0x10000);
#endif
    wr32(outline, 0, (uint32_t)(n >> 2));
    wrp(outline, 8, buf);
}
CFF_EXPORT(cff_Impl_GetOutline, cff_Impl_GetOutline_impl);

/* 0x8be20 Implementation::ConvertAdobeGlyphMetrics(adobe, GlyphMetrics *out): Adobe's box
 * (+0x00 left, +0x04 top, +0x10 advance 16.16, +0x20 width, +0x24 height) to DWrite's design
 * metrics (+0x00 advanceWidth, +0x04 advanceHeight, +0x08 verticalOriginY, +0x0c lsb,
 * +0x10 rsb, +0x14 tsb, +0x18 bsb). CFF2: advance from hmtx + HVAR, advanceHeight and tsb plus
 * the VVAR deltas. Vertical metrics from vmtx/VORG (VerticalMetrics), or for faces without
 * them from the typographic ascender and descender.
 * Controls: GLUE_ADVANCE (advance rounded down), GLUE_LSB (lsb + 1), GLUE_VMTX (tsb + 1 when
 * the face has no vertical metrics), GLUE_HVAR (no HVAR delta). */
CFF_API void cff_Impl_ConvertAdobeGlyphMetrics_impl(void *impl, const void *am, void *out) {
    GLUE_CALL(0x8be20);
    int32_t adv;
    if (rd8(impl, 0xe0) == 0) {
#ifdef CFF_CONTROL_GLUE_ADVANCE
        adv = (int32_t)(rd32(am, 0x10)) >> 16;
#else
        adv = (int32_t)(rd32(am, 0x10) + 0x8000u) >> 16;
#endif
    } else {
        uint16_t g = rd16(impl, 0x8a);
        void *hvar = rdp(impl, 0x158);
        uint16_t a = CFF_ORIG(uint16_t (*)(void *, uint32_t), RVA_HorizontalMetrics_GetGlyphAdvance)(AT(impl, 0x118), g);
        int16_t d = CFF_ORIG(int16_t (*)(void *, uint32_t), RVA_HorizontalMetricVariationsTable_GetAdvanceDelta)(hvar, g);
#ifdef CFF_CONTROL_GLUE_HVAR
        d = 0;
#endif
        adv = (int32_t)((uint32_t)(int32_t)d + a);
    }
    wr32(out, 0x00, (uint32_t)adv);
    uint32_t lsb = rd32(am, 0x00);
#ifdef CFF_CONTROL_GLUE_LSB
    lsb += 1;
#endif
    wr32(out, 0x0c, lsb);
    wr32(out, 0x10, (uint32_t)adv - rd32(am, 0x20) - lsb);
    uint32_t top = rd32(am, 0x04);
    /* DesignGlyphMetrics {advanceWidth, lsb, rsb, advanceHeight, tsb, bsb, verticalOriginY}:
     * the original zeroes the first 12 of its 14 bytes */
    uint8_t vm[14];
    memset(vm, 0, 12);
    vm[12] = vm[13] = 0;
    uint32_t height, tsb;
    void *face = rdp(impl, 8);
    if (face != NULL &&
        !CFF_ORIG(uint8_t (*)(void *, uint32_t, void *), RVA_VerticalMetrics_TryGetVerticalMetrics)(
            AT(impl, 0x38), rd16(impl, 0x8a), vm)) {
        int16_t asc = 0, desc = 0;
        CFF_ORIG(void (*)(void *, int16_t *, int16_t *), RVA_CalculateTypoAscenderDescender)(face, &asc, &desc);
        tsb = (uint32_t)(int32_t)asc - top;
#ifdef CFF_CONTROL_GLUE_VMTX
        tsb += 1;
#endif
        height = (uint16_t)((uint16_t)desc + (uint16_t)asc);
        if (tsb + 0x8000u > 0xffffu)
            safeint_overflow();
    } else {
        tsb = rd16(vm, 8);
        height = rd16(vm, 6);
    }
    wr32(out, 0x04, (uint16_t)height);
    tsb = (uint32_t)(int32_t)(int16_t)tsb;
    wr32(out, 0x14, tsb);
    if (rd8(impl, 0xe0) != 0) {
        typedef int16_t delta_t(void *, uint32_t);
        int16_t d = CFF_ORIG(delta_t *, RVA_VerticalMetricVariationsTable_GetAdvanceHeightDelta)(rdp(impl, 0x160),
                                                                                                 rd16(impl, 0x8a));
        wr32(out, 0x04, rd32(out, 0x04) + (uint32_t)(int32_t)d);
        d = CFF_ORIG(delta_t *, RVA_VerticalMetricVariationsTable_GetTsbDelta)(rdp(impl, 0x160), rd16(impl, 0x8a));
        wr32(out, 0x14, rd32(out, 0x14) + (uint32_t)(int32_t)d);
        tsb = rd32(out, 0x14);
    }
    wr32(out, 0x08, tsb + top);
    wr32(out, 0x18, rd32(out, 0x04) - rd32(am, 0x24) - tsb);
}
CFF_EXPORT(cff_Impl_ConvertAdobeGlyphMetrics, cff_Impl_ConvertAdobeGlyphMetrics_impl);

/* 0x8bfb0 Implementation::GetMetrics(GlyphMetrics *): Adobe's metrics with the design matrix
 * (+0x9c), then ConvertAdobeGlyphMetrics. A font instance that is not set up (self pointer at
 * +0x5d0, state 1 or 2 at +0x5c8) is error 0xe00104cd. AdobeInternalGetMetrics does not read
 * its OverscaleMode argument (R9 is not set by the original).
 * Control GLUE_GETMETRICS: the device matrix instead of the design matrix. */
CFF_API void cff_Impl_GetMetrics_impl(void *impl, void *out) {
    GLUE_CALL(0x8bfb0);
    _Alignas(16) uint8_t metrics[0x28] = {0};
    uint32_t flags = rd32(impl, 0xb4);
    uint16_t glyph = rd16(impl, 0x8a);
    uint8_t *inst = rdp(impl, 0x80);
    glue_xform x;
#ifdef CFF_CONTROL_GLUE_GETMETRICS
    memcpy(x.b, AT(impl, 0x8c), 16);
#else
    memcpy(x.b, AT(impl, 0x9c), 16);
#endif
    int32_t err = convert_ndv(impl, inst);
    if (err == 0) {
        if (rdp(inst, 0x5d0) == (void *)inst && rd32(inst, 0x5c8) - 1u <= 1u) {
            typedef int32_t adobe_get_metrics_t(void *, uint32_t, void *, uint32_t, uint32_t, void *);
            err = CFF_ORIG(adobe_get_metrics_t *, RVA_AdobeInternalGetMetrics)(inst, glyph, x.b, rd32(impl, 0xb0), flags,
                                                                         metrics);
        } else {
            err = (int32_t)0xe00104cd;
        }
    }
    handle_adobe_error(err);
    CFF_ORIG(void (*)(void *, const void *, void *), RVA_Impl_ConvertAdobeGlyphMetrics)(impl, metrics, out);
}
CFF_EXPORT(cff_Impl_GetMetrics, cff_Impl_GetMetrics_impl);

/* 0x8dad8 Implementation::GetBitmap(GlyphBitmap *): the bitmap with the device matrix, kept
 * in the ScopedArray at +0xc0, returned as {left, top, width, rows, format (+0xb8), rowBytes,
 * bits}. rows * rowBytes must fit the size AdobeInternalGetBitmap returned (FailAssert 0x19f).
 * Control GLUE_BITMAP: the origin moves one pixel right. */
CFF_API void cff_Impl_GetBitmap_impl(void *impl, void *bmp) {
    GLUE_CALL(0x8dad8);
    _Alignas(16) uint8_t metrics[0x28] = {0};
    uint8_t *bits = NULL;
    int32_t size = 0, row_bytes = 0;
    uint32_t width = 0;
    uint32_t flags = rd32(impl, 0xb4), mode = rd32(impl, 0xb0);
    uint16_t glyph = rd16(impl, 0x8a);
    void *inst = rdp(impl, 0x80);
    glue_xform x;
    memcpy(x.b, AT(impl, 0x8c), 16);
    int32_t err = convert_ndv(impl, inst);
    if (err == 0) {
        typedef int32_t get_bitmap_t(void *, void *, uint32_t, void *, uint32_t, uint32_t, void *, int32_t *, uint8_t **,
                                     int32_t *);
        err = CFF_ORIG(get_bitmap_t *, RVA_AdobeInternalGetBitmap)(inst, idwr_orig(RVA_Impl_GetBitmapMemory), glyph, x.b,
                                                                   mode, flags, metrics, &row_bytes, &bits, &size);
        width = (uint32_t)rd64(metrics, 0x20);
    }
    CFF_ORIG(void (*)(void *, void *, uint64_t), RVA_ScopedArray_Attach)(AT(impl, 0xc0), bits,
                                                                         (uint64_t)(int64_t)size);
    handle_adobe_error(err);
    int32_t rows = (int32_t)rd32(metrics, 0x24);
    if (rows < 0)
        safeint_overflow();
    uint64_t total = (uint64_t)(uint32_t)rows * (uint64_t)(int64_t)row_bytes;
    if (total > 0xffffffffull)
        safeint_overflow();
    if ((int64_t)(uint32_t)total > (int64_t)size) {
        fail_assert(0x19f);
        safeint_overflow();
    }
    uint32_t left = rd32(metrics, 0x00);
#ifdef CFF_CONTROL_GLUE_BITMAP
    left += 1;
#endif
    wr32(bmp, 0x00, left);
    wr32(bmp, 0x04, rd32(metrics, 0x04));
    wr32(bmp, 0x08, width);
    wr32(bmp, 0x0c, (uint32_t)rows);
    wr32(bmp, 0x10, rd32(impl, 0xb8));
    wrp(bmp, 0x18, bits);
    wr32(bmp, 0x14, (uint32_t)row_bytes);
}
CFF_EXPORT(cff_Impl_GetBitmap, cff_Impl_GetBitmap_impl);

/* ---------------------------------------------------------------------------------------- */
/* CffRasterizer::Implementation: construction and destruction */

/* 0xb4d78 Implementation::NewFont: AdobeNewFont on the CFF/CFF2 fragment; it returns the
 * font instance (+0x80), the glyph count (+0x88) and unitsPerEm, which is kept only when
 * `head` gave none.
 * Control GLUE_NEWFONT: unitsPerEm doubled. */
CFF_API void cff_Impl_NewFont_impl(void *impl) {
    GLUE_CALL(0xb4d78);
    uint16_t upem = rd16(impl, 0xac);
    uint32_t cff2 = rd8(impl, 0xe0);
    uint64_t size = rd64(impl, 0x18);
    if (size > 0xffffffffull)
        safeint_overflow();
    typedef int32_t new_font_t(void *, uint64_t, uint32_t, void *, void *, uint16_t *);
    int32_t err = CFF_ORIG(new_font_t *, RVA_AdobeNewFont)(rdp(impl, 0x10), size, cff2, AT(impl, 0x80), AT(impl, 0x88),
                                                           &upem);
    handle_adobe_error(err);
    if (rd16(impl, 0xac) == 0) {
        wr16(impl, 0xac, upem);
    } else if (CFF_ORIG(uint8_t (*)(void), RVA_IsInputAssertEnabled)() && rd16(impl, 0xac) != upem) {
        fail_assert(0xee);
        safeint_overflow();
    }
#ifdef CFF_CONTROL_GLUE_NEWFONT
    wr16(impl, 0xac, (uint16_t)(rd16(impl, 0xac) * 2));
#endif
}
CFF_EXPORT(cff_Impl_NewFont, cff_Impl_NewFont_impl);

/* std::vector's three pointers */
typedef struct {
    uint8_t *first, *last, *end;
} glue_vector;

/* array_ref<float const>: {begin, end} */
typedef struct {
    _Alignas(16) uint8_t *begin;
    uint8_t *end;
} glue_span;

static void set_instance_coordinates(void *impl, void *table) {
    glue_span s;
    s.begin = rdp(impl, 0xe8);
    s.end = s.begin + (((int64_t)((uint8_t *)rdp(impl, 0xf0) - s.begin) >> 2) << 2);
    CFF_ORIG(void (*)(void *, glue_span *), RVA_ItemVariationsTable_SetInstanceCoordinates)(table, &s);
}

/* 0xb4984 Implementation::Implementation(FontFaceReference const &): the members, then for an
 * OpenType face (reference + 0x15 == 0) the OpenTypeFace, unitsPerEm from `head`, the `CFF `
 * or (when present) `CFF2` fragment; for CFF2 also hmtx, HVAR and VVAR and the normalized
 * instance coordinates (float at +0xe8, F2Dot14 at +0x100). Kind 7 (a raw CFF file) takes the
 * whole file as the fragment. Then NewFont.
 * Control GLUE_CTOR: the HVAR and VVAR tables get no instance coordinates. */
CFF_API void *cff_Impl_Ctor_impl(void *t, void *ref) {
    GLUE_CALL(0xb4984);
    wrp(t, 0x00, AT(ref, 0x28));
    wrp(t, 0x08, NULL);
    CFF_ORIG(fn_p_pp *, RVA_FontFragmentPtr_Ctor)(AT(t, 0x10), rdp(t, 0x00));
    /* VerticalMetrics at +0x38 */
    wr64(t, 0x38, 0);
    wr32(t, 0x40, 0);
    wr64(t, 0x48, 0);
    wr32(t, 0x50, 0);
    wr64(t, 0x58, 0);
    wr64(t, 0x60, 0);
    wr64(t, 0x70, 0);
    wr64(t, 0x78, 0);
    wr64(t, 0x80, 0);
    wr32(t, 0x88, 0);
    wr16(t, 0xac, 0);
    wr64(t, 0xb0, 0);
    wr32(t, 0xb8, 0);
    wr64(t, 0xc0, 0);
    wr32(t, 0xc8, 0);
    wr64(t, 0xd0, 0);
    wr32(t, 0xd8, 0);
    wr8(t, 0xe0, 0);
    for (int o = 0xe8; o < 0x118; o += 8)
        wr64(t, o, 0);
    /* HorizontalMetrics at +0x118 */
    wr64(t, 0x118, 0);
    wr64(t, 0x120, 0);
    wr16(t, 0x128, 0);
    wr32(t, 0x12c, 0);
    wr64(t, 0x130, 0);
    wr64(t, 0x138, 0);
    wr64(t, 0x148, 0);
    wr64(t, 0x150, 0);
    wr64(t, 0x158, 0);
    wr64(t, 0x160, 0);
    memset(AT(t, 0x8c), 0, 0x20);
    uint8_t kind = rd8(ref, 0x15);
    if (kind == 0) {
        void *f = op_new(0x1d0);
        f = CFF_ORIG(fn_p_pp *, RVA_OpenTypeFace_Ctor)(f, ref);
        void *old = rdp(t, 0x08);
        wrp(t, 0x08, f);
        if (old)
            CFF_ORIG(fn_p_p *, RVA_OpenTypeFace_ScalarDtor)(old);
        uint8_t *face = rdp(t, 0x08);
        uint8_t *head = rdp(face, 0x08);
        if (head == NULL)
            throw_file_format(TAG_newcff, 0x73);
        wr16(t, 0xac, (uint16_t)(head[0x12] << 8 | head[0x13]));
        typedef void init_t(void *, uint64_t, uint64_t);
        if (rd32(face, 0x154) != 0 && rd32(face, 0x158) != 0) {
            wr8(t, 0xe0, 1);
            CFF_ORIG(init_t *, RVA_FontFragmentPtr_Initialize)(AT(t, 0x10), rd32(face, 0x154), rd32(face, 0x158));
            CFF_ORIG(fn_v_pp *, RVA_HorizontalMetrics_Initialize)(AT(t, 0x118), rdp(t, 0x08));
            void *h = op_new(0xa0);
            h = CFF_ORIG(fn_p_pp *, RVA_HorizontalMetricVariationsTable_Ctor)(h, rdp(t, 0x08));
            old = rdp(t, 0x158);
            wrp(t, 0x158, h);
            if (old)
                CFF_ORIG(fn_p_p *, RVA_HvarTable_ScalarDtor)(old);
            void *v = op_new(0xb0);
            v = CFF_ORIG(fn_p_pp *, RVA_VerticalMetricVariationsTable_Ctor)(v, rdp(t, 0x08));
            old = rdp(t, 0x160);
            wrp(t, 0x160, v);
            if (old)
                CFF_ORIG(fn_p_p *, RVA_VvarTable_ScalarDtor)(old);
        } else {
            CFF_ORIG(init_t *, RVA_FontFragmentPtr_Initialize)(AT(t, 0x10), rd32(face, 0x13c), rd32(face, 0x140));
        }
        CFF_ORIG(fn_v_pp *, RVA_VerticalMetrics_Initialize)(AT(t, 0x38), rdp(t, 0x08));
        if (rd8(t, 0xe0) != 0 && rd16(rdp(t, 0x08), 0x1c2) != 0) {
            glue_vector fixed = {0}, values = {0}; /* Fixed16_16, DWRITE_FONT_AXIS_VALUE_FIXED */
            typedef void norm_t(void *, glue_vector *, glue_vector *, void *, void *);
            CFF_ORIG(norm_t *, RVA_ComputeNormalizedAxisCoordinates)(rdp(t, 0x08), &values, &fixed, AT(t, 0x100),
                                                                     AT(t, 0xe8));
#ifndef CFF_CONTROL_GLUE_CTOR
            set_instance_coordinates(t, rdp(t, 0x158));
            set_instance_coordinates(t, rdp(t, 0x160));
#endif
            CFF_ORIG(void (*)(glue_vector *), RVA_vector_ptr_Tidy)(&values);
            if (fixed.first) {
                CFF_ORIG(void (*)(void *, uint64_t), RVA_std_Deallocate16)(
                    fixed.first, (uint64_t)(fixed.end - fixed.first) & ~(uint64_t)3);
                fixed.first = fixed.last = fixed.end = NULL;
            }
        }
    } else if (kind == 7) {
        uint64_t n = CFF_ORIG(uint64_t (*)(void *), RVA_FontFileReference_GetFileSize)(rdp(t, 0x00));
        CFF_ORIG(void (*)(void *, uint64_t, uint64_t), RVA_FontFragmentPtr_Initialize)(AT(t, 0x10), 0, (uint32_t)n);
    }
    if (rd64(t, 0x10) == 0)
        throw_file_format(TAG_newcff, 0x9c);
    CFF_ORIG(fn_v_p *, RVA_Impl_NewFont)(t);
    return t;
}
CFF_EXPORT(cff_Impl_Ctor, cff_Impl_Ctor_impl);

/* 0x8eee0 Implementation::~Implementation */
CFF_API void cff_Impl_Dtor_impl(void *t) {
    GLUE_CALL(0x8eee0);
    if (rdp(t, 0x80))
        CFF_ORIG(fn_v_p *, RVA_AdobeFreeFontInst)(rdp(t, 0x80));
    if (rdp(t, 0x160))
        CFF_ORIG(fn_p_p *, RVA_VvarTable_ScalarDtor)(rdp(t, 0x160));
    if (rdp(t, 0x158))
        CFF_ORIG(fn_p_p *, RVA_HvarTable_ScalarDtor)(rdp(t, 0x158));
    CFF_ORIG(fn_v_p *, RVA_HorizontalMetrics_Dtor)(AT(t, 0x118));
    uint8_t *c = rdp(t, 0x100);
    if (c) {
        uint64_t n = (uint64_t)(((int64_t)((uint8_t *)rdp(t, 0x110) - c) >> 1) * 2);
        CFF_ORIG(void (*)(void *, uint64_t), RVA_std_Deallocate16)(c, n);
        wr64(t, 0x100, 0);
        wr64(t, 0x108, 0);
        wr64(t, 0x110, 0);
    }
    CFF_ORIG(fn_v_p *, RVA_vector_int_Tidy)(AT(t, 0xe8));
    crt_free(rdp(t, 0xd0));
    crt_free(rdp(t, 0xc0));
    CFF_ORIG(fn_v_p *, RVA_VerticalMetrics_Dtor)(AT(t, 0x38));
    CFF_ORIG(fn_v_p *, RVA_FontFragmentPtr_Dtor)(AT(t, 0x10));
    if (rdp(t, 0x08))
        CFF_ORIG(fn_p_p *, RVA_OpenTypeFace_ScalarDtor)(rdp(t, 0x08));
}
CFF_EXPORT(cff_Impl_Dtor, cff_Impl_Dtor_impl);

/* 0x8dd08 Implementation's scalar deleting destructor (always deletes) */
CFF_API void *cff_Impl_ScalarDtor_impl(void *t) {
    GLUE_CALL(0x8dd08);
    CFF_ORIG(fn_v_p *, RVA_Impl_Dtor)(t);
    op_delete(t, 0x168);
    return t;
}
CFF_EXPORT(cff_Impl_ScalarDtor, cff_Impl_ScalarDtor_impl);

/* 0x8efe4 OpenTypeFace's scalar deleting destructor (always deletes) */
CFF_API void *cff_OpenTypeFace_ScalarDtor_impl(void *t) {
    GLUE_CALL(0x8efe4);
    CFF_ORIG(fn_v_p *, RVA_OpenTypeTables_Dtor)(t);
    op_delete(t, 0x1d0);
    return t;
}
CFF_EXPORT(cff_OpenTypeFace_ScalarDtor, cff_OpenTypeFace_ScalarDtor_impl);

/* 0x10c200 / 0x10c230 the HVAR and VVAR tables' scalar deleting destructors */
CFF_API void *cff_HvarTable_ScalarDtor_impl(void *t) {
    GLUE_CALL(0x10c200);
    CFF_ORIG(fn_v_p *, RVA_ItemVariationsTable_Dtor)(t);
    op_delete(t, 0xa0);
    return t;
}
CFF_EXPORT(cff_HvarTable_ScalarDtor, cff_HvarTable_ScalarDtor_impl);

CFF_API void *cff_VvarTable_ScalarDtor_impl(void *t) {
    GLUE_CALL(0x10c230);
    CFF_ORIG(fn_v_p *, RVA_ItemVariationsTable_Dtor)(t);
    op_delete(t, 0xb0);
    return t;
}
CFF_EXPORT(cff_VvarTable_ScalarDtor, cff_VvarTable_ScalarDtor_impl);

/* ---------------------------------------------------------------------------------------- */
/* CffRasterizer: the IFontRasterizer vtable */

/* 0xb48d4 CffRasterizer::CffRasterizer(FontFaceReference const &) */
CFF_API void *cff_CffRasterizer_Ctor_impl(void *t, void *ref) {
    GLUE_CALL(0xb48d4);
    wrp(t, 0x00, idwr_orig(RVA_vft_IFontRasterizer));
    wrp(t, 0x00, idwr_orig(RVA_vft_CffRasterizer));
    wrp(t, 0x08, NULL);
    uint32_t m = mxcsr_enter();
    void *p = op_new(0x168);
    p = CFF_ORIG(fn_p_pp *, RVA_Impl_Ctor)(p, ref);
    void *old = rdp(t, 0x08);
    wrp(t, 0x08, p);
    if (old)
        CFF_ORIG(fn_p_p *, RVA_Impl_ScalarDtor)(old);
    mxcsr_leave(m);
    return t;
}
CFF_EXPORT(cff_CffRasterizer_Ctor, cff_CffRasterizer_Ctor_impl);

/* 0xdf8fc CffRasterizer::~CffRasterizer */
CFF_API void cff_CffRasterizer_Dtor_impl(void *t) {
    GLUE_CALL(0xdf8fc);
    wrp(t, 0x00, idwr_orig(RVA_vft_CffRasterizer));
    if (rdp(t, 0x08))
        CFF_ORIG(fn_p_p *, RVA_Impl_ScalarDtor)(rdp(t, 0x08));
    wrp(t, 0x00, idwr_orig(RVA_vft_IFontRasterizer));
}
CFF_EXPORT(cff_CffRasterizer_Dtor, cff_CffRasterizer_Dtor_impl);

/* 0xdf8c0 CffRasterizer's deleting destructor (vtable slot 0) */
CFF_API void *cff_CffRasterizer_DeletingDtor_impl(void *t, uint32_t flags) {
    GLUE_CALL(0xdf8c0);
    CFF_ORIG(fn_v_p *, RVA_CffRasterizer_Dtor)(t);
    if (flags & 1)
        op_delete(t, 0x10);
    return t;
}
CFF_EXPORT(cff_CffRasterizer_DeletingDtor, cff_CffRasterizer_DeletingDtor_impl);

/* 0x14cc00 IFontRasterizer's deleting destructor */
CFF_API void *cff_IFontRasterizer_ScalarDtor_impl(void *t, uint32_t flags) {
    GLUE_CALL(0x14cc00);
    wrp(t, 0x00, idwr_orig(RVA_vft_IFontRasterizer));
    if (flags & 1)
        op_delete(t, 8);
    return t;
}
CFF_EXPORT(cff_IFontRasterizer_ScalarDtor, cff_IFontRasterizer_ScalarDtor_impl);

/* 0xdc260 CffRasterizer::NewTransform (slot 1).
 * Control GLUE_VT_TRANSFORM: oblique toggled. */
CFF_API void cff_CffRasterizer_NewTransform_impl(void *t, uint32_t a, const void *xform, uint32_t ox, uint32_t oy,
                                                 uint32_t flags) {
    GLUE_CALL(0xdc260);
    uint32_t m = mxcsr_enter();
    typedef void new_transform_t(void *, uint32_t, const void *, uint32_t, uint32_t, uint32_t);
#ifdef CFF_CONTROL_GLUE_VT_TRANSFORM
    flags ^= 4;
#endif
    CFF_ORIG(new_transform_t *, RVA_Impl_NewTransform)(rdp(t, 0x08), a, xform, ox, (uint16_t)oy, flags);
    mxcsr_leave(m);
}
GLUE_EXPORT6(cff_CffRasterizer_NewTransform, cff_CffRasterizer_NewTransform_impl);

/* 0xe22c0 CffRasterizer::NewGlyph(glyph, bool) (slot 2): only remembers the glyph.
 * Control GLUE_GLYPH: glyph + 1. */
CFF_API void cff_CffRasterizer_NewGlyph_impl(void *t, uint32_t glyph, uint32_t unused) {
    GLUE_CALL(0xe22c0);
    (void)unused;
    uint32_t m = mxcsr_enter();
#ifdef CFF_CONTROL_GLUE_GLYPH
    glyph += 1;
#endif
    wr16(rdp(t, 0x08), 0x8a, (uint16_t)glyph);
    mxcsr_leave(m);
}
CFF_EXPORT(cff_CffRasterizer_NewGlyph, cff_CffRasterizer_NewGlyph_impl);

/* 0x8c3d0, 0x8bdc0, 0x8ad00 CffRasterizer::GetBitmap, GetMetrics, GetOutline (slots 3-5).
 * Controls GLUE_VT_BITMAP (top + 1), GLUE_VT_METRICS (rsb + 1), GLUE_VT_OUTLINE (the first
 * move-to y + 0x10000). */
CFF_API void cff_CffRasterizer_GetBitmap_impl(void *t, void *out) {
    GLUE_CALL(0x8c3d0);
    uint32_t m = mxcsr_enter();
    CFF_ORIG(fn_v_pp *, RVA_Impl_GetBitmap)(rdp(t, 0x08), out);
#ifdef CFF_CONTROL_GLUE_VT_BITMAP
    wr32(out, 4, rd32(out, 4) + 1);
#endif
    mxcsr_leave(m);
}
CFF_EXPORT(cff_CffRasterizer_GetBitmap, cff_CffRasterizer_GetBitmap_impl);

CFF_API void cff_CffRasterizer_GetMetrics_impl(void *t, void *out) {
    GLUE_CALL(0x8bdc0);
    uint32_t m = mxcsr_enter();
    CFF_ORIG(fn_v_pp *, RVA_Impl_GetMetrics)(rdp(t, 0x08), out);
#ifdef CFF_CONTROL_GLUE_VT_METRICS
    wr32(out, 0x10, rd32(out, 0x10) + 1);
#endif
    mxcsr_leave(m);
}
CFF_EXPORT(cff_CffRasterizer_GetMetrics, cff_CffRasterizer_GetMetrics_impl);

CFF_API void cff_CffRasterizer_GetOutline_impl(void *t, void *out) {
    GLUE_CALL(0x8ad00);
    uint32_t m = mxcsr_enter();
    CFF_ORIG(fn_v_pp *, RVA_Impl_GetOutline)(rdp(t, 0x08), out);
#ifdef CFF_CONTROL_GLUE_VT_OUTLINE
    uint8_t *buf = rdp(out, 8);
    if (buf && rd32(out, 0) >= 3 && rd32(buf, 0) == 2)
        wr32(buf, 8, rd32(buf, 8) + 0x10000);
#endif
    mxcsr_leave(m);
}
CFF_EXPORT(cff_CffRasterizer_GetOutline, cff_CffRasterizer_GetOutline_impl);

/* 0x14cc60 GetGlyphCount (slot 6), 0x14cc40 GetDesignUnitsPerEm (slot 7).
 * Control GLUE_COUNT: one glyph less. */
CFF_API uint32_t cff_CffRasterizer_GetGlyphCount_impl(void *t) {
    GLUE_CALL(0x14cc60);
#ifdef CFF_CONTROL_GLUE_COUNT
    return (uint16_t)(rd16(rdp(t, 0x08), 0x88) - 1);
#else
    return rd16(rdp(t, 0x08), 0x88);
#endif
}
CFF_EXPORT(cff_CffRasterizer_GetGlyphCount, cff_CffRasterizer_GetGlyphCount_impl);

CFF_API uint32_t cff_CffRasterizer_GetDesignUnitsPerEm_impl(void *t) {
    GLUE_CALL(0x14cc40);
    return rd16(rdp(t, 0x08), 0xac);
}
CFF_EXPORT(cff_CffRasterizer_GetDesignUnitsPerEm, cff_CffRasterizer_GetDesignUnitsPerEm_impl);

/* ---------------------------------------------------------------------------------------- */
/* the CFF metrics builders */

typedef void new_glyph_t(void *, uint32_t, uint32_t);
typedef void get_metrics_t(void *, void *);
typedef uint32_t get_u16_t(void *);

/* 0xde3d0 OpenTypeCffFontMetricsBuilder(FontFaceReference const &, array_ref<float const>):
 * the OpenType builder, the VORG table (+0x420), a CffRasterizer (+0x418), unitsPerEm from
 * `head` (+0x414) as the rasterizer's design transform.
 * Control GLUE_BUILDER: no design transform (SetDesignTransform skipped). */
CFF_API void *cff_OTCffMetricsBuilder_Ctor_impl(void *t, void *ref, const void *axes) {
    GLUE_CALL(0xde3d0);
    glue_span a;
    memcpy(&a, axes, 16);
    CFF_ORIG(void *(*)(void *, void *, glue_span *), RVA_OpenTypeFontMetricsBuilder_Ctor)(t, ref, &a);
    wrp(t, 0x00, idwr_orig(RVA_vft_OpenTypeCffFontMetricsBuilder));
    CFF_ORIG(fn_p_pp *, RVA_VorgTable_Ctor)(AT(t, 0x420), AT(t, 0x68));
    void *r = CFF_ORIG(fn_p_p *, RVA_FontRasterizer_CreateRasterizer)(AT(t, 0x10));
    CFF_ORIG(fn_v_pp *, RVA_ScopedPtr_IFontRasterizer_Attach)(AT(t, 0x418), r);
    uint8_t *head = rdp(t, 0x70);
    if (head == NULL)
        throw_file_format(TAG_cffbldr, 0x1a6);
    uint16_t upem = (uint16_t)(head[0x12] << 8 | head[0x13]);
    wr16(t, 0x414, upem);
#ifndef CFF_CONTROL_GLUE_BUILDER
    CFF_ORIG(void (*)(void *, uint32_t), RVA_OpenTypeFontMetricsBuilder_SetDesignTransform)(rdp(t, 0x418), upem);
#endif
    return t;
}
CFF_EXPORT(cff_OTCffMetricsBuilder_Ctor, cff_OTCffMetricsBuilder_Ctor_impl);

/* 0xdd9dc ~OpenTypeCffFontMetricsBuilder: the VORG table (HorizontalMetrics' destructor, ICF),
 * then the OpenType builder's destructor (a tail call) */
CFF_API void cff_OTCffMetricsBuilder_Dtor_impl(void *t) {
    GLUE_CALL(0xdd9dc);
    CFF_ORIG(fn_v_p *, RVA_HorizontalMetrics_Dtor)(AT(t, 0x420));
    CFF_ORIG(fn_v_p *, RVA_OpenTypeFontMetricsBuilder_Dtor)(t);
}
CFF_EXPORT(cff_OTCffMetricsBuilder_Dtor, cff_OTCffMetricsBuilder_Dtor_impl);

/* 0xdd9a0 its deleting destructor */
CFF_API void *cff_OTCffMetricsBuilder_DeletingDtor_impl(void *t, uint32_t flags) {
    GLUE_CALL(0xdd9a0);
    CFF_ORIG(fn_v_p *, RVA_OTCffMetricsBuilder_Dtor)(t);
    if (flags & 1)
        op_delete(t, 0x460);
    return t;
}
CFF_EXPORT(cff_OTCffMetricsBuilder_DeletingDtor, cff_OTCffMetricsBuilder_DeletingDtor_impl);

/* the state of a GetGlyphMetrics call: `stage` mirrors the original's frame at rsp + 0x28
 * (a DesignGlyphMetrics staging copy, then at +0x10 the 8-byte record the catch block hands
 * to GetGlyphMetricsFromBlackBox) */
typedef struct {
    void *t, *ret;
    uint32_t glyph;
    _Alignas(16) uint8_t stage[0x18];
} glue_ggm;

static void ot_ggm_body(void *arg) {
    glue_ggm *g = arg;
    void *r = rdp(g->t, 0x418);
    VCALL(new_glyph_t *, r, 2)(r, g->glyph, 0);
    _Alignas(16) int32_t m[8] = {0};
    r = rdp(g->t, 0x418);
    VCALL(get_metrics_t *, r, 4)(r, m);
#ifdef CFF_CONTROL_GLUE_DESIGN
    m[0] += 1;
#endif
    /* {advanceWidth, lsb, rsb, advanceHeight, tsb, bsb, verticalOriginY} */
    static const int8_t field[7] = {0, 3, 4, 1, 5, 6, 2};
    for (int i = 0; i < 7; i++)
        wr16(g->stage, 2 * i, (uint16_t)m[field[i]]);
    for (int i = 0; i < 7; i++)
        wr16(g->ret, 2 * i, (uint16_t)m[field[i]]);
}

/* 0xe2340 OpenTypeCffFontMetricsBuilder::GetGlyphMetrics(glyph, format) -> DesignGlyphMetrics
 * (returned through the hidden pointer): the rasterizer's NewGlyph and GetMetrics, truncated
 * to 16 bits. catch (FileFormatException const &) (funclet 0x19157b): the metrics from the
 * black box (OpenTypeFontMetricsBuilder::GetGlyphMetricsFromBlackBox), with the vertical
 * origin from VORG minus the vmtx tsb when the face has both.
 * Control GLUE_DESIGN: advanceWidth + 1. */
CFF_API void *cff_OTCffMetricsBuilder_GetGlyphMetrics_impl(void *t, void *ret, uint32_t glyph, uint32_t format) {
    GLUE_CALL(0xe2340);
    (void)format;
    glue_ggm g;
    memset(&g, 0, sizeof g);
    g.t = t;
    g.ret = ret;
    g.glyph = (uint16_t)glyph;
    if (glue_try_ffe(ot_ggm_body, &g)) {
        wr64(g.stage, 0x10, 0);
        uint8_t *tt = t;
        if (CFF_ORIG(uint8_t (*)(void *, uint32_t, void *), RVA_VorgTable_TryGetValue)(tt + 0x420, g.glyph, g.stage + 0xc) &&
            CFF_ORIG(uint8_t (*)(void *, uint32_t, void *), RVA_VerticalMetrics_TryGetVerticalMetrics)(tt + 0x318, g.glyph,
                                                                                                      g.stage)) {
            uint32_t c = (uint32_t)(int32_t)(int16_t)rd16(g.stage, 0xc) - (uint32_t)(int32_t)(int16_t)rd16(g.stage, 8);
            if (c + 0x8000u > 0xffffu)
                CFF_ORIG(void (*)(void), RVA_FailAsExceptionPolicy_FileFormat_SafeIntOnOverflow)();
            wr16(g.stage, 0x16, (uint16_t)c);
        }
        CFF_ORIG(void (*)(void *, void *, uint32_t, void *), RVA_OpenTypeFontMetricsBuilder_GetGlyphMetricsFromBlackBox)(
            t, ret, g.glyph, g.stage + 0x10);
    }
    return ret;
}
CFF_EXPORT(cff_OTCffMetricsBuilder_GetGlyphMetrics, cff_OTCffMetricsBuilder_GetGlyphMetrics_impl);

/* 0x134978 RawCffFontMetricsBuilder(FontFaceReference const &) */
CFF_API void *cff_RawCffMetricsBuilder_Ctor_impl(void *t, void *ref) {
    GLUE_CALL(0x134978);
    wr32(t, 0x08, 0);
    wrp(t, 0x00, idwr_orig(RVA_vft_IFontMetricsBuilder));
    wrp(t, 0x00, idwr_orig(RVA_vft_RawCffFontMetricsBuilder));
    CFF_ORIG(fn_p_pp *, RVA_FontFaceReference_CopyCtor)(AT(t, 0x10), ref);
    wrp(t, 0x68, CFF_ORIG(fn_p_p *, RVA_FontRasterizer_CreateRasterizer)(AT(t, 0x10)));
    void *r = rdp(t, 0x68);
    uint16_t upem = (uint16_t)VCALL(get_u16_t *, r, 7)(r);
    wr16(t, 0x70, upem);
    CFF_ORIG(void (*)(void *, uint32_t), RVA_OpenTypeFontMetricsBuilder_SetDesignTransform)(rdp(t, 0x68), upem);
    return t;
}
CFF_EXPORT(cff_RawCffMetricsBuilder_Ctor, cff_RawCffMetricsBuilder_Ctor_impl);

/* 0x1349f0 ~RawCffFontMetricsBuilder */
CFF_API void cff_RawCffMetricsBuilder_Dtor_impl(void *t) {
    GLUE_CALL(0x1349f0);
    CFF_ORIG(fn_v_p *, RVA_ScopedPtr_Dtor)(AT(t, 0x68));
    CFF_ORIG(fn_v_p *, RVA_FontFileReference_Dtor)(AT(t, 0x38));
    wrp(t, 0x00, idwr_orig(RVA_vft_IFontMetricsBuilder));
}
CFF_EXPORT(cff_RawCffMetricsBuilder_Dtor, cff_RawCffMetricsBuilder_Dtor_impl);

/* 0x134b10 its deleting destructor */
CFF_API void *cff_RawCffMetricsBuilder_DeletingDtor_impl(void *t, uint32_t flags) {
    GLUE_CALL(0x134b10);
    CFF_ORIG(fn_v_p *, RVA_RawCffMetricsBuilder_Dtor)(t);
    if (flags & 1)
        op_delete(t, 0x78);
    return t;
}
CFF_EXPORT(cff_RawCffMetricsBuilder_DeletingDtor, cff_RawCffMetricsBuilder_DeletingDtor_impl);

/* 0x134f60 RawCffFontMetricsBuilder::GetDesignUnitsPerEm. 12 bytes before
 * FontFace::GetGlyphLocality at 0x134f6c: too short for the 14-byte hook, not swapped. */
CFF_API uint32_t cff_RawCffMetricsBuilder_GetDesignUnitsPerEm_impl(void *t) {
    GLUE_CALL(0x134f60);
    return rd16(t, 0x70);
}
CFF_EXPORT(cff_RawCffMetricsBuilder_GetDesignUnitsPerEm, cff_RawCffMetricsBuilder_GetDesignUnitsPerEm_impl);

static void raw_ggm_body(void *arg) {
    typedef uint16_t clamp_t(int64_t);
    glue_ggm *g = arg;
    void *r = rdp(g->t, 0x68);
    VCALL(new_glyph_t *, r, 2)(r, g->glyph, 0);
    _Alignas(16) int32_t m[8] = {0};
    r = rdp(g->t, 0x68);
    VCALL(get_metrics_t *, r, 4)(r, m);
    clamp_t *clamp = CFF_ORIG(clamp_t *, RVA_ClampSigned_short);
    uint64_t aw = (uint32_t)m[0], ah = (uint32_t)m[1];
    void *ret = g->ret;
    wr16(ret, 0x0, (uint16_t)(aw > 0xffff ? 0xffff : aw));
    wr16(ret, 0x2, clamp(m[3]));
    wr16(ret, 0x4, clamp(m[4]));
    wr16(ret, 0x6, (uint16_t)(ah > 0xffff ? 0xffff : ah));
    wr16(ret, 0x8, clamp(m[5]));
    wr16(ret, 0xa, clamp(m[6]));
    wr16(ret, 0xc, clamp(m[2]));
}

/* 0x135280 RawCffFontMetricsBuilder::GetGlyphMetrics(glyph, format) -> DesignGlyphMetrics:
 * as the OpenType one, but the advances saturate to 0xffff and the rest is ClampSigned<short>;
 * its catch (FileFormatException const &) (funclet 0x194bef) is empty: the metrics stay 0. */
CFF_API void *cff_RawCffMetricsBuilder_GetGlyphMetrics_impl(void *t, void *ret, uint32_t glyph, uint32_t format) {
    GLUE_CALL(0x135280);
    (void)format;
    wr64(ret, 0, 0);
    wr32(ret, 8, 0);
    wr16(ret, 0xc, 0);
    glue_ggm g;
    memset(&g, 0, sizeof g);
    g.t = t;
    g.ret = ret;
    g.glyph = (uint16_t)glyph;
    glue_try_ffe(raw_ggm_body, &g);
    return ret;
}
CFF_EXPORT(cff_RawCffMetricsBuilder_GetGlyphMetrics, cff_RawCffMetricsBuilder_GetGlyphMetrics_impl);

/* 0x135520 RawCffFontFaceBuilder::ReadFontFace: glyph count and unitsPerEm from a temporary
 * CffRasterizer, fixed weight/stretch/style fields */
CFF_API void cff_RawCffFaceBuilder_ReadFontFace_impl(void *t) {
    GLUE_CALL(0x135520);
    void *r = CFF_ORIG(fn_p_p *, RVA_FontRasterizer_CreateRasterizer)(rdp(t, 0x08));
    void *holder = r; /* ScopedPtr<IFontRasterizer> */
    wr16(t, 0x72, (uint16_t)VCALL(get_u16_t *, r, 6)(r));
    wr16(t, 0x28, (uint16_t)VCALL(get_u16_t *, r, 7)(r));
    wr32(t, 0x68, 0x01900190);
    wr8(t, 0x6c, 5);
    wr16(t, 0x70, rd16(t, 0x70) | 0x20);
    CFF_ORIG(void (*)(void **), RVA_ScopedPtr_Dtor)(&holder);
}
CFF_EXPORT(cff_RawCffFaceBuilder_ReadFontFace, cff_RawCffFaceBuilder_ReadFontFace_impl);

/* 0xf1c80 RawCffFontFaceBuilder's and 0x134a50 FontFaceRegionBuilder's deleting destructors
 * (two identical copies) */
CFF_API void *cff_RawCffFaceBuilder_DeletingDtor_impl(void *t, uint32_t flags) {
    GLUE_CALL(0xf1c80);
    CFF_ORIG(fn_v_p *, RVA_FontFaceRegionBuilder_Dtor)(t);
    if (flags & 1)
        op_delete(t, 0x3b0);
    return t;
}
CFF_EXPORT(cff_RawCffFaceBuilder_DeletingDtor, cff_RawCffFaceBuilder_DeletingDtor_impl);

CFF_API void *cff_FontFaceRegionBuilder_ScalarDtor_impl(void *t, uint32_t flags) {
    GLUE_CALL(0x134a50);
    CFF_ORIG(fn_v_p *, RVA_FontFaceRegionBuilder_Dtor)(t);
    if (flags & 1)
        op_delete(t, 0x3b0);
    return t;
}
CFF_EXPORT(cff_FontFaceRegionBuilder_ScalarDtor, cff_FontFaceRegionBuilder_ScalarDtor_impl);

/* 0x134a90 IFontMetricsBuilder's deleting destructor */
CFF_API void *cff_IFontMetricsBuilder_ScalarDtor_impl(void *t, uint32_t flags) {
    GLUE_CALL(0x134a90);
    wrp(t, 0x00, idwr_orig(RVA_vft_IFontMetricsBuilder));
    if (flags & 1)
        op_delete(t, 0x10);
    return t;
}
CFF_EXPORT(cff_IFontMetricsBuilder_ScalarDtor, cff_IFontMetricsBuilder_ScalarDtor_impl);

/* 0x134ad0 OpenTypeFontMetricsBuilder's deleting destructor */
CFF_API void *cff_OTMetricsBuilder_DeletingDtor_impl(void *t, uint32_t flags) {
    GLUE_CALL(0x134ad0);
    CFF_ORIG(fn_v_p *, RVA_OpenTypeFontMetricsBuilder_Dtor)(t);
    if (flags & 1)
        op_delete(t, 0x420);
    return t;
}
CFF_EXPORT(cff_OTMetricsBuilder_DeletingDtor, cff_OTMetricsBuilder_DeletingDtor_impl);
