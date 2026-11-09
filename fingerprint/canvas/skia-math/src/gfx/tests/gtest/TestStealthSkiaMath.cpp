/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The 2D canvas gives the same bytes on every CPU (StealthSkiaMath.h, engine objective 10).
//
// Measured in invisible_skia_cpu (canvas-blend/skia-cpu-dispatch-m149): Skia's raster
// pipeline on a CPU with AVX2 and on one without it gives different bytes in 26 of 182
// canvas cases, and on two CPUs with AVX2 the estimate of RCPPS differs by vendor. These
// tests hold the three things the engine declares in their place:
//
//  1. the model of RCPPS and RSQRTPS of a vendor equals the instruction of a CPU of that
//     vendor for every positive input: the SHA-256 of the outputs for the 0x7f800000 bit
//     patterns below infinity is the one measured on the hardware (the Intel hashes on an
//     Intel Core Ultra 9 275HX, and the same on 16 Intel CPUs from Westmere to Arrow Lake;
//     the AMD hashes on 5 AMD CPUs from Zen 1 to Zen 5). The AMD test is skipped only while
//     the AMD table is not in the source (kAmdTablePresent);
//  2. StealthSkiaMath::Fma is std::fma, bit for bit;
//  3. the pixels: the cases of the battery that gave different bytes on CPUs with and
//     without AVX2 give the bytes of the Intel CPU (the "A" column of the measurement) with
//     the engine on in Intel mode, through the dispatched pipeline and through the baseline
//     (SSE2) pipeline forced in place of it.
//
// Before the change the baseline pipeline gave the "B" bytes of the measurement (the
// Newton-refined reciprocal, unfused mad) in all these cases, and the dispatched pipeline
// gave the bytes of the host's own RCPPS.

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "skia/include/core/SkGraphics.h"
#include "skia/src/core/SkOpts.h"
#include "skia/src/opts/StealthSkiaMath.h"

#include "TestStealthSkiaMathCases.h"

#ifndef STEALTH_SKIA_MATH_STANDALONE
#  include "mozilla/Preferences.h"
#endif

namespace {

// ---------------------------------------------------------------------------- SHA-256

class Sha256 {
 public:
  Sha256() {
    static const uint32_t kInit[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(mH, kInit, sizeof mH);
  }
  void Update(const uint8_t* aData, size_t aLength) {
    mLength += aLength;
    while (aLength) {
      if (mFill == 0 && aLength >= 64) {
        Block(aData);
        aData += 64;
        aLength -= 64;
        continue;
      }
      const size_t k = std::min<size_t>(64 - mFill, aLength);
      memcpy(mBuf + mFill, aData, k);
      mFill += k;
      aData += k;
      aLength -= k;
      if (mFill == 64) {
        Block(mBuf);
        mFill = 0;
      }
    }
  }
  std::string Hex() {
    const uint64_t bits = mLength * 8;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (mFill != 56) {
      Update(&zero, 1);
    }
    uint8_t len[8];
    for (int i = 0; i < 8; i++) {
      len[i] = uint8_t(bits >> (56 - 8 * i));
    }
    Update(len, 8);
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 8; i++) {
      for (int s = 28; s >= 0; s -= 4) {
        out.push_back(kDigits[(mH[i] >> s) & 15]);
      }
    }
    return out;
  }

 private:
  static uint32_t Ror(uint32_t x, int k) { return (x >> k) | (x << (32 - k)); }
  void Block(const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
      w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
             (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
    }
    for (int i = 16; i < 64; i++) {
      const uint32_t s0 = Ror(w[i - 15], 7) ^ Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = Ror(w[i - 2], 17) ^ Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = mH[0], b = mH[1], c = mH[2], d = mH[3], e = mH[4], f = mH[5], g = mH[6],
             h = mH[7];
    for (int i = 0; i < 64; i++) {
      const uint32_t S1 = Ror(e, 6) ^ Ror(e, 11) ^ Ror(e, 25);
      const uint32_t t1 = h + S1 + ((e & f) ^ (~e & g)) + K[i] + w[i];
      const uint32_t S0 = Ror(a, 2) ^ Ror(a, 13) ^ Ror(a, 22);
      const uint32_t t2 = S0 + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    mH[0] += a;
    mH[1] += b;
    mH[2] += c;
    mH[3] += d;
    mH[4] += e;
    mH[5] += f;
    mH[6] += g;
    mH[7] += h;
  }

  uint32_t mH[8];
  uint64_t mLength = 0;
  uint8_t mBuf[64];
  size_t mFill = 0;
};

TEST(StealthSkiaMathSha, KnownAnswers) {
  Sha256 empty;
  EXPECT_EQ(empty.Hex(), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  Sha256 abc;
  abc.Update(reinterpret_cast<const uint8_t*>("abc"), 3);
  EXPECT_EQ(abc.Hex(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

// ---------------------------------------------------------------------------- 1. the sweeps

constexpr const char* kIntelRcpSweep =
    "851dc5b40655382db292b5d25b34478cd3d80b6319d4e6c91e0ebe182f174010";
constexpr const char* kIntelRsqSweep =
    "4ef608e5ca8d9251592dc9f1d2c00710dd384b1479ecd0ecc7b9976c930e22b1";
constexpr const char* kAmdRcpSweep =
    "531fb130e27bf24dee6991ef2396ceaaa85568079f2f7ba14d1e972a2369d869";
constexpr const char* kAmdRsqSweep =
    "6ac75fe4305cb8531056eb01e565fc389bd931d18dca06bd9e335bae005f330e";

// The SHA-256 of the outputs of the model for every positive input below infinity, in
// order (the exponent 0 to 254, the 2^23 mantissas), each as 4 bytes little endian: the
// hash rcpps_table.c prints for the instruction.
std::string SweepHash(uint32_t (*aModel)(uint32_t, const uint32_t*, int), const uint32_t* aTable,
                      int aK) {
  Sha256 sha;
  constexpr uint32_t kChunk = 1 << 14;
  std::vector<uint8_t> bytes(kChunk * 4);
  for (uint32_t base = 0; base < 0x7f800000u; base += kChunk) {
    for (uint32_t i = 0; i < kChunk; i++) {
      const uint32_t y = aModel(base + i, aTable, aK);
      uint8_t* o = &bytes[4 * i];
      o[0] = uint8_t(y);
      o[1] = uint8_t(y >> 8);
      o[2] = uint8_t(y >> 16);
      o[3] = uint8_t(y >> 24);
    }
    sha.Update(bytes.data(), bytes.size());
  }
  return sha.Hex();
}

void CheckSweeps(int aMode, const char* aRcpHash, const char* aRsqHash) {
  const uint32_t* rcp = StealthSkiaMath::kRcpByMode[aMode];
  const uint32_t* rsq = StealthSkiaMath::kRsqByMode[aMode];
  ASSERT_NE(rcp, nullptr);
  ASSERT_NE(rsq, nullptr);
  const int kRcp = StealthSkiaMath::kRcpKByMode[aMode];
  const int kRsq = StealthSkiaMath::kRsqKByMode[aMode];
  std::string rcpHash, rsqHash;
  std::thread other([&] { rsqHash = SweepHash(StealthSkiaMath::RsqBits, rsq, kRsq); });
  rcpHash = SweepHash(StealthSkiaMath::RcpBits, rcp, kRcp);
  other.join();
  EXPECT_EQ(rcpHash, aRcpHash) << "RCPPS model";
  EXPECT_EQ(rsqHash, aRsqHash) << "RSQRTPS model";
}

TEST(StealthSkiaMath, IntelModelReproducesTheSweep) {
  CheckSweeps(StealthSkiaMath::kIntel, kIntelRcpSweep, kIntelRsqSweep);
}

// The AMD model (k 12 for both instructions, from the dump of a Ryzen 9 7900X; the 5 AMD CPUs
// of the campaign, Zen 1 to Zen 5, share these sweeps). Skipped only while the AMD table is
// not in the source (kAmdTablePresent): gen_stealth_skia_math.py writes it from a dump.
TEST(StealthSkiaMath, AmdModelReproducesTheSweep) {
  if (!StealthSkiaMath::kAmdTablePresent) {
    GTEST_SKIP() << "kAmdTablePresent is false: the AMD table is not in the source yet";
  }
  CheckSweeps(StealthSkiaMath::kAmd, kAmdRcpSweep, kAmdRsqSweep);
}

// The 16 special values rcpps_model.c prints for the instruction of the Intel CPU.
TEST(StealthSkiaMath, IntelModelSpecialValues) {
  struct Special {
    uint32_t x, rcp, rsq;
  };
  static const Special kSpecial[] = {
      {0x00000000, 0x7f800000, 0x7f800000}, {0x80000000, 0xff800000, 0xff800000},
      {0x00000001, 0x7f800000, 0x7f800000}, {0x807fffff, 0xff800000, 0xff800000},
      {0x7f800000, 0x00000000, 0x00000000}, {0xff800000, 0x80000000, 0xffc00000},
      {0x7fc00000, 0x7fc00000, 0x7fc00000}, {0x7f800001, 0x7fc00001, 0x7fc00001},
      {0xffc00000, 0xffc00000, 0xffc00000}, {0xff800001, 0xffc00001, 0xffc00001},
      {0xbf800000, 0xbf7ff000, 0xffc00000}, {0xc0000000, 0xbefff000, 0xffc00000},
      {0xff7fffff, 0x80000000, 0xffc00000}, {0x00800000, 0x7e7ff000, 0x5efff000},
      {0x80800000, 0xfe7ff000, 0xffc00000}, {0x7f7fffff, 0x00000000, 0x1f800800}};
  const uint32_t* rcp = StealthSkiaMath::kRcpByMode[StealthSkiaMath::kIntel];
  const uint32_t* rsq = StealthSkiaMath::kRsqByMode[StealthSkiaMath::kIntel];
  const int kRcp = StealthSkiaMath::kRcpKByMode[StealthSkiaMath::kIntel];
  const int kRsq = StealthSkiaMath::kRsqKByMode[StealthSkiaMath::kIntel];
  for (const Special& s : kSpecial) {
    EXPECT_EQ(StealthSkiaMath::RcpBits(s.x, rcp, kRcp), s.rcp) << std::hex << s.x;
    EXPECT_EQ(StealthSkiaMath::RsqBits(s.x, rsq, kRsq), s.rsq) << std::hex << s.x;
  }
}

// ---------------------------------------------------------------------------- 2. Fma

uint32_t Bits(float f) {
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return u;
}
float FromBits(uint32_t u) {
  float f;
  memcpy(&f, &u, sizeof f);
  return f;
}

// std::fma and Fma are the same bits, or both are NaN (the payload and the sign of a NaN
// that a fused instruction returns depend on the operand order the compiler picked).
bool SameFma(float a, float b, float c) {
  const float want = fmaf(a, b, c);
  const float got = StealthSkiaMath::Fma(a, b, c);
  if (isnan(want) || isnan(got)) {
    return isnan(want) && isnan(got);
  }
  return Bits(want) == Bits(got);
}

struct XorShift {
  uint64_t s = 0x9e3779b97f4a7c15ull;
  uint64_t Next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
};

// A float with the given sign and mantissa bits and a biased exponent in [lo, lo + span).
float Make(uint64_t aBits, uint32_t aLo, uint32_t aSpan, uint32_t aMantissaMask = 0x807fffffu) {
  return FromBits((uint32_t(aBits) & aMantissaMask) | ((aLo + uint32_t(aBits >> 32) % aSpan) << 23));
}

TEST(StealthSkiaMath, FmaEqualsStdFmaOnRandomCases) {
  XorShift rng;
  constexpr int kCases = 4 * 1000 * 1000;
  int bad = 0;
  for (int i = 0; i < kCases && bad < 20; i++) {
    float a, b, c;
    const uint64_t r1 = rng.Next(), r2 = rng.Next(), r3 = rng.Next();
    switch (i & 7) {
      case 0:  // any bit pattern (NaNs and infinities included, rarely)
        a = FromBits(uint32_t(r1));
        b = FromBits(uint32_t(r2));
        c = FromBits(uint32_t(r3));
        break;
      case 1:  // moderate magnitudes: the products and sums that pixels make
        a = Make(r1, 120, 16);
        b = Make(r2, 120, 16);
        c = Make(r3, 115, 32);
        break;
      case 2:  // cancellation: c is the product, rounded, and a few ulps away
        a = Make(r1, 100, 55);
        b = Make(r2, 100, 55);
        c = FromBits(Bits(-(a * b)) + uint32_t(int32_t(r3 % 7) - 3));
        break;
      case 3:  // exact cancellation neighbours of the other sign and of the same
        a = Make(r1, 110, 16);
        b = Make(r2, 110, 16);
        c = (r3 & 1) ? -(a * b) : a * b;
        break;
      case 4:  // results in the denormal range
        a = Make(r1, 40, 64);
        b = Make(r2, 40, 64);
        c = FromBits(uint32_t(r3) & 0x807fffffu);  // a denormal or zero
        break;
      case 5:  // results near the overflow
        a = Make(r1, 190, 64);
        b = Make(r2, 190, 64);
        c = Make(r3, 200, 55);
        break;
      case 6:  // c far below the product: the sticky bit of the rounding to odd
        a = Make(r1, 127, 8);
        b = Make(r2, 127, 8);
        c = Make(r3, 0, 64);
        break;
      default: {  // halfway cases: factors of 13 bits make products of up to 26 bits, a
                  // quarter of them exactly between two floats, plus a c too small for a
                  // double to keep
        a = Make(r1, 124, 8, 0x807ff800u);
        b = Make(r2, 124, 8, 0x807ff800u);
        const double p = double(a) * double(b);
        uint64_t pbits;
        memcpy(&pbits, &p, sizeof pbits);
        const int ep = int((pbits >> 52) & 0x7ff) - 1023;
        const int ec = ep - 52 - int((r3 >> 32) % 32);
        c = FromBits((uint32_t(r3) & 0x807fffffu) | (uint32_t(ec + 127) << 23));
        break;
      }
    }
    if (!SameFma(a, b, c)) {
      bad++;
      ADD_FAILURE() << std::hex << "fma(" << Bits(a) << ", " << Bits(b) << ", " << Bits(c)
                    << ") = " << Bits(StealthSkiaMath::Fma(a, b, c)) << ", std::fma "
                    << Bits(fmaf(a, b, c));
    }
  }
  EXPECT_EQ(bad, 0);
}

TEST(StealthSkiaMath, FmaEqualsStdFmaOnTheEdgeCases) {
  static const uint32_t kValues[] = {
      0x00000000, 0x80000000,  // +0, -0
      0x00000001, 0x80000001,  // the smallest denormals
      0x007fffff, 0x807fffff,  // the largest denormals
      0x00800000, 0x80800000,  // the smallest normals
      0x33800000,              // 2^-24
      0x3f7fffff,              // 1 - 2^-24
      0x3f800000, 0xbf800000,  // 1, -1
      0x3f800001, 0xbf800001,  // 1 + 2^-23
      0x40000000,              // 2
      0x4b000000,              // 2^23
      0x4b800000,              // 2^24
      0x5f800000,              // 2^64
      0x1f800000,              // 2^-64
      0x7f7fffff, 0xff7fffff,  // +-FLT_MAX
      0x7f000000,              // 2^127
      0x7f800000, 0xff800000,  // +-infinity
      0x7fc00000, 0x7f800001   // a quiet and a signalling NaN
  };
  int bad = 0;
  for (uint32_t x : kValues) {
    for (uint32_t y : kValues) {
      for (uint32_t z : kValues) {
        const float a = FromBits(x), b = FromBits(y), c = FromBits(z);
        if (!SameFma(a, b, c)) {
          bad++;
          ADD_FAILURE() << std::hex << "fma(" << x << ", " << y << ", " << z << ") = "
                        << Bits(StealthSkiaMath::Fma(a, b, c)) << ", std::fma "
                        << Bits(fmaf(a, b, c));
        }
      }
    }
  }
  EXPECT_EQ(bad, 0);
}

// ---------------------------------------------------------------------------- 3. pixels

// The engine on, in the vendor's mode (the prefs a launched browser has), and off again.
class EngineOn {
 public:
  explicit EngineOn(const char* aVendor) {
#ifdef STEALTH_SKIA_MATH_STANDALONE
    StealthSkiaMath::SetModeForTest(strcmp(aVendor, "amd") == 0 ? StealthSkiaMath::kAmd
                                                               : StealthSkiaMath::kIntel);
#else
    mozilla::Preferences::SetInt("zoom.stealth.fpp.hw_seed", 7);
    mozilla::Preferences::SetCString("zoom.stealth.cpu.vendor", aVendor);
    StealthSkiaMath::ReloadForTest();
#endif
  }
  ~EngineOn() {
#ifdef STEALTH_SKIA_MATH_STANDALONE
    StealthSkiaMath::SetModeForTest(StealthSkiaMath::kOff);
#else
    mozilla::Preferences::ClearUser("zoom.stealth.fpp.hw_seed");
    mozilla::Preferences::ClearUser("zoom.stealth.cpu.vendor");
    StealthSkiaMath::ReloadForTest();
#endif
  }
};

// What the cases that differ between a CPU with AVX2 and one without give on the Intel CPU
// of the measurement (the sha256_A_mozilla column of its results.csv), with the engine on in
// Intel mode. The last field marks the cases Skia draws through the platform's sinf and cosf
// (a rotation): their bytes are glibc's, so they run where the libm is glibc's.
struct PixelCase {
  const char* name;
  const char* sha256;
  bool libm;
};
const PixelCase kPixelCases[] = {
    {"blend.solid.colordodge",
     "fb3afdcbabba497a1819e55ec6a92a01d2576e4cd5ba868b8a0d6e2141dab990",
     false},
    {"blend.solid.colorburn",
     "61c889c7d991f78c7cbea5baa2845d218f7a4dce814616d6e040ccbc37a022d2",
     false},
    {"blend.solid.hue",
     "33b462ed44ea1c3e7f13d4b8ec1108847e1c13dd771f2cf6fa4510e36d465c29",
     false},
    {"blend.solid.saturation",
     "fd3cae631cdb92fe992c4431e88cd7781b4e7fca9341564788ddd934fc7103b1",
     false},
    {"blend.solid.color",
     "ad43feb5bf1d1d36ee5814bccf130efeeb60742cdb9d7430d1f474576e6086ed",
     false},
    {"blend.solid.luminosity",
     "fe4869011faee39c404ccf9a0af1b8c706e232c5cbf5e06a435433f22844d812",
     false},
    {"blend.lingrad.colordodge",
     "64b06af83b184d8fba303f491842d4f9c32ba847a5c26e6f5af7334df6eeced0",
     false},
    {"blend.lingrad.colorburn",
     "a6fb9c1d041d24085401079bba4bd672fe0451054f6c1fdafc4a3df121087889",
     false},
    {"blend.lingrad.softlight",
     "cefa66580ac6d66fdde881284f32d1d5ed588e0d2da0a86e795eeff31f5aee5e",
     false},
    {"blend.lingrad.hue",
     "22abbc2e8cf95717590d3b7271c876c9091d722bdc56829086fc8e967677c89c",
     false},
    {"blend.lingrad.saturation",
     "8d34407bda0fdb82035baf46e0cbd5f9775c39fecb3fa33ffa8fdf3cd888c810",
     false},
    {"blend.lingrad.color",
     "c57f1ba53b562c5687ab69dad851241c410ea18cf9dcb8f8c1c1b267dc0d42da",
     false},
    {"blend.lingrad.luminosity",
     "31aba77e4324ace726f7602dc8fe23cc7e1863568c89271e68137b3be78e3ce8",
     false},
    {"blend.image.softlight",
     "f7daf9cc0f8279290a3182007ecdd75e605503ee552e9f77dbde721a6e52f662",
     false},
    {"blend.image.hue",
     "50208680b47545f40ad443727eaca994115ab665cd8ceb0a0b9de338fbfef4e3",
     false},
    {"blend.image.luminosity",
     "4eb560038d861e94aa79c22380acefef7baa9c89fef99b863c9aca253f805e7d",
     false},
    {"blend.bd0.hue.aa",
     "649567ebb050b1f1418c6518e50195ea2f0e0471b2929f4f532ac7058538ee71",
     false},
    {"blend.bd3.colordodge.aa",
     "9b87d4c2c8fb397f3a26c6fb9d94a20fa3a9f145cb9bbd27dc4e9d0968fa98cd",
     false},
    {"image.rot30.mitchell",
     "ad72bb41d4fbccb972f025ff01f5cca855032472b351e12de7e4bf0c32642f32",
     true},
    {"image.scale2.7.catmull",
     "7ac26e9a3c08ae42ad1b754a0a4ed44d0b95d7c54c1765d376fd7dcbdf845272",
     false},
    {"image.rot30.catmull",
     "ba2d4d7b4b9c0e6f779e07b81759750f1a3b2f51edaf1d53ea181b781a6265c3",
     true},
    {"image.frac.paintalpha.catmull",
     "95ccf74ee0facfbcf9c2705a91311930b876b9fe004be1bc29989c9ef562859d",
     false},
    {"image.rot30.mip",
     "ddbabe70318ec754657cb8d07dfcb337291a19d746969e97ff94f86cb8213ce6",
     true},
    {"image.shader.repeat.rot",
     "c3c9ae251266c55f6e7df0f7736c4ff69fa7efbcc7987f7c34479859ff7150e5",
     true},
    {"image.shader.clamp.bicubic",
     "77ed00ec9547f3563afec7fd425d4f15fa0fa716181430fe1aba5ce655c3b1f5",
     false},
    {"image.a8.color.bilinear",
     "716fb051743d001324502cbf53161278df1678973f336ee5cbd8b3ac3049befe",
     false},
};

#if defined(__GLIBC__)
constexpr bool kLibmIsMeasured = true;
#else
constexpr bool kLibmIsMeasured = false;
#endif

std::string DrawAndHash(const StealthSkiaMathCases::Case& aCase) {
  using namespace StealthSkiaMathCases;
  SkBitmap bm;
  bm.allocPixels(SkImageInfo::Make(W, W, kBGRA_8888_SkColorType, kPremul_SkAlphaType), W * 4);
  fillBackdrop(bm, aCase.backdrop);
  {
    SkSurfaceProps props;
    SkCanvas canvas(bm, props);
    aCase.draw(&canvas);
  }
  Sha256 sha;
  for (int y = 0; y < W; y++) {
    sha.Update(reinterpret_cast<const uint8_t*>(bm.getAddr32(0, y)), W * 4);
  }
  return sha.Hex();
}

// SkOpts' pipeline pointers as the process started with them, before SkOpts::Init put the
// AVX2 ones in their place: the baseline (SSE2) raster pipeline of this build. Taken when
// this file's statics are initialised, which is before any SkGraphics::Init of a test.
struct OptsSnapshot {
  size_t lowpStride = 0, highpStride = 0;
  SkOpts::StageFn highp[sizeof SkOpts::ops_highp / sizeof SkOpts::ops_highp[0]] = {};
  SkOpts::StageFn lowp[sizeof SkOpts::ops_lowp / sizeof SkOpts::ops_lowp[0]] = {};
  SkOpts::StageFn justReturnHighp = nullptr, justReturnLowp = nullptr;
  decltype(SkOpts::start_pipeline_highp) startHighp = nullptr;
  decltype(SkOpts::start_pipeline_lowp) startLowp = nullptr;
};
OptsSnapshot TakeSnapshot() {
  OptsSnapshot s;
  s.lowpStride = SkOpts::raster_pipeline_lowp_stride;
  s.highpStride = SkOpts::raster_pipeline_highp_stride;
  memcpy(s.highp, SkOpts::ops_highp, sizeof s.highp);
  memcpy(s.lowp, SkOpts::ops_lowp, sizeof s.lowp);
  s.justReturnHighp = SkOpts::just_return_highp;
  s.justReturnLowp = SkOpts::just_return_lowp;
  s.startHighp = SkOpts::start_pipeline_highp;
  s.startLowp = SkOpts::start_pipeline_lowp;
  return s;
}
void Install(const OptsSnapshot& s) {
  SkOpts::raster_pipeline_lowp_stride = s.lowpStride;
  SkOpts::raster_pipeline_highp_stride = s.highpStride;
  memcpy(SkOpts::ops_highp, s.highp, sizeof s.highp);
  memcpy(SkOpts::ops_lowp, s.lowp, sizeof s.lowp);
  SkOpts::just_return_highp = s.justReturnHighp;
  SkOpts::just_return_lowp = s.justReturnLowp;
  SkOpts::start_pipeline_highp = s.startHighp;
  SkOpts::start_pipeline_lowp = s.startLowp;
}
const OptsSnapshot gBaseline = TakeSnapshot();

// The hash of every case of kPixelCases this libm can run, by name.
std::map<std::string, std::string> HashAllPixelCases() {
  StealthSkiaMathCases::gCases.clear();
  StealthSkiaMathCases::build();
  std::map<std::string, std::string> out;
  for (const PixelCase& want : kPixelCases) {
    if (want.libm && !kLibmIsMeasured) {
      continue;
    }
    for (const auto& c : StealthSkiaMathCases::gCases) {
      if (c.name == want.name) {
        out[want.name] = DrawAndHash(c);
      }
    }
  }
  return out;
}

// Whether the baseline (SSE2) pipeline of this build was captured before SkOpts::Init.
bool BaselineAvailable() {
#if defined(__x86_64__) || defined(_M_X64)
  return gBaseline.startHighp && gBaseline.highpStride == 4;
#else
  return false;
#endif
}

void CheckPixels() {
  StealthSkiaMathCases::gCases.clear();
  StealthSkiaMathCases::build();
  int checked = 0;
  for (const PixelCase& want : kPixelCases) {
    if (want.libm && !kLibmIsMeasured) {
      continue;
    }
    const StealthSkiaMathCases::Case* found = nullptr;
    for (const auto& c : StealthSkiaMathCases::gCases) {
      if (c.name == want.name) {
        found = &c;
      }
    }
    ASSERT_NE(found, nullptr) << want.name;
    EXPECT_EQ(DrawAndHash(*found), want.sha256) << want.name;
    checked++;
  }
  EXPECT_GE(checked, 22);
}

TEST(StealthSkiaMath, PixelsOfTheIntelCpuThroughTheDispatchedPipeline) {
  SkGraphics::Init();
  EngineOn on("intel");
  CheckPixels();
}

// The pipeline a CPU without AVX2 runs, forced in place of the dispatched one: the same
// bytes as the AVX2 pipeline gives on the Intel CPU.
TEST(StealthSkiaMath, PixelsOfTheIntelCpuThroughTheBaselinePipeline) {
#if defined(__x86_64__) || defined(_M_X64)
  if (!gBaseline.startHighp || gBaseline.highpStride != 4) {
    GTEST_SKIP() << "the baseline pipeline was not captured before SkOpts::Init (highp stride "
                 << gBaseline.highpStride << ")";
  }
  SkGraphics::Init();
  const OptsSnapshot dispatched = TakeSnapshot();
  Install(gBaseline);
  {
    EngineOn on("intel");
    CheckPixels();
  }
  Install(dispatched);
#else
  GTEST_SKIP() << "the baseline pipeline is SSE2: x86-64 only";
#endif
}

// The AMD mode: the pipeline a CPU without AVX2 runs and the AVX2 one compute the same model, so
// they give the same bytes, and the model is not Intel's (12 bits of estimate against 11), so
// the cases that start from RCPPS give other bytes than the Intel ones.
//
// There are no golden hashes of an AMD CPU yet, and none is made up here: the bytes a real
// Ryzen gives for these cases are to be taken with the driver of the measurement
// (skia-cpu-dispatch-m149) on a Ryzen and put in a table like kPixelCases, which then pins
// them. Until then this asserts what follows from the model alone.
TEST(StealthSkiaMath, PixelsOfTheAmdModelAgreeBetweenPipelines) {
  if (!StealthSkiaMath::kAmdTablePresent) {
    GTEST_SKIP() << "kAmdTablePresent is false: the AMD table is not in the source yet";
  }
  SkGraphics::Init();
  std::map<std::string, std::string> dispatched;
  {
    EngineOn on("amd");
    dispatched = HashAllPixelCases();
  }
  ASSERT_FALSE(dispatched.empty());
  if (BaselineAvailable()) {
    const OptsSnapshot current = TakeSnapshot();
    Install(gBaseline);
    std::map<std::string, std::string> baseline;
    {
      EngineOn on("amd");
      baseline = HashAllPixelCases();
    }
    Install(current);
    for (const auto& kv : dispatched) {
      EXPECT_EQ(baseline[kv.first], kv.second) << kv.first << ": baseline and dispatched pipelines differ";
    }
  } else {
    GTEST_LOG_(WARNING) << "the baseline pipeline was not captured before SkOpts::Init: only the dispatched one is compared";
  }
  int differFromIntel = 0;
  for (const PixelCase& intel : kPixelCases) {
    const auto it = dispatched.find(intel.name);
    if (it != dispatched.end() && it->second != intel.sha256) {
      differFromIntel++;
    }
  }
  EXPECT_GT(differFromIntel, 0) << "the AMD model gave the Intel bytes in every case";
}

#ifndef STEALTH_SKIA_MATH_STANDALONE
// The mode of the process comes from the declaration, once, and with the engine off it is
// off.
TEST(StealthSkiaMath, ModeFollowsTheDeclaration) {
  StealthSkiaMath::ReloadForTest();
  EXPECT_EQ(StealthSkiaMath::Mode(), StealthSkiaMath::kOff);
  {
    EngineOn on("intel");
    EXPECT_EQ(StealthSkiaMath::Mode(), StealthSkiaMath::kIntel);
    // Remembered: a pipeline stage never reads a pref again.
    mozilla::Preferences::SetCString("zoom.stealth.cpu.vendor", "something else");
    EXPECT_EQ(StealthSkiaMath::Mode(), StealthSkiaMath::kIntel);
  }
  EXPECT_EQ(StealthSkiaMath::Mode(), StealthSkiaMath::kOff);
  if (StealthSkiaMath::kAmdTablePresent) {
    EngineOn on("amd");
    EXPECT_EQ(StealthSkiaMath::Mode(), StealthSkiaMath::kAmd);
  }
}
#endif

}  // namespace
