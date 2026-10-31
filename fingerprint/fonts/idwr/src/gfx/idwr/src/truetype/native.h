/*
 * Native helpers (native.c) and the error path of the standalone scaler.
 *
 * The original's SafeInt handlers throw a C++ exception that TrueTypeRasterizer catches. In
 * C the driver arms a catch frame and gets the error back as a return code:
 *
 *     idwr_catch c;
 *     idwr_catch_push(&c);
 *     if (IDWR_SETJMP(c.buf)) { idwr_catch_pop(&c); return c.code; }
 *     int err = ...scaler calls...;
 *     idwr_catch_pop(&c);
 *
 * The jump uses the C library's setjmp/longjmp everywhere. On Windows its longjmp unwinds
 * with RtlUnwindEx, which needs unwind data for every frame in between: in standalone mode
 * every frame is a C function (the register-preserving export thunks are compiled out), so it
 * has. The compiler's __builtin_setjmp/__builtin_longjmp, used here before, crash on x86-64
 * Windows with zig 0.14.1 (clang 19) as soon as a jump is taken (reimpl/texture_check.py's
 * first SafeInt-free error path found it: an access violation in the first longjmp).
 */
#ifndef TT_NATIVE_H
#define TT_NATIVE_H
#include <stdint.h>

#include <setjmp.h>
typedef jmp_buf idwr_jmp;
#define IDWR_SETJMP(b) setjmp(b)
#define IDWR_LONGJMP(b) longjmp(b, 1)

enum {
    IDWR_ERR_SAFEINT_OVERFLOW = 0x7d000001, /* SafeIntOnOverflow */
    IDWR_ERR_SAFEINT_DIVZERO = 0x7d000002,  /* SafeIntOnDivZero */
    IDWR_ERR_FAIL_ASSERT = 0x7d000003,      /* FailAssert: an internal check of the original */
    IDWR_ERR_UNMODELLED = 0x7d000004,       /* a path the standalone driver does not model */
};

#ifndef IDWR_HR_E_OUTOFMEMORY /* also in reimpl/idwr/idwr.h */
#define IDWR_HR_E_OUTOFMEMORY ((int32_t)0x8007000e)
#endif

typedef struct idwr_catch {
    idwr_jmp buf;
    volatile int code;
    struct idwr_catch *prev;
} idwr_catch;

void idwr_catch_push(idwr_catch *c);
void idwr_catch_pop(idwr_catch *c);
/* jumps to the innermost armed frame with `code`; aborts when none is armed */
_Noreturn void idwr_raise(int code);

typedef struct {
    uint32_t rva;
    const char *name;
    void *fn;
} idwr_native_entry;
extern const idwr_native_entry idwr_native_table[]; /* sorted by RVA, {0} terminated */

/* The FontFace stand-in the glyph run ports read in standalone mode (reimpl/idwr/texture.c),
 * at the offsets of DWrite's FontFace that GlyphBitmapRasterizationState's constructor and
 * the natives GetFontWeight / CreateRasterizer use:
 *   face + 0x00  pointer to the face data: + 0x58 u16 weight, + 0x60 u8 flags (bit 2: thin
 *                family, the 1-bit spread), + 0x62 u16 glyph count
 *   face + 0x36  u8, bit 0: bold simulation (GetFontWeight gives 700)
 *   face + 0x98  {void *(*create)(void *ctx), ctx}: CreateRasterizer returns create(ctx), an
 *                object whose first field is the IFontRasterizer vtable
 *                {destroy(self, flags), NewTransform, NewGlyph, GetBitmap} */

#endif
