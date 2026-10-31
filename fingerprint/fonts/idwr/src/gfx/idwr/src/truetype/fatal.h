/*
 * The library's fatal errors: states the original DLL also cannot leave (a SafeInt handler
 * with no catch frame, an RVA with no port, a failed allocation it does not survive either).
 *
 * Standalone and in the harnesses each one aborts, as it always has. Built into the engine
 * (IDWR_STATIC) it first calls idwr_host_fatal with the site, which the engine defines and
 * maps to MOZ_CRASH, so a crash report names where it stopped. There is no hook variable and
 * no setter: under IDWR_STATIC the symbol must exist at link time, or libxul does not link.
 */
#ifndef IDWR_FATAL_H
#define IDWR_FATAL_H
#include <stdlib.h>

/* IDWR_DIAG: the line printed on stderr before a fatal error, for the harnesses. The engine
 * has no stderr anybody reads, and the crash report already names the site. */
#ifdef IDWR_STATIC
void idwr_host_fatal(const char *site);
#define IDWR_FATAL(site) (idwr_host_fatal(site), abort())
#define IDWR_DIAG(...) ((void)0)
#define IDWR_DIAG_FLUSH() ((void)0)
#else
#include <stdio.h>
#define IDWR_FATAL(site) abort()
#define IDWR_DIAG(...) fprintf(stderr, __VA_ARGS__)
#define IDWR_DIAG_FLUSH() fflush(stderr)
#endif

/* The counter the standalone check reads (idwr_orig_native_hits, orig.c). Built into the
 * engine it would be a write shared by every thread that draws text, so it is compiled out
 * there unless a test build asks for it with IDWR_TESTING. */
#if !defined(IDWR_STATIC) || defined(IDWR_TESTING)
#define IDWR_COUNT_NATIVE_HIT(i) (g_native_hits[i]++)
#else
#define IDWR_COUNT_NATIVE_HIT(i) ((void)0)
#endif

#endif
