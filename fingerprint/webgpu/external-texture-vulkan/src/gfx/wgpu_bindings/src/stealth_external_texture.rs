/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! External textures on Vulkan, the shader side: what the gtest
//! StealthWebGPUExternalTexture compiles. Vulkan is the backend of every
//! Linux host, and a page there must have `importExternalTexture` and
//! `texture_external` as the D3D12 backend of Windows has them. The modules
//! are the shapes a page can write: the texture sampled, loaded and queried
//! through a parameter, a parameter only forwarded, a global passed on.

#[cfg(any(windows, all(unix, not(any(target_os = "macos", target_os = "ios")))))]
const MODULES: [&str; 3] = [
    "@group(0) @binding(0) var tex: texture_external;
     @group(0) @binding(1) var samp: sampler;
     fn test(t: texture_external) -> vec4<f32> {
       var a = textureSampleBaseClampToEdge(t, samp, vec2(0.0f));
       var b = textureLoad(t, vec2(0i));
       var c = textureLoad(t, vec2(0u));
       var d = textureDimensions(t);
       return a + b + c + vec2f(d).xyxy;
     }
     @fragment fn fragment_main() -> @location(0) vec4<f32> { return test(tex); }
     @vertex fn vertex_main() -> @builtin(position) vec4<f32> { return test(tex); }
     @compute @workgroup_size(1) fn compute_main() { _ = test(tex); }",
    "@group(0) @binding(0) var tex: texture_external;
     @group(0) @binding(1) var samp: sampler;
     fn ignore(t: texture_external) -> u32 { return 0u; }
     fn use_param(t: texture_external, s: sampler) -> vec4<f32> {
       let a = textureSampleBaseClampToEdge(t, s, vec2(0.5f));
       let b = textureLoad(t, vec2(1i));
       let c = textureLoad(t, vec2(1u));
       let d = textureDimensions(t);
       return a + b + c + vec2f(d).xyxy;
     }
     fn forward(t: texture_external) -> vec4<f32> {
       _ = ignore(t);
       return use_param(t, samp);
     }
     @fragment fn fragment_main() -> @location(0) vec4<f32> {
       return forward(tex) + use_param(tex, samp);
     }",
    "@group(0) @binding(0) var tex: texture_external;
     fn inner(t: texture_external) -> u32 { return 0u; }
     fn outer(t: texture_external) -> u32 { return inner(t); }
     @compute @workgroup_size(1) fn main() { _ = outer(tex); }",
];

/// Compiles module `index` to SPIR-V with the binding map wgpu-hal's Vulkan
/// backend builds for an external texture (three planes and the parameters
/// at bindings 0..=3, the sampler after them). False for an index past the
/// last module or a module the compiler refuses, and on the OSes whose
/// wgpu has no Vulkan backend.
#[no_mangle]
pub extern "C" fn stealth_wgpu_external_texture_compiles(index: u32) -> bool {
    #[cfg(any(windows, all(unix, not(any(target_os = "macos", target_os = "ios")))))]
    return compiles(index);
    #[cfg(not(any(windows, all(unix, not(any(target_os = "macos", target_os = "ios"))))))]
    {
        let _ = index;
        false
    }
}

#[cfg(any(windows, all(unix, not(any(target_os = "macos", target_os = "ios")))))]
fn compiles(index: u32) -> bool {
    use wgc::naga;
    use wgc::naga::back::spv;

    let Some(source) = MODULES.get(index as usize) else {
        return false;
    };
    let Ok(module) = naga::front::wgsl::parse_str(source) else {
        return false;
    };
    let Ok(info) = naga::valid::Validator::new(
        naga::valid::ValidationFlags::all(),
        naga::valid::Capabilities::TEXTURE_EXTERNAL,
    )
    .validate(&module) else {
        return false;
    };
    let mut binding_map = spv::BindingMap::default();
    for (binding, at, external_texture) in [(0, 0, true), (1, 4, false)] {
        binding_map.insert(
            naga::ResourceBinding { group: 0, binding },
            spv::BindingInfo {
                descriptor_set: 0,
                binding: at,
                binding_array_size: None,
                external_texture,
            },
        );
    }
    let options = spv::Options {
        binding_map,
        fake_missing_bindings: false,
        ..Default::default()
    };
    spv::write_vec(&module, &info, &options, None).is_ok_and(|words| !words.is_empty())
}
