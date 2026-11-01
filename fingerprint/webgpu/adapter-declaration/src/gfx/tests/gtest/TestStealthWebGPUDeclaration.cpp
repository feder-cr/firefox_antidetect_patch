/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// The parsers of the declared WebGPU adapter (StealthWebGPUDeclaration.h).
// A declaration that leaves a limit out, or names something Firefox does not
// have, must be refused: anything it did not set would keep the host's value,
// which is the leak the declaration exists against.

#include "gtest/gtest.h"
#include "mozilla/Preferences.h"
#include "mozilla/ScopeExit.h"
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
nsDependentCSubstring Key(const Limit aLimit) {
  switch (aLimit) {
#define STEALTH_KEY(X, S) \
  case Limit::X: \
    return nsDependentCSubstring(S, sizeof(S) - 1);
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
  return nsDependentCSubstring();
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

#ifdef XP_WIN

// The adapter on a Windows host without a GPU (gfx/wgpu_bindings/src/
// stealth_webgpu_adapter.rs). The CI Windows runner has no GPU: the server's
// search for the compositor's D3D12 adapter finds nothing there, and every
// requestAdapter() resolved null where retail on the declared GPU has one.
// Results: 0 null, 1 an adapter of device type Cpu (WARP), 2 another
// adapter, 4 the found adapter left alone.
extern "C" uint32_t stealth_wgpu_adapter_after_search(bool aFound);

namespace {
constexpr char kSeedPref[] = "zoom.stealth.fpp.hw_seed";
}

TEST(StealthWebGPUAdapter, NoCompositorAdapterIsTheSoftwareOne)
{
  auto restore = MakeScopeExit([] { Preferences::ClearUser(kSeedPref); });
  Preferences::SetInt(kSeedPref, 7);
  EXPECT_EQ(stealth_wgpu_adapter_after_search(false), 1u);
}

TEST(StealthWebGPUAdapter, EngineOffKeepsMozillasNull)
{
  Preferences::ClearUser(kSeedPref);
  EXPECT_EQ(stealth_wgpu_adapter_after_search(false), 0u);
}

TEST(StealthWebGPUAdapter, AFoundCompositorAdapterIsLeftAlone)
{
  auto restore = MakeScopeExit([] { Preferences::ClearUser(kSeedPref); });
  Preferences::SetInt(kSeedPref, 7);
  EXPECT_EQ(stealth_wgpu_adapter_after_search(true), 4u);
}

// The server's wgpu instance without the host's WGPU_* variables
// (gfx/wgpu_bindings/src/stealth_webgpu_env.rs). Stock Firefox reads seven of
// them into the instance flags; with the engine on, the flags are the ones
// built with no variable set, whatever the host's environment says.
extern "C" uint64_t stealth_wgpu_instance_flags(bool aWithEnv);

TEST(StealthWebGPUInstanceEnv, TheHostsVariablesDoNotReachTheInstance)
{
  auto restore = MakeScopeExit([] { Preferences::ClearUser(kSeedPref); });
  Preferences::ClearUser(kSeedPref);
  const uint64_t stock = stealth_wgpu_instance_flags(false);
  // Stock reads the environment: the test's variables do change the flags.
  EXPECT_NE(stealth_wgpu_instance_flags(true), stock);
  Preferences::SetInt(kSeedPref, 7);
  // Engine on: the same flags as stock with no variable set, so our copy of
  // Mozilla's expression is still Mozilla's.
  EXPECT_EQ(stealth_wgpu_instance_flags(true), stock);
  EXPECT_EQ(stealth_wgpu_instance_flags(false), stock);
}

#endif  // XP_WIN

#if defined(XP_LINUX) && !defined(ANDROID) && defined(__x86_64__)

#  include <dlfcn.h>

#  include <fstream>
#  include <string>

// WebGPU's Vulkan driver on Linux is ours (scripts/ci/lavapipe_build.py), in
// dist/bin/lavapipe/ next to the executable, mapped under the loader's name
// (StealthWebGPUPreloadBundledVulkan). Before it, requestAdapter() answered
// null on a host without a Vulkan driver and the host's Mesa elsewhere. x86_64
// only: arm64 ships without it and WebGPU answers null there.

extern "C" uint32_t stealth_wgpu_vulkan_adapter(uint8_t* aName,
                                                size_t aNameLen,
                                                const ffi::WGPULimits* aDeclared,
                                                uint32_t* aBadLimits);

TEST(StealthWebGPUBundledVulkan, TheLoaderNameLeadsToOurDriver)
{
  ASSERT_TRUE(StealthWebGPUPreloadBundledVulkan());
  void* byName = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
  ASSERT_TRUE(byName);
  Dl_info info = {};
  ASSERT_TRUE(dladdr(dlsym(byName, "vkGetInstanceProcAddr"), &info));
  dlclose(byName);
  ASSERT_TRUE(info.dli_fname);
  EXPECT_NE(std::string(info.dli_fname).find("/lavapipe/libvulkan_lvp.so"),
            std::string::npos)
      << info.dli_fname;
}

// The adapter is our lavapipe at the pinned profile (128-bit, whatever this
// runner's CPU), and every limit the declaration gives a page is one it has.
TEST(StealthWebGPUBundledVulkan, TheAdapterIsOursAndHoldsTheDeclaration)
{
  ASSERT_TRUE(StealthWebGPUPreloadBundledVulkan());
  ffi::WGPULimits declared = {};
  ASSERT_TRUE(
      StealthParseWebGPULimits(nsLiteralCString(kRetail), Key, declared));
  uint8_t name[256] = {};
  uint32_t badLimits = 0xffffffff;
  EXPECT_EQ(stealth_wgpu_vulkan_adapter(name, sizeof(name), &declared,
                                        &badLimits),
            1u);
  const std::string adapter(reinterpret_cast<const char*>(name));
  EXPECT_NE(adapter.find("llvmpipe"), std::string::npos) << adapter;
  EXPECT_NE(adapter.find("128 bits"), std::string::npos) << adapter;
  EXPECT_EQ(badLimits, 0u);
}

// No library of the host's Vulkan stack is mapped in this process: the only
// libvulkan anywhere in its maps is ours.
TEST(StealthWebGPUBundledVulkan, NoHostVulkanIsMapped)
{
  ASSERT_TRUE(StealthWebGPUPreloadBundledVulkan());
  std::ifstream maps("/proc/self/maps");
  ASSERT_TRUE(maps.is_open());
  std::string line;
  bool ours = false;
  while (std::getline(maps, line)) {
    if (line.find("libvulkan") == std::string::npos) {
      continue;
    }
    EXPECT_NE(line.find("/lavapipe/libvulkan_lvp.so"), std::string::npos)
        << line;
    ours = true;
  }
  EXPECT_TRUE(ours);
}

#endif  // XP_LINUX && !ANDROID && __x86_64__
