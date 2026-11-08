/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The input domains of invisible_DWriteCore's reimpl/math_check.c, calling the C library by
 * name (sin, cos, tan, sinf, cosf, tanf, powf, hypot) instead of idwr_*: whatever those names
 * bind to in the library that includes this file. Each run of items [lo, hi) of a test returns
 * the FNV-1a 64 hash of its outputs in item order, exactly as math_check prints it without a
 * reference (the Linux leg), so it equals the chunk hashes recorded from ucrtbase.dll in
 * math_reference.json.
 *
 * C and C++. The arithmetic around the calls must not contract (math_check is built with
 * -ffp-contract=off): the pragma below, and x86-64's baseline has no FMA anyway.
 */
#ifndef LIBM_UCRT_CASES_H
#define LIBM_UCRT_CASES_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

enum {
  LUC_TANF_WR, LUC_TAN_GECKO, LUC_POWF_WR, LUC_POWF_SKIA, LUC_HYPOT_WR, LUC_TANF,
  LUC_TAN_RAND, LUC_POWF_RAND, LUC_HYPOT_RAND, LUC_SINCOSF_COLR, LUC_SINF, LUC_COSF,
  LUC_SINCOS_CSS, LUC_SINCOS_RAND
};

typedef struct { uint64_t h; } luc_state;

static inline void luc_hash(luc_state* st, const void* p, size_t n) {
  const uint8_t* b = (const uint8_t*)p;
  while (n--) { st->h ^= *b++; st->h *= 1099511628211ull; }
}
static inline uint32_t luc_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float luc_u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t luc_d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double luc_u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

static inline float luc_powf(luc_state* st, float x, float y) { float r = powf(x, y); luc_hash(st, &r, 4); return r; }
static inline float luc_tanf(luc_state* st, float x) { float r = tanf(x); luc_hash(st, &r, 4); return r; }
static inline float luc_sinf(luc_state* st, float x) { float r = sinf(x); luc_hash(st, &r, 4); return r; }
static inline float luc_cosf(luc_state* st, float x) { float r = cosf(x); luc_hash(st, &r, 4); return r; }
static inline double luc_tan(luc_state* st, double x) { double r = tan(x); luc_hash(st, &r, 8); return r; }
static inline double luc_sin(luc_state* st, double x) { double r = sin(x); luc_hash(st, &r, 8); return r; }
static inline double luc_cos(luc_state* st, double x) { double r = cos(x); luc_hash(st, &r, 8); return r; }
static inline double luc_hypot(luc_state* st, double x, double y) { double r = hypot(x, y); luc_hash(st, &r, 8); return r; }

static inline uint64_t luc_splitmix(uint64_t* s) {
  uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

static inline double luc_rand_double(uint64_t* s) {
  uint64_t r = luc_splitmix(s), k = r >> 61;
  uint64_t m = luc_splitmix(s) & 0x000fffffffffffffull, sign = r & 0x8000000000000000ull;
  uint64_t e;
  switch (k) {
    case 0: return luc_u2d(luc_splitmix(s));
    case 1: e = 1023 - 30 + (r % 31); break;
    case 2: e = 1023 + (r % 4); break;
    case 3: e = 1023 + 4 + (r % 21); break;
    case 4: e = 1023 + 25 + (r % 200); break;
    case 5: e = 1 + (r % 2046); break;
    case 6: e = 1023 - 600 + (r % 1200); break;
    default: e = 1023 - 2 + (r % 3); break;
  }
  return luc_u2d(sign | (e << 52) | m);
}

/* WebRender gamma_lut.rs, line for line */
typedef struct { int kind; float gamma; } luc_wr_space; /* 0 linear, 1 gamma, 2 srgb */
static inline luc_wr_space luc_wr_new(float g) { luc_wr_space s; s.gamma = g; s.kind = g == 1.0f ? 0 : g == 0.0f ? 2 : 1; return s; }
static inline float luc_wr_to_luma(luc_state* st, luc_wr_space s, float l) {
  if (s.kind == 0) return l;
  if (s.kind == 1) return luc_powf(st, l, s.gamma);
  if (l <= 0.04045f) return l / 12.92f;
  return luc_powf(st, (l + 0.055f) / 1.055f, 2.4f);
}
static inline float luc_wr_from_luma(luc_state* st, luc_wr_space s, float l) {
  if (s.kind == 0) return l;
  if (s.kind == 1) return luc_powf(st, l, 1.0f / s.gamma);
  if (l <= 0.0031308f) return l * 12.92f;
  return 1.055f * luc_powf(st, l, 1.0f / 2.4f) - 0.055f;
}
static inline uint8_t luc_wr_round_to_u8(float x) {
  int32_t v;
  float f = floorf(x + 0.5f);
  v = f >= 2147483648.0f ? INT32_MAX : f <= -2147483649.0f ? INT32_MIN : f != f ? 0 : (int32_t)f;
  return (uint8_t)v;
}
static inline uint8_t luc_scale255(uint8_t n, uint8_t base) {
  uint8_t lum, i;
  base = (uint8_t)(base << (8 - n));
  lum = base;
  for (i = n; i < 8; i += n) lum |= base >> i;
  return lum;
}
static inline float luc_apply_contrast(float srca, float contrast) { return srca + ((1.0f - srca) * contrast * srca); }
static inline void luc_wr_build(luc_state* st, uint8_t* table, uint8_t srcI, float contrast, luc_wr_space sp, luc_wr_space dc) {
  float src = (float)srcI / 255.0f, lin_src = luc_wr_to_luma(st, sp, src), dst = 1.0f - src;
  float lin_dst = luc_wr_to_luma(st, dc, dst), adj = contrast * lin_dst, ii = 0.0f;
  int i;
  if (fabsf(src - dst) < (1.0f / 256.0f)) {
    for (i = 0; i < 256; i++, ii += 1.0f) table[i] = luc_wr_round_to_u8(255.0f * luc_apply_contrast(ii / 255.0f, adj));
  } else {
    for (i = 0; i < 256; i++, ii += 1.0f) {
      float srca = luc_apply_contrast(ii / 255.0f, adj), dsta, lin_out, out;
      dsta = 1.0f - srca;
      lin_out = lin_src * srca + dsta * lin_dst;
      out = luc_wr_from_luma(st, dc, lin_out);
      table[i] = luc_wr_round_to_u8(255.0f * ((out - dst) / (src - dst)));
    }
  }
}

/* Skia SkMaskGamma.cpp */
static inline float luc_sk_to_luma(luc_state* st, float g, float l) {
  if (g == 1.0f) return l;
  if (g == 0.0f) return l <= 0.04045f ? l / 12.92f : luc_powf(st, (l + 0.055f) / 1.055f, 2.4f);
  return luc_powf(st, l, g);
}
static inline float luc_sk_from_luma(luc_state* st, float g, float l) {
  if (g == 1.0f) return l;
  if (g == 0.0f) return l <= 0.0031308f ? l * 12.92f : 1.055f * luc_powf(st, l, 1.0f / 2.4f) - 0.055f;
  return luc_powf(st, l, 1.0f / g);
}
static inline uint8_t luc_sk_round_u8(float x) {
  double d = floor((double)x + 0.5);
  float f = (float)d;
  int v;
  if (f != f) v = 2147483647;
  else { if (!(f < 2147483520.0f)) f = 2147483520.0f; if (!(f > -2147483520.0f)) f = -2147483520.0f; v = (int)f; }
  return (uint8_t)v;
}
static inline void luc_sk_build(luc_state* st, uint8_t* table, unsigned srcI, float contrast, float g) {
  float src = (float)srcI / 255.0f, linSrc = luc_sk_to_luma(st, g, src), dst = 1.0f - src;
  float linDst = luc_sk_to_luma(st, g, dst), adj = contrast * linDst, ii = 0.0f;
  int i;
  if (fabs(src - dst) < (1.0f / 256.0f)) {
    for (i = 0; i < 256; ++i, ii += 1.0f) table[i] = luc_sk_round_u8(255.0f * luc_apply_contrast(ii / 255.0f, adj));
  } else {
    for (i = 0; i < 256; ++i, ii += 1.0f) {
      float srca = luc_apply_contrast(ii / 255.0f, adj), dsta = 1.0f - srca;
      float linOut = (linSrc * srca + dsta * linDst);
      float out = luc_sk_from_luma(st, g, linOut);
      table[i] = luc_sk_round_u8(255.0f * ((out - dst) / (src - dst)));
    }
  }
}

/* items [lo, hi) of a test: the hash math_check prints for them */
static inline uint64_t luc_run(int t, uint64_t lo, uint64_t hi) {
  luc_state st = {1469598103934665603ull};
  luc_state* s = &st;
  uint64_t i;
  switch (t) {
    case LUC_TANF:
      for (i = lo; i < hi; i++) luc_tanf(s, luc_u2f((uint32_t)i));
      break;
    case LUC_TANF_WR:
      for (i = lo; i < hi; i++) {
        int16_t ang = (int16_t)((int64_t)i - 22784);
        float deg = (float)ang / 256.0f;
        const float RADS_PER_DEG = 3.14159265358979323846f / 180.0f;
        luc_tanf(s, deg * RADS_PER_DEG);
      }
      break;
    case LUC_TAN_GECKO:
      for (i = lo; i < hi; i++) {
        float angle = (float)((int64_t)i - 23040) / 256.0f;
        const double kRadPerDegree = 2.0 * 3.14159265358979323846 / 360.0;
        luc_tan(s, angle * kRadPerDegree);
      }
      break;
    case LUC_TAN_RAND:
      for (i = lo; i < hi; i++) { uint64_t r = i * 0x632be59bd9b4e019ull; luc_tan(s, luc_rand_double(&r)); }
      break;
    case LUC_POWF_WR:
    case LUC_POWF_SKIA: {
      int wr = t == LUC_POWF_WR;
      for (i = lo; i < hi; i++) {
        uint8_t tables[8][256];
        unsigned k;
        if (wr ? (i & 255) > 100 : (i == 64 * 256)) continue;
        if (wr) {
          unsigned gamma = (unsigned)(i >> 8), contrast = (unsigned)(i & 255);
          float c = (float)contrast / 100.0f, g = (float)gamma / 100.0f;
          luc_wr_space sp = luc_wr_new(g), dc = luc_wr_new(g);
          for (k = 0; k < 8; k++) luc_wr_build(s, tables[k], luc_scale255(3, (uint8_t)k), c, sp, dc);
        } else {
          unsigned gi = (unsigned)(i >> 8), ci = (unsigned)(i & 255);
          float g = (float)gi / 64.0f, c = (float)ci / 255.0f;
          for (k = 0; k < 8; k++) {
            unsigned base = k << 5, lum = base;
            lum |= base >> 3;
            lum |= base >> 6;
            luc_sk_build(s, tables[k], lum, c, g);
          }
        }
        luc_hash(s, tables, sizeof tables);
      }
      break;
    }
    case LUC_POWF_RAND:
      for (i = lo; i < hi; i++) {
        uint64_t q = i * 0x9e3779b97f4a7c15ull + 7, r = luc_splitmix(&q);
        uint32_t x = (uint32_t)r, y = (uint32_t)(r >> 32);
        switch ((r >> 13) & 7) {
          case 0: case 1:
            x &= 0x3fffffffu; x = x > 0x3f800000u ? x - 0x3f800000u : x;
            y = (y & 0x007fffffu) | 0x3f000000u | ((y >> 23 & 1) << 23);
            break;
          case 2: y = (y & 0x807fffffu) | ((0x7au + (y >> 24) % 10u) << 23); break;
          default: break;
        }
        luc_powf(s, luc_u2f(x), luc_u2f(y));
      }
      break;
    case LUC_HYPOT_WR:
      for (i = lo; i < hi; i++) {
        int64_t a2 = (int64_t)(i / 8193) - 4096, b2 = (int64_t)(i % 8193) - 4096;
        float sx = (float)a2 / 1024.0f, ky = (float)b2 / 1024.0f;
        luc_hypot(s, (double)sx, (double)ky);
      }
      break;
    case LUC_HYPOT_RAND:
      for (i = lo; i < hi; i++) {
        uint64_t q = i * 0xd1342543de82ef95ull + 3;
        double x = luc_rand_double(&q), y;
        uint64_t r = luc_splitmix(&q);
        if ((r & 3) == 0) y = luc_u2d((luc_d2u(x) & 0xfff0000000000000ull) + ((r >> 2) % (60ull << 52)) - (30ull << 52) + (luc_splitmix(&q) & 0x000fffffffffffffull));
        else y = luc_rand_double(&q);
        luc_hypot(s, x, y);
      }
      break;
    case LUC_SINF:
      for (i = lo; i < hi; i++) luc_sinf(s, luc_u2f((uint32_t)i));
      break;
    case LUC_COSF:
      for (i = lo; i < hi; i++) luc_cosf(s, luc_u2f((uint32_t)i));
      break;
    case LUC_SINCOSF_COLR:
      for (i = lo; i < hi; i++) {
        float ang = (float)((int64_t)i - 131072) / 16384.0f;
        float theta = -ang * 3.14159265358979323846f;
        luc_sinf(s, theta);
        luc_cosf(s, theta);
      }
      break;
    case LUC_SINCOS_CSS:
      for (i = lo; i < hi; i++) {
        uint32_t b = (uint32_t)(i >> 1) | (uint32_t)((i & 1) << 31);
        const double kRadPerDegree = 2.0 * 3.14159265358979323846 / 360.0;
        double theta = (double)luc_u2f(b) * kRadPerDegree;
        luc_sin(s, theta);
        luc_cos(s, theta);
      }
      break;
    case LUC_SINCOS_RAND:
      for (i = lo; i < hi; i++) {
        uint64_t q = i * 0x2545f4914f6cdd1dull + 5;
        double x = luc_rand_double(&q);
        luc_sin(s, x);
        luc_cos(s, x);
      }
      break;
    default:
      return 0;
  }
  return st.h;
}

/* math_check.py's total: FNV-1a 64 over the chunk hashes, little-endian */
static inline uint64_t luc_combine(uint64_t h, uint64_t chunk) {
  int k;
  for (k = 0; k < 8; k++) { h ^= (chunk >> (8 * k)) & 0xff; h *= 1099511628211ull; }
  return h;
}

#endif
