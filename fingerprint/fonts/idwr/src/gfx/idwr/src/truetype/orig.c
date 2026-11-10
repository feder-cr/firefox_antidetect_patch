/*
 * idwr_orig: the address of an original DWrite.dll function or datum, by RVA (orig.h).
 *
 * Oracle mode: DLL base + RVA, the base from the oracle that loaded the pinned DWrite.dll.
 * Standalone mode (-DIDWR_STANDALONE): the port that replaces that original (the table
 * reimpl/gen_orig_table.py generates from reimpl/swaps.txt, then interp.c's tt_fn_map for the
 * interpreter functions stored in its state), else a native helper (native.c). Any other RVA
 * is a hole in the standalone scaler: it is printed and the process aborts.
 */
#include <stdint.h>
#include "fatal.h"
#include "orig.h"
#include "tt_state.h"
#include "native.h"

#ifndef IDWR_STANDALONE

void *oracle_dwrite_base(void); /* reimpl/oracle/fs_oracle.c */

void *idwr_orig(uint32_t rva) { return (uint8_t *)oracle_dwrite_base() + rva; }

#else

#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint32_t rva;
    const char *name;
    void *fn;
} idwr_port_entry;
#include "orig_ports.h"

#define MAX_NATIVE 256
static uint32_t g_native_hits[MAX_NATIVE];

static void *find_port(uint32_t rva) {
    size_t lo = 0, hi = IDWR_PORT_COUNT;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (idwr_port_table[mid].rva < rva) lo = mid + 1;
        else hi = mid;
    }
    return lo < IDWR_PORT_COUNT && idwr_port_table[lo].rva == rva ? idwr_port_table[lo].fn : NULL;
}

void *idwr_orig(uint32_t rva) {
    void *p = find_port(rva);
    if (p) return p;
    for (int i = 0; idwr_native_table[i].fn; i++)
        if (idwr_native_table[i].rva == rva) {
            if (i < MAX_NATIVE) IDWR_COUNT_NATIVE_HIT(i);
            return idwr_native_table[i].fn;
        }
    for (const tt_fnmap *m = tt_fn_map; m->rva; m++)
        if (m->rva == rva) return m->port;
    IDWR_DIAG("idwr_orig: no port or native helper for DWrite.dll RVA 0x%x (0x%llx)\n", rva,
              0x180000000ULL + rva);
    IDWR_DIAG_FLUSH();
    IDWR_FATAL("orig.c:60");
}

/* how many times idwr_orig returned the native helper for `rva` */
TT_EXPORT uint32_t idwr_orig_native_hits(uint32_t rva) {
    for (int i = 0; idwr_native_table[i].fn && i < MAX_NATIVE; i++)
        if (idwr_native_table[i].rva == rva) return g_native_hits[i];
    return 0;
}

/* 1 when idwr_orig resolves `rva` without aborting (port, native helper or tt_fn_map) */
TT_EXPORT int idwr_orig_resolves(uint32_t rva) {
    if (find_port(rva)) return 1;
    for (int i = 0; idwr_native_table[i].fn; i++)
        if (idwr_native_table[i].rva == rva) return 1;
    for (const tt_fnmap *m = tt_fn_map; m->rva; m++)
        if (m->rva == rva) return 1;
    return 0;
}

#endif
