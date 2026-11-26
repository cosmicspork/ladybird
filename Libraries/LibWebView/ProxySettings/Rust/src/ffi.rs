/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::rust_panic::abort_on_panic;
use crate::settings::{Decision, Environment, ForcedBy, ProxyMode, ProxyType, SavedProxy, resolve};
use std::ffi::c_void;

type FfiStringFn = unsafe extern "C" fn(ctx: *mut c_void, data: *const u8, len: usize);

#[repr(u8)]
#[derive(Clone, Copy)]
pub enum ProxySettingsDecision {
    /// Connect without a proxy.
    Direct = 0,
    /// Use what the environment and the command line say.
    Environment = 1,
    /// Use the proxy this resolution describes.
    Manual = 2,
    /// Fail every request.
    Block = 3,
}

#[repr(u8)]
#[derive(Clone, Copy)]
pub enum ProxySettingsForcedBy {
    Nothing = 0,
    Environment = 1,
    CommandLine = 2,
}

#[repr(C)]
pub struct ProxySettingsResolution {
    pub decision: ProxySettingsDecision,
    pub forced_by: ProxySettingsForcedBy,
    /// A value of `HTTP::Proxy::Type`, for a manual decision.
    pub proxy_type: u8,
    /// For a manual decision.
    pub port: u16,
    /// What is wrong with the saved proxy, as static UTF-8 text for the user. Empty if nothing is.
    pub error: *const u8,
    pub error_len: usize,
}

/// The text at `data`, if it is UTF-8.
unsafe fn str_from_raw<'a>(data: *const u8, len: usize) -> Option<&'a str> {
    // SAFETY: The caller passes a pointer to `len` bytes that outlive `'a`, or null and a length of zero.
    std::str::from_utf8(unsafe { ak::slice_from_raw_parts(data, len) }).ok()
}

/// Decides how requests leave the browser, given the saved proxy settings and what overrides them.
///
/// `mode` is 0 for the environment, 1 for no proxy and 2 for the saved proxy. For a manual decision, `on_host` is
/// called once with the proxy's host, and `on_bypass_entry` once for each host that bypasses the proxy.
///
/// # Safety
/// - `proxy_type`, `host` and `bypass` must each point to as many bytes as their length says, or be null with a
///   length of zero.
/// - `on_host` and `on_bypass_entry` must not retain `data` beyond the duration of the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webview_proxy_settings_resolve(
    mode: u8,
    proxy_type: *const u8,
    proxy_type_len: usize,
    host: *const u8,
    host_len: usize,
    port: i64,
    bypass: *const u8,
    bypass_len: usize,
    environment_names_a_proxy: bool,
    has_proxy_autoconfig_url: bool,
    ctx: *mut c_void,
    on_host: FfiStringFn,
    on_bypass_entry: FfiStringFn,
) -> ProxySettingsResolution {
    abort_on_panic(|| {
        let mode = match mode {
            0 => ProxyMode::System,
            1 => ProxyMode::None,
            // A mode this does not know can only be newer than this code, so it is held to the strictest rules.
            _ => ProxyMode::Manual,
        };
        // Text that is not UTF-8 is not a host name, a type or a bypass entry, and never passes validation.
        const INVALID: &str = "\u{fffd}";
        // SAFETY: The caller guarantees that each pointer is valid for its length.
        let saved = unsafe {
            SavedProxy {
                proxy_type: str_from_raw(proxy_type, proxy_type_len).unwrap_or(INVALID),
                host: str_from_raw(host, host_len).unwrap_or(INVALID),
                port,
                bypass: str_from_raw(bypass, bypass_len).unwrap_or(INVALID),
            }
        };
        let environment = Environment {
            names_a_proxy: environment_names_a_proxy,
            has_proxy_autoconfig_url,
        };

        let resolution = resolve(mode, &saved, environment);
        let error = resolution.error.map_or("", |error| error.message());
        let mut result = ProxySettingsResolution {
            decision: ProxySettingsDecision::Direct,
            forced_by: match resolution.forced_by {
                ForcedBy::Nothing => ProxySettingsForcedBy::Nothing,
                ForcedBy::Environment => ProxySettingsForcedBy::Environment,
                ForcedBy::CommandLine => ProxySettingsForcedBy::CommandLine,
            },
            proxy_type: ProxyType::Http as u8,
            port: 0,
            error: error.as_ptr(),
            error_len: error.len(),
        };

        match resolution.decision {
            Decision::Direct => {}
            Decision::Environment => result.decision = ProxySettingsDecision::Environment,
            Decision::Block => result.decision = ProxySettingsDecision::Block,
            Decision::Manual(proxy) => {
                result.decision = ProxySettingsDecision::Manual;
                result.proxy_type = proxy.proxy_type as u8;
                result.port = proxy.port;
                // SAFETY: The callbacks only read the strings while they are called.
                unsafe {
                    on_host(ctx, proxy.host.as_ptr(), proxy.host.len());
                    for entry in &proxy.bypass {
                        on_bypass_entry(ctx, entry.as_ptr(), entry.len());
                    }
                }
            }
        }
        result
    })
}
