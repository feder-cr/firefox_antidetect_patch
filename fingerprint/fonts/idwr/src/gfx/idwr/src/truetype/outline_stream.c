/*
 * StreamGlyphOutlines<OutlineTransformWithOffset<1>> 0x180151518 (DWrite.dll 10.0.26100.9711),
 * ported with the original's signature for the function swap: the sideways half of
 * IDWriteFontFace::GetGlyphRunOutline's streaming. idwr's own model of it is stream_sideways in
 * reimpl/idwr/outline.c; this port lets reimpl/outline_hinted_check.py (--swap, with
 * reimpl/outline_swaps.txt) run it inside DWrite.dll in place of the original.
 *
 *   void StreamGlyphOutlines<OutlineTransformWithOffset<1>>(float emSize, uint16 unitsPerEm,
 *       GlyphOutlineLayout const *const *layouts, PointF const *origins, uint32 count,
 *       ID2D1SimplifiedGeometrySink *sink)
 *
 * A GlyphOutlineLayout is { uint32 n; uint32 offset; } with n int32 command words at
 * layout + offset: 2 move (x, y), 3 line (x, y), 4 cubic (3 points), 5 close, 6 quadratic
 * (control, end), in 16.16 font units. Each point (x, y) goes to
 * (ox + -(y * s), oy + -(x * s)) with s = emSize * 2^-16 / unitsPerEm; a quadratic becomes a
 * cubic with the constants 0x3f2aaaaa and 0x3eaaaaab. The sink calls go through the original
 * helpers (GeometrySetFillModeToWinding, GeometryBeginFigure, GeometryLineTo, GeometryBezierTo,
 * GeometryEndFigure) and malformed layouts throw InvalidCacheDataException like the original.
 *
 * Oracle builds on Windows only: standalone and portable builds have no DWrite sink to drive,
 * so the file is empty there.
 */
#if defined(_WIN32) && !defined(IDWR_STANDALONE) && defined(__x86_64__)
#include <stdint.h>
#include "fatal.h"
#include <string.h>
#include <stdlib.h>
#include "orig.h"

#define RVA_GeometrySetFillModeToWinding 0x8fdc4
#define RVA_GeometryBeginFigure          0x8fd08
#define RVA_GeometryLineTo               0xa64ec
#define RVA_GeometryBezierTo             0x8f0bc
#define RVA_GeometryEndFigure            0x8fd74
#define RVA_Exception_ctor               0xaa7f0   /* Exception::Exception(int hr, uint) */
#define RVA_LogAndThrow_InvalidCache     0x10bf90  /* LogAndThrow<InvalidCacheDataException> */
#define TAG_OUTLN                        0x6e6c74756full /* "outln" */
#define HR_INVALID_CACHE                 0x88985007u

typedef void (*sink_fn)(void *);
typedef void (*point_fn)(void *, uint64_t);
typedef void (*bezier_fn)(void *, uint64_t, uint64_t, uint64_t);

static uint64_t pack(float x, float y) {
    uint32_t a, b;
    memcpy(&a, &x, 4), memcpy(&b, &y, 4);
    return (uint64_t)b << 32 | a;
}

__attribute__((noreturn)) static void invalid(uint32_t line) {
    __attribute__((aligned(16))) uint8_t e[32];
    memset(e, 0, sizeof e);
    ORIG(void (*)(void *, uint32_t, uint32_t), RVA_Exception_ctor)(e, HR_INVALID_CACHE, 0);
    ORIG(void (*)(void *, uint64_t, uint32_t), RVA_LogAndThrow_InvalidCache)(e, TAG_OUTLN, line);
    IDWR_FATAL("outline_stream.c:55");
}

/* negative control of the swap test: 1 makes the 1/3 constant one ulp larger */
static int g_perturb;
__declspec(dllexport) void tt_outline_stream_perturb(int bits) { g_perturb = bits; }

void os_StreamGlyphOutlines_WithOffset1(float em, uint16_t upem, const uint8_t *const *layouts, const float *org,
                                        uint32_t count, void *sink) {
    float s = (em * 1.52587891e-05f) / (float)upem;
    const float two = 0.666666627f, one = g_perturb & 1 ? 0.333333373f : 0.333333343f;
    ORIG(sink_fn, RVA_GeometrySetFillModeToWinding)(sink);
    for (uint32_t k = 0; k < count; k++) {
        float ox = org[2 * k], oy = org[2 * k + 1];
        const uint8_t *g = layouts[k];
        uint32_t n = *(const uint32_t *)g;
        const int32_t *v = (const int32_t *)(g + *(const uint32_t *)(g + 4));
        int open = 0;
        float cx = 0.0f, cy = 0.0f;
#define PX(i) (ox + -((float)v[(i) + 1] * s))
#define PY(i) (oy + -((float)v[(i)] * s))
        for (uint32_t i = 0; i < n;) {
            int32_t op = v[i++];
            if (op == 2) {
                if (open) ORIG(sink_fn, RVA_GeometryEndFigure)(sink);
                if (n - i < 2) invalid(0x8a);
                cx = PX(i), cy = PY(i);
                ORIG(point_fn, RVA_GeometryBeginFigure)(sink, pack(cx, cy));
                open = 1, i += 2;
            } else if (op == 3) {
                if (!open || n - i < 2) invalid(0x9e);
                cx = PX(i), cy = PY(i);
                ORIG(point_fn, RVA_GeometryLineTo)(sink, pack(cx, cy));
                i += 2;
            } else if (op == 4) {
                if (!open || n - i < 6) invalid(0xa8);
                float x1 = PX(i), y1 = PY(i), x2 = PX(i + 2), y2 = PY(i + 2);
                cx = PX(i + 4), cy = PY(i + 4);
                ORIG(bezier_fn, RVA_GeometryBezierTo)(sink, pack(x1, y1), pack(x2, y2), pack(cx, cy));
                i += 6;
            } else if (op == 5) {
                if (!open) invalid(0x96);
                ORIG(sink_fn, RVA_GeometryEndFigure)(sink);
                open = 0;
            } else if (op == 6) {
                if (!open || n - i < 4) invalid(0xb8);
                float qx = PX(i) * two, qy = PY(i) * two;
                float ex = PX(i + 2), ey = PY(i + 2);
                ORIG(bezier_fn, RVA_GeometryBezierTo)(sink, pack(cx * one + qx, cy * one + qy),
                                                      pack(ex * one + qx, ey * one + qy), pack(ex, ey));
                cx = ex, cy = ey, i += 4;
            } else {
                invalid(0xd3);
            }
        }
#undef PX
#undef PY
        if (open) ORIG(sink_fn, RVA_GeometryEndFigure)(sink);
    }
}

/* the export: saves RAX, RCX, RDX, R8-R11 and XMM0-XMM5 around the port and re-pushes 16 stack
 * arguments, as glyphrun_helpers.c's thunks do (DWrite's callers may keep values in volatile
 * registers across a call they know); SEH unwind data lets the original's exceptions through */
__asm__(".text\n.globl tt_StreamGlyphOutlines_WithOffset1\n"
        ".def tt_StreamGlyphOutlines_WithOffset1; .scl 2; .type 32; .endef\n"
        ".p2align 4\ntt_StreamGlyphOutlines_WithOffset1:\n.seh_proc tt_StreamGlyphOutlines_WithOffset1\n"
        "subq $0x148, %rsp\n.seh_stackalloc 0x148\n.seh_endprologue\n"
        "movq %rcx, 0xa0(%rsp)\nmovq %rdx, 0xa8(%rsp)\nmovq %r8, 0xb0(%rsp)\n"
        "movq %r9, 0xb8(%rsp)\nmovq %r10, 0xc0(%rsp)\nmovq %r11, 0xc8(%rsp)\n"
        "movq %rax, 0xd0(%rsp)\n"
        "movaps %xmm0, 0xe0(%rsp)\nmovaps %xmm1, 0xf0(%rsp)\nmovaps %xmm2, 0x100(%rsp)\n"
        "movaps %xmm3, 0x110(%rsp)\nmovaps %xmm4, 0x120(%rsp)\nmovaps %xmm5, 0x130(%rsp)\n"
        "movq 0x170(%rsp), %rax\nmovq %rax, 0x20(%rsp)\nmovq 0x178(%rsp), %rax\nmovq %rax, 0x28(%rsp)\n"
        "movq 0x180(%rsp), %rax\nmovq %rax, 0x30(%rsp)\nmovq 0x188(%rsp), %rax\nmovq %rax, 0x38(%rsp)\n"
        "movq 0x190(%rsp), %rax\nmovq %rax, 0x40(%rsp)\nmovq 0x198(%rsp), %rax\nmovq %rax, 0x48(%rsp)\n"
        "movq 0x1a0(%rsp), %rax\nmovq %rax, 0x50(%rsp)\nmovq 0x1a8(%rsp), %rax\nmovq %rax, 0x58(%rsp)\n"
        "movq 0x1b0(%rsp), %rax\nmovq %rax, 0x60(%rsp)\nmovq 0x1b8(%rsp), %rax\nmovq %rax, 0x68(%rsp)\n"
        "movq 0x1c0(%rsp), %rax\nmovq %rax, 0x70(%rsp)\nmovq 0x1c8(%rsp), %rax\nmovq %rax, 0x78(%rsp)\n"
        "movq 0x1d0(%rsp), %rax\nmovq %rax, 0x80(%rsp)\nmovq 0x1d8(%rsp), %rax\nmovq %rax, 0x88(%rsp)\n"
        "movq 0x1e0(%rsp), %rax\nmovq %rax, 0x90(%rsp)\nmovq 0x1e8(%rsp), %rax\nmovq %rax, 0x98(%rsp)\n"
        "movq 0xd0(%rsp), %rax\n"
        "callq os_StreamGlyphOutlines_WithOffset1\n"
        "movq 0xa0(%rsp), %rcx\nmovq 0xa8(%rsp), %rdx\nmovq 0xb0(%rsp), %r8\n"
        "movq 0xb8(%rsp), %r9\nmovq 0xc0(%rsp), %r10\nmovq 0xc8(%rsp), %r11\n"
        "movaps 0xf0(%rsp), %xmm1\nmovaps 0x100(%rsp), %xmm2\nmovaps 0x110(%rsp), %xmm3\n"
        "movaps 0x120(%rsp), %xmm4\nmovaps 0x130(%rsp), %xmm5\n"
        "movaps 0xe0(%rsp), %xmm0\nmovq 0xd0(%rsp), %rax\n"
        "addq $0x148, %rsp\nretq\n.seh_endproc\n"
        ".section .drectve,\"yn\"\n.ascii \" -export:tt_StreamGlyphOutlines_WithOffset1\"\n.text\n");
#else
typedef int idwr_outline_stream_empty; /* ISO C: a translation unit is not empty */
#endif
