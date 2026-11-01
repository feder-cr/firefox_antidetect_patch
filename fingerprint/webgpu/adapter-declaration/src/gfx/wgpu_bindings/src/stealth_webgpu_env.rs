/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! The WebGPU server's wgpu instance, without the host's environment.
//!
//! `wgpu_server_new` builds its instance flags with `InstanceFlags::with_env()`,
//! which reads seven `WGPU_*` variables of the process environment:
//! `WGPU_VALIDATION`, `WGPU_DEBUG`, `WGPU_DISCARD_HAL_LABELS`,
//! `WGPU_ALLOW_UNDERLYING_NONCOMPLIANT_ADAPTER`, `WGPU_GPU_BASED_VALIDATION`,
//! `WGPU_VALIDATION_INDIRECT_CALL` and `WGPU_STRICT_WEBGPU_COMPLIANCE`. A host
//! value then decides which adapters a page can get (strict compliance,
//! non-compliant adapters) and how they behave (validation layers). It is the
//! only environment read reachable in a release build: the backends and the
//! backend options are built there without one, `WGPU_CTS_XTASK` (wgpu-hal) is
//! compiled only with debug assertions and the lock observer of wgpu-core is a
//! feature we do not build.
//!
//! With the stealth engine on, the flags are the ones the same code gives with
//! no variable set. The hook sits right after Mozilla's statement and before
//! its pref-driven `DISCARD_HAL_LABELS`, which still applies.

extern "C" {
    fn stealth_webgpu_engine_active() -> bool;
}

/// The flags `wgpu_server_new` starts from, before the environment and its own
/// pref-driven bit: Mozilla's expression, copied. The gtest
/// StealthWebGPUInstanceEnv holds the copy to the server built with no
/// variable set, so a port that changes Mozilla's expression fails there.
fn without_env() -> wgt::InstanceFlags {
    wgt::InstanceFlags::from_build_config()
        | wgt::InstanceFlags::AUTOMATIC_TIMESTAMP_NORMALIZATION
        | wgt::InstanceFlags::STRICT_WEBGPU_COMPLIANCE
}

/// Runs right after the flags were read with the environment.
pub(super) fn without_host_env(flags: &mut wgt::InstanceFlags) {
    // SAFETY: a plain C function that reads a pref.
    if unsafe { stealth_webgpu_engine_active() } {
        *flags = without_env();
    }
}

/// For the gtest StealthWebGPUInstanceEnv: the flags of the instance of a
/// server built like the browser's, with the host's `WGPU_*` variables set
/// (`with_env`) or absent. The variables set include ones Firefox's path does
/// not read today (backend, power preference, DX12 compiler), so a future read
/// of them shows up too. They are removed again before returning.
#[no_mangle]
pub extern "C" fn stealth_wgpu_instance_flags(with_env: bool) -> u64 {
    const VARS: &[(&str, &str)] = &[
        ("WGPU_VALIDATION", "1"),
        ("WGPU_DEBUG", "1"),
        ("WGPU_DISCARD_HAL_LABELS", "1"),
        ("WGPU_ALLOW_UNDERLYING_NONCOMPLIANT_ADAPTER", "1"),
        ("WGPU_GPU_BASED_VALIDATION", "1"),
        ("WGPU_VALIDATION_INDIRECT_CALL", "1"),
        ("WGPU_STRICT_WEBGPU_COMPLIANCE", "0"),
        ("WGPU_BACKEND", "gl"),
        ("WGPU_POWER_PREF", "low"),
        ("WGPU_DX12_COMPILER", "fxc"),
    ];
    for (key, value) in VARS {
        if with_env {
            std::env::set_var(key, value);
        } else {
            std::env::remove_var(key);
        }
    }
    let server = super::wgpu_server_new(super::WebGPUParentPtr(core::ptr::null_mut()));
    // SAFETY: wgpu_server_new returns a Box it gave up.
    let server = unsafe { Box::from_raw(server) };
    let bits = server.instance().flags.bits() as u64;
    for (key, _) in VARS {
        std::env::remove_var(key);
    }
    bits
}
