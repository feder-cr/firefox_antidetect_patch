/*
 * Floating point evaluated exactly as written, in every translation unit of idwr (included by
 * idwr/idwr_internal.h, tt_state.h and portable.h): no a * b + c fused into one rounding.
 *
 * DWrite's x86-64 SSE code never fuses. Clang contracts by default on aarch64: without
 * -ffp-contract=off, idwr/outline.c and idwr/variation.c get 33 fmadd/fmsub there. GCC outside ISO
 * mode contracts too, and so may MSVC on ARM64. The builds pass -ffp-contract=off as well, but a
 * build that forgets it (another build system, another compiler) must still give the same bits.
 * A file-scope pragma holds to the end of the translation unit.
 */
#ifndef IDWR_FP_EXACT_H
#define IDWR_FP_EXACT_H
#if defined(IDWR_NEGATIVE_CONTROL_FMA)
/* linux_check.py --arch aarch64 --fma-control: contraction left to the compiler flags */
#elif defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#endif
#endif
