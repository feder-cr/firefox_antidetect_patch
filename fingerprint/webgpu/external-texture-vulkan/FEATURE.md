# fingerprint/webgpu/external-texture-vulkan

Category: backport, spoof
Prefs: none read directly
Test: gtest StealthWebGPUExternalTexture.* (gfx/tests/gtest/TestStealthWebGPUExternalTexture.cpp, calling gfx/wgpu_bindings/src/stealth_external_texture.rs)

## What it does

Every Linux host runs WebGPU on Vulkan, and Windows on D3D12. The vendored wgpu (4f4dc63)
had external textures on D3D12 only: on Linux `GPUDevice.importExternalTexture` existed but
every external texture and every bind group layout with one failed validation
(Features::EXTERNAL_TEXTURE missing), and a `texture_external` in a shader would have
aborted the GPU process at the SPIR-V writer's `unimplemented!()`. A page could tell.

The folder carries upstream wgpu's Vulkan external-texture work ahead of Mozilla
(BACKPORT.json: the merged binding support, the WIP SPIR-V lowering and its
prerequisites, the wgpu-core fixes that come with exposing the feature), with our fixes
to the WIP where it panicked, wrote invalid SPIR-V, or computed otherwise than the HLSL
helpers D3D12 runs (rounding, chroma scaling, clamp, plane-count default).

Upstream code is category `backport`: it is not ours, so it has no hook budget; the
check ties it to the vendored revision in BACKPORT.json and fails when Mozilla vendors
another one, which is when it is dropped (`drop_when`).

Checked outside Firefox on the same code: wgpu's GPU tests on Vulkan (Mesa lavapipe)
371/374 (all 11 external-texture tests run and pass; the 3 failures need dxc), naga's
tests, spirv-val on every external-texture module at SPIR-V 1.0 and 1.3.

## Hooks in Mozilla files

- `Cargo.lock` (the naga-types package's dependencies, hard): naga-types depends on bytemuck from upstream wgpu 66e992c3a (ExternalTextureTransferFunction moved there); the lock says what the vendored Cargo.toml says.
- `gfx/wgpu_bindings/src/lib.rs` (the module list after RawString, already-hook): Declares our module with the function the gtest StealthWebGPUExternalTexture calls; the module is ours.
- `third_party/rust/naga-types/.cargo-checksum.json` (the hashes of the files the backport changes, hard): Cargo verifies vendored crates against these hashes; each changed or added file of the crate has its new sha256.
- `third_party/rust/naga-types/Cargo.toml` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga-types/src/lib.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga-types/src/spv.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/.cargo-checksum.json` (the hashes of the files the backport changes, hard): Cargo verifies vendored crates against these hashes; each changed or added file of the crate has its new sha256.
- `third_party/rust/naga/src/back/hlsl/writer.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/msl/writer.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/spv/block.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/spv/helpers.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/spv/image.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/spv/index.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/spv/mod.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/spv/ray/mod.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/spv/ray/pipeline.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/spv/ray/query.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/back/spv/writer.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/common/mod.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/front/type_gen.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/proc/namer.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/proc/type_methods.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/naga/src/valid/type.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/wgpu-core/.cargo-checksum.json` (the hashes of the files the backport changes, hard): Cargo verifies vendored crates against these hashes; each changed or added file of the crate has its new sha256.
- `third_party/rust/wgpu-core/src/device/resource.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/wgpu-hal/.cargo-checksum.json` (the hashes of the files the backport changes, hard): Cargo verifies vendored crates against these hashes; each changed or added file of the crate has its new sha256.
- `third_party/rust/wgpu-hal/src/vulkan/adapter.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/wgpu-hal/src/vulkan/conv.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/wgpu-hal/src/vulkan/device.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/wgpu-hal/src/vulkan/mod.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.
- `third_party/rust/wgpu-types/.cargo-checksum.json` (the hashes of the files the backport changes, hard): Cargo verifies vendored crates against these hashes; each changed or added file of the crate has its new sha256.
- `third_party/rust/wgpu-types/src/texture/external_texture.rs` (upstream wgpu code (see BACKPORT.json), hard): Upstream wgpu's Vulkan external-texture code, as BACKPORT.json lists it, on the vendored 4f4dc63.

## Our own files

- `gfx/tests/gtest/TestStealthWebGPUExternalTexture.cpp`
- `gfx/wgpu_bindings/src/stealth_external_texture.rs`
- `third_party/rust/naga/src/common/external_texture.rs`
