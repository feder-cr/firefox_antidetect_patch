/*
 * Scan converter setup, ported from DWrite.dll 10.0.26100.9711: fsc_SetupScan (0x18006fb90),
 * fsc_AllocVMem (0x180071720), FixPointers (0x18010b2fc). fsc_SetupScan is called by
 * fsc_FillGlyph (0x18006e980) and lays out the per-scan-line crossing lists in the work memory.
 *
 * StateVars (sv), 0x138 bytes, at WorkScan + 0x20 (fsc_FillGlyph). Offsets as far as known:
 *   +0x00 int32   previous point x (fsc_FillGlyph sets 0x7fffffff at a contour start)
 *   +0x04 int32   previous point y
 *   +0x08 int32   current point x          +0x0c int32  current point y
 *   +0x10 int32   first point x of the contour (topology checks)   +0x14 int32  ... y
 *   +0x18 int32   rect left   (sign-extended Rect.left,   rect + 2)
 *   +0x1c int32   rect right  (Rect.right,  rect + 6)
 *   +0x20 int32   rect top    (Rect.top,    rect + 0): rows are bottom <= y < top
 *   +0x24 int32   rect bottom (Rect.bottom, rect + 4)
 *   +0x30..+0x50  five row tables (int16 *[top - bottom], index y - bottom), set here:
 *                 +0x30 = +0x40 = start of the row's crossings ("on" half),
 *                 +0x38 = +0x48 = start of the second half ("off" half), +0x50 = end of the row.
 *                 Each row gets 2 * count int16 slots, count = sum of the reversal counts up to
 *                 that row, each shifted left by +0x114. +0x30/+0x38 stay the begin pointers,
 *                 +0x40/+0x48 are presumably the moving fill pointers (same initial values).
 *   +0x70..+0x90  the same five tables for columns (int16 *[right - left], index x - left),
 *                 only built when dropout control is on (flags & 2 == 0)
 *   +0xb0 fnptr   horizontal crossing adder: AddHorizSmartScan 0x1800a5570 / AddHorizSimpleScan 0x1800965e0
 *   +0xb8 fnptr   vertical crossing adder:   AddVertSmartScan 0x1800a53d0 / AddVertSimpleScan 0x180095d90
 *   +0xc0..+0xe8  CalcHoriz/VertLine/Sp/Ep Subpix function pointers (set by fsc_FillGlyph)
 *   +0xf0 uint8   fsc_FillGlyph's bool argument (stored here, meaning not established)
 *   +0xf8 int32 * contour start x per contour (smart dropout only, element count +0x108)
 *   +0x100 int32 * contour start y
 *   +0x108 int32  capacity of +0xf8/+0x100 (element count argument)
 *   +0x10c int32  entries used in +0xf8/+0x100 (reset to 0 here)
 *   +0x112 int16  scan kind: 2 smart dropout, 1 otherwise
 *   +0x114 int16  shift applied to the reversal counts: 1 smart (two int16 per crossing), 0 otherwise
 *   +0x118 uint8 * pool 1 cursor (rows: tables and row crossing memory); starts at sv + 0x138
 *   +0x120 uint8 * pool 2 cursor (columns and contour start arrays); WorkScan + 0x28
 *   +0x128 uint8 * pool 1 end (WorkScan + 4 = pool 1 size + 0x138)
 *   +0x130 uint8 * pool 2 end (WorkScan + 8 = pool 2 size)
 * An allocation moves the cursor by the size rounded up to 8 and fails (NULL / 0x1a01) when the
 * cursor passes the end; the cursor is left moved.
 *
 * RevRoots (reversal list roots): +0x00 row list head, +0x08 column list head, +0x10, +0x18 two
 * more pointers, +0x20 the address of the RevRoots block when the pointers were written.
 * Reversal node: +0 int16 scan line, +2 int16 count, +8 next; a list ends at a node with
 * scan line >= 0x7fff (sentinel).
 *
 * fsc_SetupScan flags (fsc_FillGlyph's scan kind): bit 1 (2) no dropout control, bit 2 (4)
 * smart dropout. SafeInt checks are reproduced; none can fire with 16-bit rects and the
 * argument limits checked here (the original throws via SafeIntOnOverflow, called as well).
 */
#include "tt_state.h"
#include "orig.h"

#define ERR_SCAN_MEMORY 0x1a01

/* the original throws (SafeIntExceptionHandler<SafeIntRasterizerException>::SafeIntOnOverflow) */
static void overflow(void) { ORIG(void (*)(void), 0x1090b0)(); }

/* SafeInt<int>(n) * 8, n a 64-bit difference that must fit in int32 */
static int32_t times8(int64_t d64) {
    if ((uint64_t)d64 + 0x80000000u > 0xffffffffu) overflow();
    int32_t d = (int32_t)d64;
    uint32_t a = d < 0 ? 0u - (uint32_t)d : (uint32_t)d;
    uint64_t m = (uint64_t)a << 3;
    if (d < 0) {
        if (m > 0x80000000u) overflow();
        return (int32_t)(0u - (uint32_t)m);
    }
    if (m > 0x7fffffffu) overflow();
    return (int32_t)m;
}

/* (SafeInt<int>(n) + 7) & ~7, sign-extended to 64 bits */
static int64_t round8(int32_t n) {
    int64_t t = (int64_t)n + 7;
    if ((uint64_t)t + 0x80000000u > 0xffffffffu) overflow();
    return (int64_t)(int32_t)t & ~(int64_t)7;
}

/* SafeInt pointer + int64: a negative delta moves the cursor back (only with a negative size) */
static uint64_t ptr_add(uint64_t cur, int64_t delta) {
    if (delta >= 0) {
        uint64_t n = cur + (uint64_t)delta;
        if (n < cur) overflow();
        return n;
    }
    uint64_t m = 0 - (uint64_t)delta;
    if (m > cur) overflow();
    return cur - m;
}

/* fsc_AllocVMem: size bytes (rounded up to 8) from pool 2; NULL when it runs past the end */
TT_EXPORT void *tt_fsc_AllocVMem(uint8_t *sv, int32_t size) {
    uint64_t cur = AT(sv, 0x120, uint64_t);
    uint64_t next = ptr_add(cur, round8(size));
    AT(sv, 0x120, uint64_t) = next;
    return next > AT(sv, 0x130, uint64_t) ? NULL : (void *)cur;
}

/* FixPointers: the RevRoots block was moved; rebase its pointers and the list links. The
 * sentinel's next pointer is left alone. */
TT_EXPORT void tt_FixPointers(uint8_t *roots) {
    uint64_t delta = (uint64_t)roots - AT(roots, 0x20, uint64_t);
    for (int i = 0; i < 4; i++) AT(roots, 8 * i, uint64_t) += delta;
    for (int l = 0; l < 2; l++) {
        uint8_t *n = AT(roots, 8 * l, uint8_t *);
        while (AT(n, 0, int16_t) < 0x7fff) {
            AT(n, 8, uint64_t) += delta;
            n = AT(n, 8, uint8_t *);
        }
    }
    AT(roots, 0x20, uint8_t *) = roots;
}

/* Five tables of `delta` bytes from the pool at cur_off/end_off into sv + tab .. tab + 0x20.
 * Returns the new cursor, 0 on failure (the slot that failed is set to NULL). */
static int alloc_tables(uint8_t *sv, int cur_off, int end_off, int tab, int64_t delta, uint64_t *cursor) {
    uint64_t cur = AT(sv, cur_off, uint64_t), end = 0;
    for (int i = 0; i < 5; i++) {
        uint64_t next = ptr_add(cur, delta);
        AT(sv, cur_off, uint64_t) = next;
        if (i == 0) end = AT(sv, end_off, uint64_t);  /* the original reads the end once, here */
        if (next > end) {
            AT(sv, tab + 8 * i, uint64_t) = 0;
            return 0;
        }
        AT(sv, tab + 8 * i, uint64_t) = cur;
        if (cur == 0) return 0;
        cur = next;
    }
    *cursor = cur;
    return 1;
}

/* crossing memory: count << (shift + 2) bytes (int16 pairs per reversal unit) from a pool */
static int alloc_crossings(uint8_t *sv, int cur_off, uint64_t end, int32_t count, uint64_t *base) {
    int64_t sh = (int64_t)AT(sv, 0x114, int16_t) + 2;
    if ((uint64_t)sh + 0x80000000u > 0xffffffffu) overflow();
    int32_t bytes = (int32_t)((uint32_t)count << (sh & 31));
    uint64_t cur = AT(sv, cur_off, uint64_t);
    uint64_t next = ptr_add(cur, round8(bytes));
    AT(sv, cur_off, uint64_t) = next;
    if (next > end || cur == 0) return 0;
    *base = cur;
    return 1;
}

/* walk the reversal list up to scan line s, adding the counts (16-bit, shifted by +0x114) */
static inline void advance(uint8_t *sv, uint8_t **node, int16_t s, uint16_t *acc) {
    while (AT(*node, 0, int16_t) <= s) {
        int sh = AT(sv, 0x114, int16_t) & 31;
        *acc = (uint16_t)(*acc + (uint16_t)((uint32_t)AT(*node, 2, uint16_t) << sh));
        *node = AT(*node, 8, uint8_t *);
    }
}

/* fill the five tables for one scan line and advance the memory pointer */
static inline void set_line(uint64_t **t, uint64_t *mem, uint16_t acc) {
    int64_t half = (int64_t)(int16_t)acc * 2;
    *t[0]++ = *mem;
    *t[2]++ = *mem;
    *mem += (uint64_t)half;
    *t[1]++ = *mem;
    *t[3]++ = *mem;
    *mem += (uint64_t)half;
    *t[4]++ = *mem;
}

/* fsc_SetupScan(StateVars *, Rect const *, ushort flags, bool, int row crossings,
 *               int column crossings, int elements, RevRoots *) */
TT_EXPORT int tt_fsc_SetupScan(uint8_t *sv, const int16_t *rect, uint16_t flags, uint8_t keep,
                               int32_t hcount, int32_t vcount, int32_t elements, uint8_t *roots) {
    AT(sv, 0x20, int32_t) = rect[0];
    AT(sv, 0x24, int32_t) = rect[2];
    AT(sv, 0x18, int32_t) = rect[1];
    AT(sv, 0x1c, int32_t) = rect[3];
    AT(sv, 0xf0, uint8_t) = keep;

    /* the adders are ported elsewhere and swapped at their entries: store the originals */
    int smart = !(flags & 2) && (flags & 4);
    AT(sv, 0x112, int16_t) = smart ? 2 : 1;
    AT(sv, 0x114, int16_t) = smart ? 1 : 0;
    AT(sv, 0xb0, void *) = ORIG(void *, smart ? 0xa5570 : 0x965e0);  /* AddHoriz{Smart,Simple}Scan */
    AT(sv, 0xb8, void *) = ORIG(void *, smart ? 0xa53d0 : 0x95d90);  /* AddVert{Smart,Simple}Scan */

    /* rows: five tables of (top - bottom) pointers from pool 1, then the crossing memory */
    uint64_t mem;
    int64_t rows = round8(times8((int64_t)rect[0] - rect[2]));
    if (!alloc_tables(sv, 0x118, 0x128, 0x30, rows, &mem)) return ERR_SCAN_MEMORY;
    uint64_t end1 = AT(sv, 0x128, uint64_t);
    AT(sv, 0x108, int32_t) = elements;
    AT(sv, 0x10c, int32_t) = 0;
    if ((uint32_t)hcount > 0x7ffffff) return ERR_SCAN_MEMORY;
    uint64_t base;
    if (!alloc_crossings(sv, 0x118, end1, hcount, &base)) return ERR_SCAN_MEMORY;

    if (roots != AT(roots, 0x20, uint8_t *)) ORIG(void (*)(uint8_t *), 0x10b2fc)(roots);  /* FixPointers */

    int32_t y32 = AT(sv, 0x24, int32_t);
    uint16_t y = (uint16_t)y32, acc = 0;
    uint8_t *node = AT(roots, 0, uint8_t *);
    uint64_t *t[5] = {AT(sv, 0x30, uint64_t *), AT(sv, 0x38, uint64_t *), AT(sv, 0x40, uint64_t *),
                      AT(sv, 0x48, uint64_t *), AT(sv, 0x50, uint64_t *)};
    if ((uint32_t)y32 + 0x8000u > 0xffffu) overflow();  /* SafeInt<short> conversions */
    for (;;) {
        int32_t top = AT(sv, 0x20, int32_t);
        if ((uint32_t)top + 0x8000u > 0xffffu) overflow();
        if ((int16_t)top <= (int16_t)y) break;
        advance(sv, &node, (int16_t)y, &acc);
        set_line(t, &base, acc);
        y++;
    }
    if (flags & 2) return 0;

    /* columns (dropout control): the same from pool 2 */
    int64_t cols = round8(times8((int64_t)rect[3] - rect[1]));
    if (!alloc_tables(sv, 0x120, 0x130, 0x70, cols, &mem)) return ERR_SCAN_MEMORY;
    if ((uint32_t)vcount > 0x7ffffff) return ERR_SCAN_MEMORY;
    if (!alloc_crossings(sv, 0x120, AT(sv, 0x130, uint64_t), vcount, &base)) return ERR_SCAN_MEMORY;

    uint16_t x = (uint16_t)rect[1];
    acc = 0;
    node = AT(roots, 8, uint8_t *);
    uint64_t *u[5] = {AT(sv, 0x70, uint64_t *), AT(sv, 0x78, uint64_t *), AT(sv, 0x80, uint64_t *),
                      AT(sv, 0x88, uint64_t *), AT(sv, 0x90, uint64_t *)};
    while ((int16_t)x < rect[3]) {
        advance(sv, &node, (int16_t)x, &acc);
        set_line(u, &base, acc);
        x++;
    }
    if (!(flags & 4)) return 0;

    /* smart dropout: contour start x and y arrays of `elements` int32 from pool 2 */
    if (elements < 0 || (uint32_t)elements > 0xfffffff) return ERR_SCAN_MEMORY;
    uint64_t bytes = (uint64_t)(uint32_t)elements << 2;
    if (bytes > 0x7fffffff) overflow();
    void *(*alloc)(uint8_t *, int32_t) = ORIG(void *(*)(uint8_t *, int32_t), 0x71720);  /* fsc_AllocVMem */
    if (!(AT(sv, 0xf8, void *) = alloc(sv, (int32_t)bytes))) return ERR_SCAN_MEMORY;
    if (!(AT(sv, 0x100, void *) = alloc(sv, (int32_t)bytes))) return ERR_SCAN_MEMORY;
    return 0;
}
