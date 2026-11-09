/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// sin, cos, tan, sinf, cosf, tanf, powf, hypot, pow, expf, logf, asinf, acosf, atanf and
// atan2f, as code inside libxul calls them, give ucrtbase.dll's results. On Windows they are ucrtbase; elsewhere they
// are libxul's own hidden definitions (gfx/idwr/libm_ucrt.c), and the last test
// also checks that the names bind inside libxul and are not exported from it.
//
// Expected values: hashes of ucrtbase 10.0.26100.9711's outputs over the input
// domains of invisible_DWriteCore's reimpl/math_check.c, copied from its
// reimpl/math_reference.json (sha256 40e1a9b9...622c3815; the expf, logf, asinf, acosf, atanf and
// atan2f chunks from the same file at sha256 d9b3922b...6799cea5, which keeps the earlier
// hashes unchanged). Each case hashes the
// same items in the same order as math_check, calling the functions by name
// (LibmUcrtCases.h), so a sin and a cos of one value go through whatever the
// compiler makes of them here (on GNU targets, one sincos call).

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <iterator>

#include "gtest/gtest.h"
#include "LibmUcrtCases.h"

#if !defined(XP_WIN)
#  include <dlfcn.h>
#endif

namespace {

struct Chunk {
  int test;
  uint64_t items, chunk;
  uint32_t index;
  uint64_t hash;  // math_reference.json: tests.<name>.chunk_hashes[index]
};

void ExpectChunks(const Chunk* aChunks, size_t aCount) {
  for (size_t i = 0; i < aCount; i++) {
    const Chunk& c = aChunks[i];
    uint64_t lo = c.index * c.chunk;
    uint64_t hi = lo + c.chunk < c.items ? lo + c.chunk : c.items;
    EXPECT_EQ(luc_run(c.test, lo, hi), c.hash)
        << "test " << c.test << " chunk " << c.index << " differs from ucrtbase";
  }
}

}  // namespace

// Whole domains: WebRender's synthetic-italic angles (tanf), Gecko's oblique
// angles (tan), COLRv1 PaintRotate angles (sinf and cosf), WebRender's
// compute_scale grid (hypot, 67 million pairs, through the total of its 17
// chunks as math_check.py combines them).
TEST(IdwrLibm, WholeDomains)
{
  const Chunk kChunks[] = {
      {LUC_TANF_WR, 45569, 45569, 0, 0x821d08f9199ff2dfull},
      {LUC_TAN_GECKO, 46081, 46081, 0, 0x8a1a408d5fffa4cbull},
      {LUC_SINCOSF_COLR, 262145, 262145, 0, 0xb6514cc0c84364caull},
  };
  ExpectChunks(kChunks, std::size(kChunks));

  uint64_t total = 1469598103934665603ull;
  for (uint64_t k = 0; k < 17; k++) {
    uint64_t lo = k << 22;
    uint64_t hi = lo + (1 << 22) < 67125249 ? lo + (1 << 22) : 67125249;
    total = luc_combine(total, luc_run(LUC_HYPOT_WR, lo, hi));
  }
  EXPECT_EQ(total, 0xd7da107db1ddb8b0ull) << "hypot_wr differs from ucrtbase";
}

// Gamma tables (powf), every powf call and every table byte: WebRender GammaLut
// for gammas 1.36-1.43, 1.68-1.71, 1.76-1.83, 2.00-2.07 and 2.16-2.23 (the
// declared 1.4, 1.8 and 2.2, and 1.68, 1.79 and 2.01 where glibc's powf moves a
// byte), every contrast; Skia SkMaskGamma for the keys of 1.4, 1.8 and 2.2.
TEST(IdwrLibm, GammaTables)
{
  const Chunk kChunks[] = {
      {LUC_POWF_WR, 102656, 2048, 17, 0x6462bf691b6ebb7cull},
      {LUC_POWF_WR, 102656, 2048, 21, 0x8998de2f794db46dull},
      {LUC_POWF_WR, 102656, 2048, 22, 0x3eba6172cfeee682ull},
      {LUC_POWF_WR, 102656, 2048, 25, 0x1fcdf73710cf72a7ull},
      {LUC_POWF_WR, 102656, 2048, 27, 0xcff532f1320e8ec1ull},
      {LUC_POWF_SKIA, 65536, 1024, 22, 0xc778eb85775cb5dfull},
      {LUC_POWF_SKIA, 65536, 1024, 28, 0x797c05f323bd93e6ull},
      {LUC_POWF_SKIA, 65536, 1024, 35, 0xef54bd1506eb5a98ull},
  };
  ExpectChunks(kChunks, std::size(kChunks));
}

// Samples of the exhaustive and random runs: every float in [0.5, 2) for tanf,
// sinf and cosf; CSS angles of 2^14 to 2^15 degrees (sin, cos); random doubles
// and pairs over every reduction path.
TEST(IdwrLibm, Samples)
{
  const Chunk kChunks[] = {
      {LUC_TANF, 4294967296ull, 16777216, 63, 0xdfeac40a99c70719ull},
      {LUC_SINF, 4294967296ull, 16777216, 63, 0x73a190ddfd24ad31ull},
      {LUC_COSF, 4294967296ull, 16777216, 63, 0x6cdbe2a8aa24fb91ull},
      {LUC_TAN_RAND, 268435456, 4194304, 0, 0x3dff8e060544effaull},
      {LUC_POWF_RAND, 1073741824, 16777216, 0, 0x241af46b0b5dd980ull},
      {LUC_HYPOT_RAND, 1073741824, 16777216, 0, 0x0946f15f0dea580aull},
      {LUC_SINCOS_CSS, 2466250752ull, 16777216, 134, 0x43c999b6cfc2fe43ull},
      {LUC_SINCOS_RAND, 268435456, 4194304, 0, 0xffb5f96574545910ull},
  };
  ExpectChunks(kChunks, std::size(kChunks));
}

// Chunks of the float domains of the functions CSS calc() reaches (servo style's
// calc.rs runs f32::asin, acos, atan, atan2, ln and exp, i.e. asinf, acosf, atanf,
// atan2f, logf and expf): every float in [0.5, 2) for each (chunk 63), [2, 8) and
// [-128, -32) for expf (64, 194), [16, 32) for logf and atanf (65), [-2, -0.5) for
// asinf and acosf (191), and atan2f(y, 1) and atan2f(1, x) over [0.5, 2). glibc's
// asinf and acosf differ from ucrtbase on half of all floats, logf on one in 8,500
// and expf on one in 22,000.
TEST(IdwrLibm, CalcFunctionDomains)
{
  const Chunk kChunks[] = {
      {LUC_EXPF, 4294967296ull, 16777216, 63, 0x484376a31ef68357ull},
      {LUC_EXPF, 4294967296ull, 16777216, 64, 0xa4e179e4a0e25f40ull},
      {LUC_EXPF, 4294967296ull, 16777216, 194, 0xa79a04deff35b01cull},
      {LUC_LOGF, 4294967296ull, 16777216, 63, 0xb27b6ec137895ae9ull},
      {LUC_LOGF, 4294967296ull, 16777216, 65, 0x0aee9eae055b91b6ull},
      {LUC_ASINF, 4294967296ull, 16777216, 63, 0x9c60f6551f78eb09ull},
      {LUC_ASINF, 4294967296ull, 16777216, 191, 0x5367861752a10089ull},
      {LUC_ACOSF, 4294967296ull, 16777216, 63, 0x7bc3ef757c38856full},
      {LUC_ACOSF, 4294967296ull, 16777216, 191, 0x1fde9420b8b6172aull},
      {LUC_ATANF, 4294967296ull, 16777216, 63, 0xec53f838ddf1625full},
      {LUC_ATANF, 4294967296ull, 16777216, 65, 0xca45e73c5ac4d0f9ull},
      {LUC_ATAN2F_Y, 4294967296ull, 16777216, 63, 0x067994f1e17915ebull},
      {LUC_ATAN2F_X, 4294967296ull, 16777216, 63, 0x749f1075474dd641ull},
  };
  ExpectChunks(kChunks, std::size(kChunks));
}

// The inputs of the wrapper's tests/data/ucrt_css_calc.txt where the CSS calc() text of
// Windows Firefox and of Firefox on glibc differ: asin(0.59423828125) is 36.4583deg,
// acos(0.615234375) 52.031deg, log(1.0966796875) 0.0922872, log(24.4873046875) 3.19816,
// log(31.2861328125) 3.44317 (those five differ on glibc 2.39 and 2.43, one ulp each; then
// three more of the file: glibc 2.43 differs on two of them, logf(16.7880859375) agrees on
// both). The expected bits are ucrtbase 10.0.26100.9711's, measured on Windows.
TEST(IdwrLibm, CalcInputsWherePageTextDiffers)
{
  float (*volatile asinf_p)(float) = &::asinf;
  float (*volatile acosf_p)(float) = &::acosf;
  float (*volatile logf_p)(float) = &::logf;
  struct Case {
    const char* what;
    float (*volatile* fn)(float);
    uint32_t in, want;
  };
  const Case kCases[] = {
      {"asinf(0.59423828125)", &asinf_p, 0x3f182000u, 0x3f22e5c0u},
      {"acosf(0.615234375)", &acosf_p, 0x3f1d8000u, 0x3f687a19u},
      {"logf(1.0966796875)", &logf_p, 0x3f8c6000u, 0x3dbd010cu},
      {"logf(24.4873046875)", &logf_p, 0x41c3e600u, 0x404cae92u},
      {"logf(31.2861328125)", &logf_p, 0x41fa4a00u, 0x405c5cfau},
      {"asinf(-0.27099609375)", &asinf_p, 0xbe8ac000u, 0xbe8c81c9u},
      {"acosf(-0.61669921875)", &acosf_p, 0xbf1de000u, 0x400f0fccu},
      {"logf(16.7880859375)", &logf_p, 0x41864e00u, 0x403485d9u},
  };
  for (const Case& c : kCases) {
    float in, r;
    memcpy(&in, &c.in, 4);
    r = (*c.fn)(in);
    uint32_t got;
    memcpy(&got, &r, 4);
    EXPECT_EQ(got, c.want) << c.what << " differs from ucrtbase";
  }
}

// pow(3803691534.2437644, -3.7239311377732847): ucrtbase gives 0x38866be77bbc275b, glibc
// 2.43 0x38866be77bbc275c (the witness of pow_special, where glibc differs on 10 of 6,401 items).
// Math.pow in js/src/builtin/Math.cpp is std::pow.
TEST(IdwrLibm, PowWitness)
{
  double (*volatile f)(double, double) = &::pow;
  uint64_t xb = 0x41ec56f741c7ccebull, yb = 0xc00dca9c688a56b0ull;
  double x, y;
  memcpy(&x, &xb, 8);
  memcpy(&y, &yb, 8);
  double r = f(x, y);
  uint64_t u;
  memcpy(&u, &r, 8);
  EXPECT_EQ(u, 0x38866be77bbc275bull);
}

// Math.sin(Math.PI / 4), which a page reads: ucrtbase's 0x3fe6a09e667f3bcd
// (0.7071067811865476), where glibc and fdlibm give ...bcc.
TEST(IdwrLibm, PageValue)
{
  double (*volatile f)(double) = &::sin;
  double r = f(0.7853981633974483);
  uint64_t u;
  memcpy(&u, &r, 8);
  EXPECT_EQ(u, 0x3fe6a09e667f3bcdull);
}

#if !defined(XP_WIN)
// The names bind inside libxul, and libxul does not export them. For each name:
// the address this file takes of it (&sin, ...) lies in libxul; and dlsym on
// libxul's own handle, which searches libxul before its dependencies, finds
// either nothing or an address outside libxul (the host libm's copy), never one
// in libxul.
TEST(IdwrLibm, BoundInsideLibxulNotExported)
{
  Dl_info self;
  ASSERT_TRUE(dladdr(reinterpret_cast<void*>(&luc_combine), &self));
  void* xul = dlopen(self.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
  ASSERT_TRUE(xul) << self.dli_fname;

  struct Name {
    const char* name;
    void* address;
  };
  const Name kNames[] = {
      {"sin", reinterpret_cast<void*>(static_cast<double (*)(double)>(&::sin))},
      {"cos", reinterpret_cast<void*>(static_cast<double (*)(double)>(&::cos))},
      {"tan", reinterpret_cast<void*>(static_cast<double (*)(double)>(&::tan))},
      {"sinf", reinterpret_cast<void*>(&::sinf)},
      {"cosf", reinterpret_cast<void*>(&::cosf)},
      {"tanf", reinterpret_cast<void*>(&::tanf)},
      {"powf", reinterpret_cast<void*>(&::powf)},
      {"hypot",
       reinterpret_cast<void*>(static_cast<double (*)(double, double)>(&::hypot))},
      {"pow",
       reinterpret_cast<void*>(static_cast<double (*)(double, double)>(&::pow))},
      {"expf", reinterpret_cast<void*>(&::expf)},
      {"logf", reinterpret_cast<void*>(&::logf)},
      {"asinf", reinterpret_cast<void*>(&::asinf)},
      {"acosf", reinterpret_cast<void*>(&::acosf)},
      {"atanf", reinterpret_cast<void*>(&::atanf)},
      {"atan2f", reinterpret_cast<void*>(&::atan2f)},
  };
  for (const Name& n : kNames) {
    Dl_info where;
    ASSERT_TRUE(dladdr(n.address, &where)) << n.name;
    EXPECT_STREQ(where.dli_fname, self.dli_fname)
        << n.name << " binds outside libxul";
    void* exported = dlsym(xul, n.name);
    if (exported) {
      ASSERT_TRUE(dladdr(exported, &where)) << n.name;
      EXPECT_STRNE(where.dli_fname, self.dli_fname)
          << n.name << " is exported from libxul";
    }
  }
  dlclose(xul);
}
#endif
