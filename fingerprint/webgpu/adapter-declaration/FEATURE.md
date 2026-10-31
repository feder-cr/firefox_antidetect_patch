# fingerprint/webgpu/adapter-declaration

Category: spoof
Prefs: zoom.stealth.webgpu.adapter, zoom.stealth.webgpu.features, zoom.stealth.webgpu.limits
Test: gfx/tests/gtest/TestStealthWebGPUDeclaration.cpp

## What it does

The WebGPU adapter a page sees is declared by invisible_core, never read from the host. Firefox's
Adapter::Adapter hands the page the features and limits wgpu reports for the real adapter (D3D12 on
Windows, Vulkan on Linux, a CPU adapter on a Linux machine with no GPU), clamped only under
resistFingerprinting. Measured 2026-10-31 against retail Firefox 159 for Windows, headed: Linux
answered isFallbackAdapter true, one extra feature (clip-distances) and 14 different limits. Retail's
13 features and 35 limits were identical for an RTX 5080, an Intel iGPU and WARP on the same machine:
constants of Firefox's D3D12 backend, not of the GPU. So the core declares them once, and "none" for a
GPU without Direct3D 12 (requestAdapter resolves null, as on such a machine). isFallbackAdapter is true
only for an adapter asked for with forceFallbackAdapter, as retail's WARP answers.

What reaches wgpu stays real: RequestDevice reports the limits and features the page asked for, as
Firefox does, but the descriptor sent to wgpu is clamped to the host adapter, so asking for a
declared maximum the host lacks still gives a device.

The hook ceiling of gfx/tests/gtest/moz.build (owned by fingerprint/canvas/noise-projection) went
from 11 to 12 for the one line that registers this folder's gtest.

## Hooks in Mozilla files

- `dom/webgpu/Adapter.cpp` (includes; AdapterInfo::IsFallbackAdapter; a forward declaration of ToJsKey; Adapter::Adapter after the feature loop; Adapter::RequestDevice before wgpu_client_request_device, already-hook): Calls only: the declared features, limits and fallback answer, and the clamp of what is sent to wgpu, all in dom/webgpu/StealthWebGPUDeclaration.cpp. ToJsKey is Adapter.cpp's own static; it is forward-declared so the declaration is keyed by the names the page reads.
- `dom/webgpu/Instance.cpp` (includes; Instance::RequestAdapter before and after wgpu_client_request_adapter, already-hook): A declared GPU without Direct3D 12 resolves requestAdapter with null; otherwise the request notes forceFallbackAdapter for the adapter's isFallbackAdapter.
- `dom/webgpu/moz.build` (after UNIFIED_SOURCES of h_and_cpp, already-hook): Registers our file on lines of its own, outside Mozilla's list; non-unified because it has file-static helpers with short names.

## Our own files

- `dom/webgpu/StealthWebGPUDeclaration.cpp`
- `dom/webgpu/StealthWebGPUDeclaration.h`
- `gfx/tests/gtest/TestStealthWebGPUDeclaration.cpp`

External textures (2026-10-31, the host gate): retail Windows exposes GPUDevice.importExternalTexture
and it works; Linux had no such method, because dom.webgpu.external-texture.enabled defaults to false
off Windows and macOS, and with the pref on gfxPlatform::InitWebGPUConfig force-disables the feature
("not supported on this Operating System"). The pref takes Windows' value on its own line in
StaticPrefList.yaml, and the force-disable is compiled out (one rewritten line each, in
removed-budget.json); off Windows the import runs on the generic shared-memory buffer path.
