/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The parsers of the declared WebGPU adapter (StealthWebGPUDeclaration.h).
// A declaration that leaves a limit out, or names something Firefox does not
// have, must be refused: anything it did not set would keep the host's value,
// which is the leak the declaration exists against.

#include <string_view>

#include "gtest/gtest.h"
#include "mozilla/dom/WebGPUBinding.h"
#include "mozilla/webgpu/StealthWebGPUDeclaration.h"
#include "mozilla/webgpu/SupportedLimits.h"
#include "mozilla/webgpu/ffi/wgpu.h"
#include "nsString.h"

using namespace mozilla;
using namespace mozilla::webgpu;

namespace {

// The JS names, as Adapter.cpp's ToJsKey gives them. The test only needs a
// key per limit; using the real names keeps the specs readable.
std::string_view Key(const Limit aLimit) {
  switch (aLimit) {
#define STEALTH_KEY(X, S) \
  case Limit::X: \
    return S;
    STEALTH_KEY(MaxTextureDimension1D, "maxTextureDimension1D")
    STEALTH_KEY(MaxTextureDimension2D, "maxTextureDimension2D")
    STEALTH_KEY(MaxTextureDimension3D, "maxTextureDimension3D")
    STEALTH_KEY(MaxTextureArrayLayers, "maxTextureArrayLayers")
    STEALTH_KEY(MaxBindGroups, "maxBindGroups")
    STEALTH_KEY(MaxBindGroupsPlusVertexBuffers, "maxBindGroupsPlusVertexBuffers")
    STEALTH_KEY(MaxBindingsPerBindGroup, "maxBindingsPerBindGroup")
    STEALTH_KEY(MaxDynamicUniformBuffersPerPipelineLayout,
      "maxDynamicUniformBuffersPerPipelineLayout")
    STEALTH_KEY(MaxDynamicStorageBuffersPerPipelineLayout,
      "maxDynamicStorageBuffersPerPipelineLayout")
    STEALTH_KEY(MaxSampledTexturesPerShaderStage, "maxSampledTexturesPerShaderStage")
    STEALTH_KEY(MaxSamplersPerShaderStage, "maxSamplersPerShaderStage")
    STEALTH_KEY(MaxStorageBuffersInVertexStage, "maxStorageBuffersInVertexStage")
    STEALTH_KEY(MaxStorageBuffersInFragmentStage, "maxStorageBuffersInFragmentStage")
    STEALTH_KEY(MaxStorageBuffersPerShaderStage, "maxStorageBuffersPerShaderStage")
    STEALTH_KEY(MaxStorageTexturesInVertexStage, "maxStorageTexturesInVertexStage")
    STEALTH_KEY(MaxStorageTexturesInFragmentStage, "maxStorageTexturesInFragmentStage")
    STEALTH_KEY(MaxStorageTexturesPerShaderStage, "maxStorageTexturesPerShaderStage")
    STEALTH_KEY(MaxUniformBuffersPerShaderStage, "maxUniformBuffersPerShaderStage")
    STEALTH_KEY(MaxUniformBufferBindingSize, "maxUniformBufferBindingSize")
    STEALTH_KEY(MaxStorageBufferBindingSize, "maxStorageBufferBindingSize")
    STEALTH_KEY(MinUniformBufferOffsetAlignment, "minUniformBufferOffsetAlignment")
    STEALTH_KEY(MinStorageBufferOffsetAlignment, "minStorageBufferOffsetAlignment")
    STEALTH_KEY(MaxVertexBuffers, "maxVertexBuffers")
    STEALTH_KEY(MaxBufferSize, "maxBufferSize")
    STEALTH_KEY(MaxVertexAttributes, "maxVertexAttributes")
    STEALTH_KEY(MaxVertexBufferArrayStride, "maxVertexBufferArrayStride")
    STEALTH_KEY(MaxInterStageShaderVariables, "maxInterStageShaderVariables")
    STEALTH_KEY(MaxColorAttachments, "maxColorAttachments")
    STEALTH_KEY(MaxColorAttachmentBytesPerSample, "maxColorAttachmentBytesPerSample")
    STEALTH_KEY(MaxComputeWorkgroupStorageSize, "maxComputeWorkgroupStorageSize")
    STEALTH_KEY(MaxComputeInvocationsPerWorkgroup, "maxComputeInvocationsPerWorkgroup")
    STEALTH_KEY(MaxComputeWorkgroupSizeX, "maxComputeWorkgroupSizeX")
    STEALTH_KEY(MaxComputeWorkgroupSizeY, "maxComputeWorkgroupSizeY")
    STEALTH_KEY(MaxComputeWorkgroupSizeZ, "maxComputeWorkgroupSizeZ")
    STEALTH_KEY(MaxComputeWorkgroupsPerDimension, "maxComputeWorkgroupsPerDimension")
#undef STEALTH_KEY
  }
  return "";
}

// Retail Firefox 159 for Windows, headed, 2026-10-31 (the D3D12 constants).
const char kRetail[] =
    "maxTextureDimension1D=16384\nmaxTextureDimension2D=16384\n"
    "maxTextureDimension3D=2048\nmaxTextureArrayLayers=2048\n"
    "maxBindGroups=8\nmaxBindGroupsPlusVertexBuffers=24\n"
    "maxBindingsPerBindGroup=4294967295\n"
    "maxDynamicUniformBuffersPerPipelineLayout=8\n"
    "maxDynamicStorageBuffersPerPipelineLayout=4\n"
    "maxSampledTexturesPerShaderStage=64\nmaxSamplersPerShaderStage=64\n"
    "maxStorageBuffersInVertexStage=64\nmaxStorageBuffersInFragmentStage=64\n"
    "maxStorageBuffersPerShaderStage=64\nmaxStorageTexturesInVertexStage=64\n"
    "maxStorageTexturesInFragmentStage=64\n"
    "maxStorageTexturesPerShaderStage=64\nmaxUniformBuffersPerShaderStage=64\n"
    "maxUniformBufferBindingSize=65536\n"
    "maxStorageBufferBindingSize=2147483644\n"
    "minUniformBufferOffsetAlignment=256\nminStorageBufferOffsetAlignment=32\n"
    "maxVertexBuffers=16\nmaxBufferSize=2147483644\nmaxVertexAttributes=30\n"
    "maxVertexBufferArrayStride=2048\nmaxInterStageShaderVariables=31\n"
    "maxColorAttachments=8\nmaxColorAttachmentBytesPerSample=128\n"
    "maxComputeWorkgroupStorageSize=32768\n"
    "maxComputeInvocationsPerWorkgroup=1024\nmaxComputeWorkgroupSizeX=1024\n"
    "maxComputeWorkgroupSizeY=1024\nmaxComputeWorkgroupSizeZ=64\n"
    "maxComputeWorkgroupsPerDimension=65535\n";

}  // namespace

TEST(StealthWebGPUDeclaration, EveryLimitIsSetFromTheDeclaration)
{
  ffi::WGPULimits limits = {};
  ASSERT_TRUE(StealthParseWebGPULimits(nsLiteralCString(kRetail), Key, limits));
  EXPECT_EQ(GetLimit(limits, Limit::MaxBindingsPerBindGroup), 4294967295u);
  EXPECT_EQ(GetLimit(limits, Limit::MinUniformBufferOffsetAlignment), 256u);
  EXPECT_EQ(GetLimit(limits, Limit::MaxComputeWorkgroupSizeZ), 64u);
  EXPECT_EQ(GetLimit(limits, Limit::MaxStorageBufferBindingSize),
            2147483644u);
}

TEST(StealthWebGPUDeclaration, ALimitLeftOutIsRefused)
{
  nsAutoCString spec(kRetail);
  spec.ReplaceSubstring("maxComputeWorkgroupSizeZ=64\n"_ns, ""_ns);
  ffi::WGPULimits limits = {};
  EXPECT_FALSE(StealthParseWebGPULimits(spec, Key, limits));
}

TEST(StealthWebGPUDeclaration, AnUnknownOrRepeatedOrMalformedLimitIsRefused)
{
  ffi::WGPULimits limits = {};
  EXPECT_FALSE(StealthParseWebGPULimits(
      nsAutoCString(nsLiteralCString(kRetail) + "maxNothing=1\n"_ns), Key,
      limits));
  EXPECT_FALSE(StealthParseWebGPULimits(
      nsAutoCString(nsLiteralCString(kRetail) + "maxBindGroups=8\n"_ns), Key,
      limits));
  nsAutoCString bad(kRetail);
  bad.ReplaceSubstring("maxBindGroups=8\n"_ns, "maxBindGroups=eight\n"_ns);
  EXPECT_FALSE(StealthParseWebGPULimits(bad, Key, limits));
  EXPECT_FALSE(StealthParseWebGPULimits(""_ns, Key, limits));
}

TEST(StealthWebGPUDeclaration, FeaturesAreFirefoxsOwnNames)
{
  const auto features = StealthParseWebGPUFeatures(
      "depth-clip-control\nshader-f16\n\ntimestamp-query\n"_ns);
  ASSERT_TRUE(features.isSome());
  EXPECT_EQ(features->Length(), 3u);
  EXPECT_EQ((*features)[1], dom::GPUFeatureName::Shader_f16);
  EXPECT_TRUE(StealthParseWebGPUFeatures("shader-f16\nno-such-feature\n"_ns)
                  .isNothing());
}
