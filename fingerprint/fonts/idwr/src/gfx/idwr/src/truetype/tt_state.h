/*
 * Layout of the TrueType interpreter state in DWrite.dll 10.0.26100.9711, kept byte for byte
 * so the port can run on the original's live state (lockstep checks, notes/truetype.md).
 * Fields are reached by offset; names are given as their meaning is established.
 *
 *   gs  fnt_LocalGraphicStateType, 0xD8 bytes, built by itrp_Execute on its stack
 *   G   fnt_GlobalGraphicStateType (gs + 0x40)
 *   E   fnt_ElementType, 0x70 bytes (zone: 0 = twilight, 1 = glyph)
 */
#ifndef TT_STATE_H
#define TT_STATE_H
#include <stdint.h>
#include "fp_exact.h"  /* no FMA contraction */
#include <stddef.h>

/* functions the oracle swaps in for DWrite.dll's (reimpl/oracle.py --swap); built into the
 * engine (IDWR_STATIC) nothing is exported */
#if defined(IDWR_STATIC)
#define TT_EXPORT
#elif defined(_WIN32)
#define TT_EXPORT __declspec(dllexport)
#else
#define TT_EXPORT __attribute__((visibility("default")))
#endif
#ifndef _WIN32
/* the original's debugger messages (bogus table sizes, illegal cases): nothing observable */
static inline void OutputDebugStringA(const char *s) { (void)s; }
#endif

#define AT(p, o, T) (*(T *)((uint8_t *)(p) + (o)))

/* ---- local graphics state ---- */
#define GS_CE0(gs)        AT(gs, 0x00, uint8_t *)   /* zone pointers (fnt_ElementType *) */
#define GS_CE1(gs)        AT(gs, 0x08, uint8_t *)
#define GS_CE2(gs)        AT(gs, 0x10, uint8_t *)
#define GS_PROJ_X(gs)     AT(gs, 0x18, int16_t)     /* projection vector, 2.14 */
#define GS_PROJ_Y(gs)     AT(gs, 0x1a, int16_t)
#define GS_FREE_X(gs)     AT(gs, 0x1c, int16_t)     /* freedom vector */
#define GS_FREE_Y(gs)     AT(gs, 0x1e, int16_t)
#define GS_OPROJ_X(gs)    AT(gs, 0x20, int16_t)     /* dual projection vector */
#define GS_OPROJ_Y(gs)    AT(gs, 0x22, int16_t)
#define GS_SP(gs)         AT(gs, 0x28, int32_t *)   /* stack pointer: next free slot */
#define GS_INSPTR(gs)     AT(gs, 0x30, const uint8_t *)
#define GS_ELEMENTS(gs)   AT(gs, 0x38, uint8_t *)   /* zone array, 0x70 per zone */
#define GS_G(gs)          AT(gs, 0x40, uint8_t *)
#define GS_TRACE(gs)      AT(gs, 0x48, void *)
#define GS_RP0(gs)        AT(gs, 0x50, int32_t)
#define GS_RP1(gs)        AT(gs, 0x54, int32_t)
#define GS_RP2(gs)        AT(gs, 0x58, int32_t)
#define GS_OPCODE(gs)     AT(gs, 0x60, uint8_t)
#define GS_PFPROJ(gs)     AT(gs, 0x62, int16_t)     /* projection . freedom, 2.14 */
#define GS_MOVEPOINT(gs)  AT(gs, 0x68, void *)
#define GS_PROJECT(gs)    AT(gs, 0x70, void *)
#define GS_OLDPROJECT(gs) AT(gs, 0x78, void *)
#define GS_INTERP(gs)     AT(gs, 0x80, void *)
#define GS_GETCVT(gs)     AT(gs, 0x88, void *)
#define GS_GETSW(gs)      AT(gs, 0x90, void *)
#define GS_CHANGECVT(gs)  AT(gs, 0x98, void *)
#define GS_ERROR(gs)      AT(gs, 0xa8, int32_t)
#define GS_ENDPTR(gs)     AT(gs, 0xb0, const uint8_t *)
#define GS_STARTPTR(gs)   AT(gs, 0xb8, const uint8_t *)
#define GS_JUMPS(gs)      AT(gs, 0xc0, int32_t)     /* jump budget (JMPR, JROT, JROF) */
#define GS_BUDGET(gs)     AT(gs, 0xc8, int32_t)     /* instruction budget */

/* ---- global graphics state ---- */
#define G_STACKBASE(G)    AT(G, 0x00, int32_t *)
#define G_STACKSIZE(G)    AT(G, 0x08, uint16_t)     /* in entries */
#define G_STORAGE(G)      AT(G, 0x10, int32_t *)
#define G_CVT(G)          AT(G, 0x18, int32_t *)
#define G_FLAGS88(G)      AT(G, 0x88, uint32_t)     /* instruct control and friends */
#define G_DELTABASE(G)    AT(G, 0xa8, int16_t)
#define G_DELTASHIFT(G)   AT(G, 0xaa, int16_t)
#define G_AUTOFLIP(G)     AT(G, 0xb0, uint8_t)
#define G_MAXP(G)         AT(G, 0x178, uint8_t *)   /* native LocalMaxProfile, maxp field order */
#define G_CVTCOUNT(G)     AT(G, 0x180, uint16_t)
#define G_COMPAT1C0(G)    AT(G, 0x1c0, uint16_t)
#define G_COMPAT1C2(G)    AT(G, 0x1c2, uint16_t)
#define MAXP_TWILIGHT(m)  AT(m, 0x10, uint16_t)
#define MAXP_STORAGE(m)   AT(m, 0x12, uint16_t)
#define MAXP_POINTS(m)    AT(m, 0x06, uint16_t)
#define MAXP_CPOINTS(m)   AT(m, 0x0a, uint16_t)

/* ---- zone ---- */
#define E_X(e)            AT(e, 0x00, int32_t *)
#define E_Y(e)            AT(e, 0x08, int32_t *)
#define E_OX(e)           AT(e, 0x10, int32_t *)
#define E_OY(e)           AT(e, 0x18, int32_t *)
#define E_OOX(e)          AT(e, 0x20, int32_t *)
#define E_OOY(e)          AT(e, 0x28, int32_t *)
#define E_ONCURVE(e)      AT(e, 0x30, uint8_t *)
#define E_SP(e)           AT(e, 0x38, int16_t *)
#define E_EP(e)           AT(e, 0x40, int16_t *)
#define E_F(e)            AT(e, 0x48, uint8_t *)
#define E_NC(e)           AT(e, 0x50, int16_t)

/* error codes seen in the handlers */
#define ERR_DIVIDE_BY_ZERO 0x1103
#define ERR_JUMP_BUDGET    0x1107
#define ERR_JUMP_BEFORE    0x110b
#define ERR_BUDGET         0x110e
#define ERR_STACK_UNDER    0x1110
#define ERR_STACK_OVER     0x1111
#define ERR_INDEX          0x1113
#define ERR_STORAGE        0x1119
#define ERR_INSPTR         0x111d

typedef const uint8_t *(*tt_handler)(uint8_t *gs, const uint8_t *p, int op);
extern tt_handler tt_opcodes[256];

/* functions the state points to: original RVA <-> port (terminated by rva 0) */
typedef struct { uint32_t rva; void *port; } tt_fnmap;
extern const tt_fnmap tt_fn_map[];

#endif
