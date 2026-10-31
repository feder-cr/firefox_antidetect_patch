/*
 * Addresses of original DWrite.dll functions and data (10.0.26100.9711), by RVA. Every call a
 * port makes to an original, and every original address a port stores or compares, goes
 * through idwr_orig (reimpl/truetype/orig.c):
 *   oracle mode (default)       DLL base + RVA: while the scaler is ported function by
 *                               function, a ported function calls the original of every
 *                               callee, and the oracle's function swap redirects the ported
 *                               ones (reimpl/oracle/fs_oracle.c provides the base);
 *   standalone (-DIDWR_STANDALONE) the port for that RVA (reimpl/swaps.txt, interp.c's
 *                               tt_fn_map) or a native helper (native.c); any other RVA
 *                               prints it and aborts. DWrite.dll is never loaded.
 */
#ifndef TT_ORIG_H
#define TT_ORIG_H
#include <stdint.h>

void *idwr_orig(uint32_t rva);
#define ORIG(type, rva) ((type)idwr_orig(rva))

#endif
