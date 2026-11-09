//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// StealthNvMath.h:
//   The HLSL helpers that give float dot() of two, three or four components the bits of the
//   NVIDIA hardware with IEEE-only arithmetic. OutputHLSL writes them into the shader header
//   when ShCompileOptions::stealthAluNvidia is on and the shader uses one.
//

#ifndef COMPILER_TRANSLATOR_HLSL_STEALTHNVMATH_H_
#define COMPILER_TRANSLATOR_HLSL_STEALTHNVMATH_H_

namespace sh
{

// Text of reimpl/angle_nv/nv_math.hlsl of the invisible_D3D11Core study, verbatim (every
// operation `precise`).
constexpr char kStealthNvMathHLSL[] = R"HLSL(// HLSL helpers that reproduce NVIDIA arithmetic with IEEE float32 only (every operation `precise`).
// Same text on every adapter; bit-exact against the RTX 5080 (see scripts/nv_dot_proto.py).
// fma_nv(a,b,c) = RN(a*b + c), ONE rounding: TwoProd (Dekker/Veltkamp split) + TwoSum + round-to-odd (Boldo-Melquiond).
// Domain: finite values whose products and splits do not overflow (|x| < 2^100) and no denormal intermediates.
precise float2 nv_two_sum(precise float a, precise float b) {
  precise float s = a + b;
  precise float bb = s - a;
  precise float e = (a - (s - bb)) + (b - bb);
  return float2(s, e);
}
precise float2 nv_split(precise float x) {
  precise float t = 4097.0 * x;
  precise float hi = t - (t - x);
  return float2(hi, x - hi);
}
precise float2 nv_two_prod(precise float a, precise float b) {
  precise float p = a * b;
  precise float2 sa = nv_split(a);
  precise float2 sb = nv_split(b);
  precise float e = ((sa.x * sb.x - p) + sa.x * sb.y + sa.y * sb.x) + sa.y * sb.y;
  return float2(p, e);
}
precise float nv_round_to_odd_add(precise float x, precise float y) {
  precise float2 s = nv_two_sum(x, y);
  uint bits = asuint(s.x);
  if (s.y != 0.0 && (bits & 1u) == 0u) {
    bool same_sign = (s.y > 0.0) == (s.x > 0.0);
    bits = same_sign ? bits + 1u : bits - 1u;
  }
  return asfloat(bits);
}
precise float nv_fma(precise float a, precise float b, precise float c) {
  precise float2 u = nv_two_prod(a, b);
  precise float2 t = nv_two_sum(c, u.x);
  precise float v = nv_round_to_odd_add(t.y, u.y);
  precise float z = t.x + v;
  return z;
}
// dot, length(squared part), distance(squared part): one fma chain, left to right, first product rounded.
precise float nv_dot2(precise float2 a, precise float2 b) { precise float t = a.x * b.x; return nv_fma(a.y, b.y, t); }
precise float nv_dot3(precise float3 a, precise float3 b) { precise float t = a.x * b.x; t = nv_fma(a.y, b.y, t); return nv_fma(a.z, b.z, t); }
precise float nv_dot4(precise float4 a, precise float4 b) { precise float t = a.x * b.x; t = nv_fma(a.y, b.y, t); t = nv_fma(a.z, b.z, t); return nv_fma(a.w, b.w, t); }
)HLSL";

}  // namespace sh

#endif  // COMPILER_TRANSLATOR_HLSL_STEALTHNVMATH_H_
