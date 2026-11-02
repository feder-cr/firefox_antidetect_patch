/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! The adapter a page gets on a Windows host without a GPU.
//!
//! On Windows the server looks for the D3D12 adapter the compositor uses and,
//! when none matches, answers null with no fallback. On a GPU-less host the
//! compositor's D3D11 device sits on the Basic Render Driver adapter, which
//! wgpu-hal drops on purpose, or there is no compositor device at all: every
//! `requestAdapter()` resolved null there, where retail on the declared GPU
//! returns an adapter. The answer to "is there an adapter" was the host's.
//!
//! When the stealth engine is on and the compositor search found nothing, the
//! adapter is the software D3D12 one (WARP), which exists on every Windows
//! host. Its features and limits are the declared ones like any adapter
//! (dom/webgpu StealthWebGPUDeclaration), and `isFallbackAdapter` stays what
//! the page asked for. A compositor adapter that was found is left alone, and
//! so is a page that asked for the fallback itself (it takes the generic path).

#[cfg(windows)]
extern "C" {
    fn stealth_webgpu_engine_active() -> bool;
}

/// Runs after the server's compositor search in `RequestAdapter`. `result`
/// is that search's outcome: `Some(false)` when it found no adapter.
pub(super) fn settle(
    global: &wgpu_core_remote::global::Global,
    desc: &wgpu_core_remote_types::RequestAdapterOptions,
    adapter_id: wgpu_core_remote_types::id::AdapterId,
    result: &mut Option<bool>,
) {
    #[cfg(windows)]
    {
        if *result != Some(false)
            || desc.force_fallback_adapter
            || !unsafe { stealth_webgpu_engine_active() }
        {
            return;
        }
        let warp = wgpu_core_remote_types::RequestAdapterOptions {
            power_preference: desc.power_preference,
            force_fallback_adapter: true,
        };
        let created = match global.request_adapter(&warp, false, wgt::Backends::DX12, adapter_id)
        {
            Ok(_) => true,
            Err(e) => {
                log::warn!("No software D3D12 adapter either: {e}");
                false
            }
        };
        *result = Some(created);
    }
    #[cfg(not(windows))]
    {
        let _ = (global, desc, adapter_id, result);
    }
}

/// For the gtest StealthWebGPUAdapter: a server built like the browser's,
/// `settle` after a compositor search that did (`found`) or did not find an
/// adapter. 0: null; 1: an adapter of device type Cpu (WARP); 2: another
/// adapter; 3: not Windows; 4: the found adapter, left alone.
#[no_mangle]
pub extern "C" fn stealth_wgpu_adapter_after_search(found: bool) -> u32 {
    #[cfg(windows)]
    {
        let server = super::wgpu_server_new(super::WebGPUParentPtr(core::ptr::null_mut()));
        // SAFETY: wgpu_server_new returns a Box it gave up.
        let server = unsafe { Box::from_raw(server) };
        let adapter_id = wgpu_core_remote_types::id::AdapterId::zip(1, 1);
        let desc = wgpu_core_remote_types::RequestAdapterOptions {
            power_preference: wgt::PowerPreference::None,
            force_fallback_adapter: false,
        };
        let mut result = Some(found);
        settle(&server, &desc, adapter_id, &mut result);
        match result {
            Some(true) if found => 4,
            Some(true) => match server.adapter_get_info(adapter_id).device_type {
                wgt::DeviceType::Cpu => 1,
                _ => 2,
            },
            _ => 0,
        }
    }
    #[cfg(not(windows))]
    {
        let _ = found;
        3
    }
}

/// For the gtest StealthWebGPUBundledVulkan (Linux): a server built like the
/// browser's asks its Vulkan backend for an adapter, after the test mapped the
/// bundled lavapipe (dom/webgpu StealthWebGPUPreloadBundledVulkan). Writes the
/// adapter's name (NUL-terminated, cut to `name_len`), counts in `bad_limits`
/// the declared limits the adapter does not really have (each one a value a
/// page reads and cannot use), writes them as "name declared/has" entries
/// into `bad_names` (NUL-terminated, cut to `bad_names_len`) so a failure says
/// which, and answers the device type: 0 no adapter, 1 Cpu, 2 another type,
/// 3 not Linux.
#[no_mangle]
pub extern "C" fn stealth_wgpu_vulkan_adapter(
    name: *mut u8,
    name_len: usize,
    declared: *const wgt::Limits,
    bad_limits: *mut u32,
    bad_names: *mut u8,
    bad_names_len: usize,
) -> u32 {
    #[cfg(target_os = "linux")]
    {
        let server = super::wgpu_server_new(super::WebGPUParentPtr(core::ptr::null_mut()));
        // SAFETY: wgpu_server_new returns a Box it gave up.
        let server = unsafe { Box::from_raw(server) };
        let adapter_id = wgpu_core_remote_types::id::AdapterId::zip(1, 1);
        let desc = wgpu_core_remote_types::RequestAdapterOptions {
            power_preference: wgt::PowerPreference::None,
            force_fallback_adapter: false,
        };
        if server
            .request_adapter(&desc, false, wgt::Backends::VULKAN, adapter_id)
            .is_err()
        {
            return 0;
        }
        let info = server.adapter_get_info(adapter_id);
        if !name.is_null() && name_len > 0 {
            let bytes = info.name.as_bytes();
            let n = bytes.len().min(name_len - 1);
            // SAFETY: the caller gives a buffer of name_len bytes.
            unsafe {
                core::ptr::copy_nonoverlapping(bytes.as_ptr(), name, n);
                *name.add(n) = 0;
            }
        }
        let real = super::restrict_limits(server.adapter_limits(adapter_id));
        let mut bad = 0u32;
        let mut names = String::new();
        // SAFETY: the caller passes the declared limits it parsed.
        let declared = unsafe { &*declared };
        declared.check_limits_with_fail_fn(&real, false, |limit, wanted, has| {
            log::warn!("declared {limit} = {wanted}, the bundled driver has {has}");
            bad += 1;
            if !names.is_empty() {
                names.push_str("; ");
            }
            names.push_str(&format!("{limit} {wanted}/{has}"));
        });
        // SAFETY: the caller passes a counter.
        unsafe { *bad_limits = bad };
        if !bad_names.is_null() && bad_names_len > 0 {
            let bytes = names.as_bytes();
            let n = bytes.len().min(bad_names_len - 1);
            // SAFETY: the caller gives a buffer of bad_names_len bytes.
            unsafe {
                core::ptr::copy_nonoverlapping(bytes.as_ptr(), bad_names, n);
                *bad_names.add(n) = 0;
            }
        }
        match info.device_type {
            wgt::DeviceType::Cpu => 1,
            _ => 2,
        }
    }
    #[cfg(not(target_os = "linux"))]
    {
        let _ = (name, name_len, declared, bad_limits, bad_names, bad_names_len);
        3
    }
}
