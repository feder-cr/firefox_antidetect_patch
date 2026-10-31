/*
 * Portability shims for the glyph run layer (glyphrun_*.c) outside x86-64 Windows.
 *
 * The glyph run ports keep the original's SSE sequences (cvtss2si, cvttss2si, MXCSR, sfence)
 * and, in the parts that work on DWrite's own cache objects (GetGlyphData, AddGlyph, the
 * lookup cache), Windows synchronisation and VirtualAlloc. The standalone texture path
 * (reimpl/idwr/texture.c) uses only the rasterization state, the GlyphBitmapLayout writer, the
 * merges and filters; the cache parts are never reached there, but they must still compile.
 *
 *   - x86-64 (any OS): the real SSE intrinsics.
 *   - elsewhere: C versions with the same results on every input the texture path gives them
 *     (cvtss2si rounds to nearest even and gives INT_MIN out of range or for NaN; MXCSR reads
 *     as its default 0x1f80 and writes are ignored).
 *   - not Windows: the few Windows names the cache parts use, as inert stand-ins.
 */
#ifndef TT_PORTABLE_H
#define TT_PORTABLE_H
#include <stdint.h>
#include "fp_exact.h"  /* no FMA contraction */

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#include <emmintrin.h>
#else
#include <math.h>
typedef struct { float f; } __m128;
static inline __m128 _mm_set_ss(float v) { __m128 r = { v }; return r; }
static inline int32_t _mm_cvtss_si32(__m128 v) {
    if (!(v.f >= -2147483648.0f && v.f < 2147483648.0f)) return INT32_MIN;
    return (int32_t)nearbyintf(v.f);
}
static inline int64_t _mm_cvttss_si64(__m128 v) {
    if (!(v.f >= -9223372036854775808.0f && v.f < 9223372036854775808.0f)) return INT64_MIN;
    return (int64_t)v.f;
}
static inline uint32_t _mm_getcsr(void) { return 0x1f80; }
static inline void _mm_setcsr(uint32_t v) { (void)v; }
static inline void _mm_sfence(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
#endif

#ifdef TT_NEED_WINDOWS   /* defined by the file that includes this one for its cache code */
#ifdef _WIN32
#include <windows.h>
#else
/* only the DWrite cache code names these; it is never reached in standalone mode */
typedef int32_t LONG;
typedef struct { void *opaque[5]; } CRITICAL_SECTION;
static inline void EnterCriticalSection(CRITICAL_SECTION *cs) { (void)cs; }
static inline void LeaveCriticalSection(CRITICAL_SECTION *cs) { (void)cs; }
static inline LONG InterlockedIncrement(volatile LONG *p) { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
static inline LONG InterlockedDecrement(volatile LONG *p) { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
typedef struct { uint32_t dwPageSize; } SYSTEM_INFO;
static inline void GetSystemInfo(SYSTEM_INFO *si) { si->dwPageSize = 4096; }
#define MEM_COMMIT 0x1000
#define PAGE_READWRITE 0x04
static inline void *VirtualAlloc(void *p, uint64_t n, uint32_t type, uint32_t protect) {
    (void)n; (void)type; (void)protect;
    return p;
}
#endif
#endif

#endif
