/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_WEBGPU_STEALTHWEBGPUDECLARATION_H_
#define DOM_WEBGPU_STEALTHWEBGPUDECLARATION_H_

// The WebGPU adapter a page sees, with the engine on: DECLARED, never the
// host's. Called from the hooks in Instance.cpp (RequestAdapter) and
// Adapter.cpp (the Adapter constructor, AdapterInfo::IsFallbackAdapter,
// Adapter::RequestDevice).
//
// WHY. Adapter::Adapter hands the page the features and limits wgpu reports
// for the host's adapter, clamped only under resistFingerprinting. That is the
// D3D12 backend on Windows and Vulkan on Linux, and on a Linux machine with no
// GPU it is a CPU adapter. Measured 2026-10-31 against retail Firefox 159 for
// Windows: Linux answered isFallbackAdapter true, one extra feature
// (clip-distances) and 14 different limits (maxStorageBufferBindingSize
// 134217728 against 2147483644, minUniformBufferOffsetAlignment 32 against
// 256, ...). Retail's 13 features and 35 limits are the same for the RTX 5080,
// the Intel iGPU and WARP on the same machine: they are constants of
// Firefox's D3D12 backend, not of the GPU, so invisible_core declares them
// once for every identity that has a D3D12 adapter.
//
// What is declared (zoom.stealth.webgpu.*):
//  - adapter: "d3d12", or "none" for a GPU without Direct3D 12 (a D3D10-class
//    renderer), where retail's navigator.gpu.requestAdapter() resolves null;
//  - features: the GPUFeatureName strings, one per line;
//  - limits: "jsName=value", one per line, every limit Firefox has.
// isFallbackAdapter is what the page asked for: true only with
// forceFallbackAdapter, as retail's WARP adapter answers.
//
// What reaches wgpu stays real. RequestDevice reports the limits and features
// the page asked for, as Firefox does, but the descriptor sent to wgpu is
// clamped to what the host adapter supports (StealthWebGPUClampToReal), so a
// page asking for a declared maximum the host lacks still gets a device.

#include <string_view>

#include "mozilla/Maybe.h"
#include "mozilla/RefPtr.h"
#include "mozilla/webgpu/WebGPUTypes.h"
#include "nsStringFwd.h"
#include "nsTArray.h"

namespace mozilla {
namespace dom {
enum class GPUFeatureName : uint8_t;
}  // namespace dom
namespace webgpu {

class Adapter;
class Instance;
class SupportedFeatures;
enum class Limit : uint8_t;
namespace ffi {
struct WGPUAdapterInformation;
struct WGPUFfiDeviceDescriptor;
struct WGPULimits;
}  // namespace ffi

// Adapter.cpp's own JS name of a limit ("maxBindGroups"), passed in so the
// declaration is keyed by the very names the page reads.
using StealthLimitKey = std::string_view (*)(Limit);

// Instance::RequestAdapter, after Firefox's own validation: true when the
// declared GPU has no WebGPU adapter, and the promise resolves null.
bool StealthWebGPUNoAdapter();

// Instance::RequestAdapter, once the request is made: remembers whether this
// adapter was asked for with forceFallbackAdapter.
void StealthWebGPUNoteRequest(const Instance* aInstance, RawId aAdapter,
                              bool aForceFallback);

// Adapter::Adapter, after Firefox has built features and limits from the
// host's adapter: both are replaced with the declaration. False, and nothing
// touched, when the engine is off.
bool StealthWebGPUDeclareAdapter(Adapter* aAdapter, const Instance* aInstance,
                                 RawId aId, RefPtr<SupportedFeatures>& aFeatures,
                                 ffi::WGPULimits& aLimits, StealthLimitKey aKey);

// AdapterInfo::IsFallbackAdapter: the declared answer, Nothing() when the
// engine is off.
Maybe<bool> StealthWebGPUDeclaredIsFallback(const Adapter* aAdapter);

// Adapter::RequestDevice, just before the descriptor goes to wgpu: every
// "max" limit lowered to the host's, every "min" raised to it, and the
// features cut to those the host has. No-op when the engine is off.
void StealthWebGPUClampToReal(ffi::WGPUFfiDeviceDescriptor& aDesc,
                              const ffi::WGPUAdapterInformation& aReal,
                              StealthLimitKey aKey);

// The parsers, exposed for the gtest. Nothing() on any malformed line.
Maybe<nsTArray<dom::GPUFeatureName>> StealthParseWebGPUFeatures(
    const nsACString& aSpec);
bool StealthParseWebGPULimits(const nsACString& aSpec, StealthLimitKey aKey,
                              ffi::WGPULimits& aOut);

}  // namespace webgpu
}  // namespace mozilla

#endif  // DOM_WEBGPU_STEALTHWEBGPUDECLARATION_H_
