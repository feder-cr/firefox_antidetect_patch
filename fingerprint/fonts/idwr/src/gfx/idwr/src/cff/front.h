/* The records the Adobe front end (front.c, notes/cff.md group "front") builds on its stack
 * and hands to the consumers through the proc table (ATMBuildBitMap, ATMCharOutline,
 * ATMQRedCharBitMap). Pinned down from AdobeInternalGet{Bitmap,Metrics,Outline} 0x18008c528,
 * 0x18008c0c0, 0x18008aef0, AdobePostRenderCommon 0x18008c340, ValidateCharDataDesc
 * 0x18008da30, and from what the consumers read (ATMCharOutline copies a _t_CharIO as six
 * qwords, ATMBuildBitMap reads io->grow[3], ConvertRuns lays out the bitmap record).
 *
 * The proc call: consumer(state = inst + 0x9a0, memory = inst + 0x610, cff_t1_char_data_desc *,
 * cff_char_io *, arg5, flags, inst) where arg5 is the cff_bitmap_rec * for ATMBuildBitMap /
 * ATMQRedCharBitMap and the cff_path_procs * (or an all-NULL one for metrics) for
 * ATMCharOutline; flags = (RenderingFlags & 1) | (RenderingFlags & 2). */
#ifndef CFF_FRONT_H
#define CFF_FRONT_H
#include <stddef.h>
#include <stdint.h>

/* _t_T1CharDataDesc, 0x58 bytes, zeroed by the caller (memset) */
typedef struct cff_t1_char_data_desc {
    uint8_t *cs_start;        /* +0x00 first byte of the charstring (GetCharStringPointers) */
    uint8_t *cs_end;          /* +0x08 its last byte (INDEX base - 1 + next offset) */
    uint8_t *lsubrs_base;     /* +0x10 CFFFontInst + 0x4f8: local subrs INDEX data base */
    uint8_t *lsubrs_offsets;  /* +0x18 CFFFontInst + 0x500: their offset array, count + 1 entries */
    uint8_t zero20[0x10];     /* +0x20 zero at the call (written by the interpreter) */
    uint8_t *gsubrs_base;     /* +0x30 CFFFontInst + 0x508: global subrs INDEX data base */
    uint8_t *gsubrs_offsets;  /* +0x38 CFFFontInst + 0x510: their offset array */
    uint32_t zero40;          /* +0x40 */
    uint8_t lsubrs_offsize;   /* +0x44 CFFFontInst + 0x552 (1..4, checked by ValidateCharDataDesc) */
    uint8_t zero45;
    uint8_t gsubrs_offsize;   /* +0x46 CFFFontInst + 0x554 */
    uint8_t zero47;
    uint8_t *data_lo;         /* +0x48 font data start (ValidateCharDataDesc) */
    uint8_t *data_hi;         /* +0x50 font data end */
} cff_t1_char_data_desc;

/* one of the nine grow buffers at CFFFontInst + 0x640 + 16 * i */
typedef struct cff_grow_buff {
    void *ptr;
    uint32_t size;
    uint32_t pad;
} cff_grow_buff;

/* _t_CharIO, 0x30 bytes */
typedef struct cff_char_io {
    cff_grow_buff **grow;     /* +0x00 nine pointers to CFFFontInst + 0x640 ... + 0x6c0 */
    uint8_t *metrics;         /* +0x08 a zeroed 0x28-byte record; +0x20/+0x24 = advance x/y (16.16) */
    int32_t *box;             /* +0x10 Fixed xMin, yMin, xMax, yMax (16 zeroed bytes) */
    uint32_t flags18;         /* +0x18 0, or 0x2000 for a CFF2 bitmap (AdobeInternalGetBitmap) */
    uint32_t flags1c;         /* +0x1c 0; Type1InterpretCharString ORs 0x2000 into it on a retry */
    uint32_t zero20;          /* +0x20 */
    uint32_t zero24;          /* +0x24 */
    uint32_t zero28;          /* +0x28 u16 +0x28/+0x2a copied into the state by the interpreter */
    uint32_t zero2c;          /* +0x2c */
} cff_char_io;

/* the bitmap record of ATMBuildBitMap / ATMQRedCharBitMap / ConvertRuns, 0x30 bytes */
typedef struct cff_bitmap_rec {
    uint8_t *bits;            /* +0x00 in: the caller's buffer; out: possibly reallocated */
    uint32_t capacity;        /* +0x08 in/out: its size in bytes */
    uint32_t row_bytes;       /* +0x0c 0 in; ConvertRuns: (width + 31) >> 3 & ~3 */
    uint64_t zero10;          /* +0x10 */
    uint32_t rows;            /* +0x18 */
    uint32_t width;           /* +0x1c pixels */
    int32_t origin_x;         /* +0x20 -floor(xMin) */
    int32_t origin_y;         /* +0x24 */
    uint32_t zero28;          /* +0x28 */
    uint32_t zero2c;          /* +0x2c */
} cff_bitmap_rec;

/* the client path callbacks of AdobeInternalGetOutline (all NULL for metrics), 0x30 bytes */
typedef struct cff_path_procs {
    void *new_char, *move_to, *line_to, *curve_to, *close_path, *zero;
} cff_path_procs;

/* _t_BCRendProp as AdobeDoSetUpIfNeeded fills it (0x18 bytes, zeroed by the callers) */
typedef struct cff_rend_prop {
    int32_t a0;               /* +0x00 0x16a0a */
    int32_t a4;               /* +0x04 0x16a0a */
    uint16_t voverscale8;     /* +0x08 1, or the vertical overscale 5 / 4 */
    uint16_t one;             /* +0x0a 1 */
    uint16_t voverscale_c;    /* +0x0c as +0x08 */
    uint8_t zero_e[4];        /* +0x0e u32 0 (unaligned) */
    uint8_t pad12[2];
    uint32_t mode14;          /* +0x14 3 with a vertical overscale, else 0 */
} cff_rend_prop;

#define FRONT_CHECK(t, f, o) _Static_assert(offsetof(t, f) == (o), #t "." #f)
FRONT_CHECK(cff_t1_char_data_desc, gsubrs_offsets, 0x38);
FRONT_CHECK(cff_t1_char_data_desc, gsubrs_offsize, 0x46);
FRONT_CHECK(cff_t1_char_data_desc, data_hi, 0x50);
_Static_assert(sizeof(cff_t1_char_data_desc) == 0x58, "_t_T1CharDataDesc");
FRONT_CHECK(cff_char_io, flags18, 0x18);
FRONT_CHECK(cff_char_io, zero2c, 0x2c);
_Static_assert(sizeof(cff_char_io) == 0x30, "_t_CharIO");
FRONT_CHECK(cff_bitmap_rec, rows, 0x18);
FRONT_CHECK(cff_bitmap_rec, origin_y, 0x24);
_Static_assert(sizeof(cff_bitmap_rec) == 0x30, "bitmap record");
FRONT_CHECK(cff_rend_prop, mode14, 0x14);
_Static_assert(sizeof(cff_rend_prop) == 0x18, "_t_BCRendProp");

/* CFF_EXPORT for functions with stack arguments (5 to 10 arguments): the same
 * register-preserving thunk, which also copies the six stack argument slots of the caller
 * (a slot past the real arguments reads the caller's frame and is ignored). Layout: home
 * area 0x20 + 6 slots at 0x20..0x4f, XMM1-5 at 0x50..0x9f, frame 0xa8 after six pushes, so
 * the caller's fifth argument is at 0x100(%rsp). */
#include "cff_port.h"
#if defined(IDWR_STANDALONE)
#define CFF_EXPORT_STACK(name, impl) CFF_EXPORT(name, impl)
#elif defined(_WIN32)
#define CFF_EXPORT_STACK(name, impl)                                                   \
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
            "subq $0xa8, %rsp\n.seh_stackalloc 0xa8\n"                                 \
            "movaps %xmm1, 0x50(%rsp)\n.seh_savexmm %xmm1, 0x50\n"                     \
            "movaps %xmm2, 0x60(%rsp)\n.seh_savexmm %xmm2, 0x60\n"                     \
            "movaps %xmm3, 0x70(%rsp)\n.seh_savexmm %xmm3, 0x70\n"                     \
            "movaps %xmm4, 0x80(%rsp)\n.seh_savexmm %xmm4, 0x80\n"                     \
            "movaps %xmm5, 0x90(%rsp)\n.seh_savexmm %xmm5, 0x90\n"                     \
            ".seh_endprologue\n"                                                       \
            "movq 0x100(%rsp), %r10\nmovq %r10, 0x20(%rsp)\n"                          \
            "movq 0x108(%rsp), %r10\nmovq %r10, 0x28(%rsp)\n"                          \
            "movq 0x110(%rsp), %r10\nmovq %r10, 0x30(%rsp)\n"                          \
            "movq 0x118(%rsp), %r10\nmovq %r10, 0x38(%rsp)\n"                          \
            "movq 0x120(%rsp), %r10\nmovq %r10, 0x40(%rsp)\n"                          \
            "movq 0x128(%rsp), %r10\nmovq %r10, 0x48(%rsp)\n"                          \
            "callq " #impl "\n"                                                        \
            "movaps 0x50(%rsp), %xmm1\n"                                               \
            "movaps 0x60(%rsp), %xmm2\n"                                               \
            "movaps 0x70(%rsp), %xmm3\n"                                               \
            "movaps 0x80(%rsp), %xmm4\n"                                               \
            "movaps 0x90(%rsp), %xmm5\n"                                               \
            "addq $0xa8, %rsp\n"                                                       \
            "popq %r11\npopq %r10\npopq %r9\npopq %r8\npopq %rdx\npopq %rcx\n"         \
            "retq\n"                                                                   \
            ".seh_endproc\n"                                                           \
            ".section .drectve,\"yn\"\n"                                               \
            ".ascii \" -export:" #name "\"\n"                                          \
            ".text\n")
#endif

#endif
