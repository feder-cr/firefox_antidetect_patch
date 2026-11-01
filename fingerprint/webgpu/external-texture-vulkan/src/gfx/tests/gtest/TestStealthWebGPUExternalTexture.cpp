/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// External textures on Vulkan, the shader side (wgpu_bindings'
// stealth_external_texture.rs). Before the backport the SPIR-V writer had no
// texture_external: a page's shader aborted the GPU process where D3D12, on
// Windows, compiles it.

#include "gtest/gtest.h"

extern "C" bool stealth_wgpu_external_texture_compiles(uint32_t aIndex);

TEST(StealthWebGPUExternalTexture, SampledLoadedQueriedThroughAParameter)
{
  EXPECT_TRUE(stealth_wgpu_external_texture_compiles(0));
}

TEST(StealthWebGPUExternalTexture, ParameterAndGlobalPassedOn)
{
  EXPECT_TRUE(stealth_wgpu_external_texture_compiles(1));
}

TEST(StealthWebGPUExternalTexture, OnlyForwarded)
{
  EXPECT_TRUE(stealth_wgpu_external_texture_compiles(2));
}

TEST(StealthWebGPUExternalTexture, NoModulePastTheLast)
{
  EXPECT_FALSE(stealth_wgpu_external_texture_compiles(3));
}
