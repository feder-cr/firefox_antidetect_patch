/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The arithmetic of Skia's raster pipeline that depends on the CPU, made the same on
// every CPU (engine objective 10: the 2D canvas gives the same bytes everywhere).
//
// Measured (invisible_skia_cpu, canvas-blend/skia-cpu-dispatch-m149): Skia's raster
// pipeline on a CPU with AVX2 (the retail code) and on a CPU without it gives different
// bytes in 26 of 182 canvas cases, for two reasons:
//   1. the baseline (SSE2) code multiplies and adds in two steps where the AVX2 code
//      fuses them (mad, nmad), and it refines the reciprocal with a Newton step where
//      the AVX2 code uses the raw estimate of RCPPS (rcp_fast, rsqrt);
//   2. RCPPS and RSQRTPS are not specified bit for bit: Intel and AMD give different
//      estimates.
// What this file declares instead, with the engine on:
//   - mad and nmad are an exact fused multiply-add on every CPU (Fma below, which is
//     std::fma without the host: integer-exact steps in double, round to odd);
//   - rcp_fast, rsqrt and rcp_precise are the AVX2 ones, and the estimate they start
//     from is a model of RCPPS/RSQRTPS of one vendor, declared by zoom.stealth.cpu.vendor
//     (a table per vendor and the exponent arithmetic), never the instruction of the host.
//
// The functions are `static inline` on purpose: this header is included by translation
// units compiled for different instruction sets (SkOpts.cpp for SSE2, SkOpts_ml3.cpp for
// AVX2), and an `inline` function with external linkage would be one function for the
// linker, which may pick the AVX2 copy for the SSE2 caller. Nothing here uses an
// instruction of a particular set, and the only state is in StealthSkiaMath.cpp.

#ifndef STEALTH_SKIA_MATH_H_
#define STEALTH_SKIA_MATH_H_

#include <stdint.h>
#include <string.h>

#include <atomic>

namespace StealthSkiaMath {

constexpr int kOff = 0;    // the engine is off: stock Skia, byte for byte
constexpr int kIntel = 1;  // zoom.stealth.cpu.vendor = intel
constexpr int kAmd = 2;    // zoom.stealth.cpu.vendor = amd

// The mode of the process: -1 until resolved. It is resolved by ResolveMode() from
// zoom.stealth.fpp.hw_seed and zoom.stealth.cpu.vendor, ONCE, the first time the engine is
// seen on; while the engine is off (the prefs of a launched browser arrive after the
// process is up) it answers kOff without remembering it.
extern std::atomic<int> gMode;
int ResolveMode();

// Whether the AMD table is in the source (gen_stealth_skia_math.py wrote it). While it
// is not, the AMD mode is refused.
extern const bool kIntelTablePresent;
extern const bool kAmdTablePresent;
// The tables of a mode (index kIntel, kAmd; null where there is none) and their k, the number
// of high mantissa bits that decide the output: RCPPS 2^k_rcp words (Intel 11, AMD 12), RSQRTPS
// 2 x 2^k_rsq words flat (index parity * 2^k_rsq + the k_rsq high mantissa bits; Intel 10,
// AMD 12). k is read by gen_stealth_skia_math.py from the size of each vendor's dump.
extern const uint32_t* const kRcpByMode[3];
extern const uint32_t* const kRsqByMode[3];
extern const int kRcpKByMode[3];
extern const int kRsqKByMode[3];

// Tests only: force a mode, or forget it so that the next use resolves it again.
void SetModeForTest(int aMode);
void ReloadForTest();

static inline int Mode() {
  const int m = gMode.load(std::memory_order_relaxed);
  return m >= 0 ? m : ResolveMode();
}
static inline bool On() { return Mode() > 0; }

// RCPPS, as a function of the bits: the table of 1.m (m = the aK high bits of the
// mantissa, exponent 127) and the arithmetic of the exponent. Verified against the
// instruction of an Intel CPU (aK 11) and of an AMD CPU (aK 12) for the 2139095040
// positive finite inputs; the gtest pins the SHA-256 of the sweep of each.
static inline uint32_t RcpBits(uint32_t x, const uint32_t* aTable, int aK) {
  const uint32_t s = x & 0x80000000u, e = (x >> 23) & 255, m = x & 0x7fffffu;
  if (e == 255) {
    return m ? (x | 0x00400000u) : s;  // NaN stays NaN, quiet; infinity gives zero
  }
  if (e == 0) {
    return s | 0x7f800000u;  // zero and denormals give infinity
  }
  const uint32_t b = aTable[m >> (23 - aK)];
  const int ne = int((b >> 23) & 255) + 127 - int(e);
  if (ne >= 255) {
    return s | 0x7f800000u;
  }
  if (ne <= 0) {
    return s;
  }
  return s | (b & 0x007fffffu) | (uint32_t(ne) << 23);
}

// RSQRTPS likewise: the table of the exponent's parity (odd: 127, even: 128) and the aK
// high mantissa bits, then half the exponent; negative numbers give the default NaN.
static inline uint32_t RsqBits(uint32_t x, const uint32_t* aTable, int aK) {
  const uint32_t s = x & 0x80000000u, e = (x >> 23) & 255, m = x & 0x7fffffu;
  if (e == 255) {
    return m ? (x | 0x00400000u) : (s ? 0xffc00000u : 0u);
  }
  if (e == 0) {
    return s | 0x7f800000u;  // zero and denormals: infinity of the sign (-0 gives -inf)
  }
  if (s) {
    return 0xffc00000u;
  }
  const int parity = (e & 1) ? 0 : 1;
  const int be0 = (e & 1) ? 127 : 128;
  const uint32_t b = aTable[(uint32_t(parity) << aK) + (m >> (23 - aK))];
  const int ne = int((b >> 23) & 255) + (be0 - int(e)) / 2;
  if (ne >= 255) {
    return 0x7f800000u;
  }
  if (ne <= 0) {
    return 0u;
  }
  return (b & 0x007fffffu) | (uint32_t(ne) << 23);
}

// std::fma(a, b, c) without the host's FMA, bit for bit (NaN payloads aside: the NaN a
// fused instruction returns depends on the form the compiler picked). The product of two
// floats is exact in a double (48 bits). The sum is rounded to odd in double (Boldo and
// Melquiond: the error of the sum is found exactly with TwoSum, and when it is not zero
// an even result is moved to the odd neighbour on the side of the error); a double has
// more than the 2 * 24 + 2 bits that make the final rounding to float, which is the
// hardware's round to nearest even, the single correct rounding of a * b + c. That holds
// for the denormal results too, and the overflow. No libm, no FMA instruction.
static inline float Fma(float aA, float aB, float aC) {
  const double p = double(aA) * double(aB);  // exact
  const double c = double(aC);
  double s = p + c;
  uint64_t bits;
  memcpy(&bits, &s, sizeof bits);
  // Infinities and NaNs (an input, or inf - inf, inf * 0) follow IEEE in the double.
  if (((bits >> 52) & 0x7ff) == 0x7ff) {
    return float(s);
  }
  const double bb = s - p;
  const double err = (p - (s - bb)) + (c - bb);  // TwoSum: s + err == p + c exactly
  if (err != 0.0) {
    if (!(bits & 1)) {
      // s is even: move it one unit in the last place toward the side of err (in the
      // magnitude: up when err has the sign of s, down when it has the other).
      bits += ((err > 0.0) == (s > 0.0)) ? 1 : uint64_t(-1);
      memcpy(&s, &bits, sizeof s);
    }
  }
  return float(s);
}

// The lanes of any vector type F of floats (4 or 8 of them), through memory.
template <typename F>
static inline F FmaLanes(F aF, F aM, F aA) {
  constexpr int N = int(sizeof(F) / sizeof(float));
  float f[N], m[N], a[N];
  memcpy(f, &aF, sizeof f);
  memcpy(m, &aM, sizeof m);
  memcpy(a, &aA, sizeof a);
  for (int i = 0; i < N; i++) {
    f[i] = Fma(f[i], m[i], a[i]);
  }
  F out;
  memcpy(&out, f, sizeof f);
  return out;
}

// The model of RCPPS / RSQRTPS on the lanes of any vector type F of floats, of the
// vendor of the current mode.
template <typename F>
static inline F RcpLanes(F aV) {
  constexpr int N = int(sizeof(F) / sizeof(float));
  const int mode = Mode();
  const uint32_t* table = kRcpByMode[mode];
  const int k = kRcpKByMode[mode];
  uint32_t w[N];
  memcpy(w, &aV, sizeof w);
  for (int i = 0; i < N; i++) {
    w[i] = RcpBits(w[i], table, k);
  }
  F out;
  memcpy(&out, w, sizeof w);
  return out;
}
template <typename F>
static inline F RsqLanes(F aV) {
  constexpr int N = int(sizeof(F) / sizeof(float));
  const int mode = Mode();
  const uint32_t* table = kRsqByMode[mode];
  const int k = kRsqKByMode[mode];
  uint32_t w[N];
  memcpy(w, &aV, sizeof w);
  for (int i = 0; i < N; i++) {
    w[i] = RsqBits(w[i], table, k);
  }
  F out;
  memcpy(&out, w, sizeof w);
  return out;
}

}  // namespace StealthSkiaMath

#endif  // STEALTH_SKIA_MATH_H_
