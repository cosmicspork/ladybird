/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWebCommon's HTML::ScriptingRules reaches the script policy crate, which decides which
//! sites may run scripts. Strings are lent as UTF-8 for the call, and policies cross as the values of
//! `Web::HTML::ScriptingPolicy`.

use core::ffi::c_void;

use ak::Span;
use libwebcommon_script_policy_rust::{Party, Policy, Rules, SitePattern};

use crate::rust_panic::abort_on_panic;

/// The rules that C++ holds, and frees with `script_policy_rules_free`.
pub struct ScriptPolicyRules(Rules);

type OnScriptPolicySite = unsafe extern "C" fn(ctx: *mut c_void, site: Span<'_, u8>);

/// The bytes as a string, or `None` where they are not UTF-8, which no host and no site pattern is.
fn as_str(bytes: Span<'_, u8>) -> Option<&str> {
    std::str::from_utf8(bytes.as_slice()).ok()
}

/// The policy with the value, or the strictest one for a value that is not a policy, so that a mistake blocks rather
/// than allows.
fn policy_from_ffi(policy: u8) -> Policy {
    Policy::from_u8(policy).unwrap_or(Policy::Block)
}

/// Rules without exceptions.
#[unsafe(no_mangle)]
pub extern "C" fn script_policy_rules_create(default_policy: u8) -> *mut ScriptPolicyRules {
    abort_on_panic(|| Box::into_raw(Box::new(ScriptPolicyRules(Rules::new(policy_from_ffi(default_policy))))))
}

/// # Safety
///
/// `rules` must be null or a pointer `script_policy_rules_create` returned, which is not used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn script_policy_rules_free(rules: *mut ScriptPolicyRules) {
    abort_on_panic(|| {
        if !rules.is_null() {
            // SAFETY: The caller passes a pointer from `script_policy_rules_create` and gives it up.
            drop(unsafe { Box::from_raw(rules) });
        }
    });
}

/// Makes an exception for the site, and returns whether `site` named one.
///
/// # Safety
///
/// `rules` must be a pointer `script_policy_rules_create` returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn script_policy_rules_set_exception(
    rules: *mut ScriptPolicyRules,
    site: Span<'_, u8>,
    policy: u8,
) -> bool {
    abort_on_panic(|| {
        let Some(pattern) = as_str(site).and_then(SitePattern::parse) else {
            return false;
        };
        // SAFETY: The caller passes a pointer from `script_policy_rules_create`.
        let rules = unsafe { &mut *rules };
        rules.0.set_exception(pattern, policy_from_ffi(policy));
        true
    })
}

/// Whether every script runs on every site.
///
/// # Safety
///
/// `rules` must be a pointer `script_policy_rules_create` returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn script_policy_rules_allow_everything(rules: *const ScriptPolicyRules) -> bool {
    abort_on_panic(|| {
        // SAFETY: The caller passes a pointer from `script_policy_rules_create`.
        unsafe { &*rules }.0.allow_everything()
    })
}

/// The policy for pages whose top-level document is from the host, which is empty for a document from no host.
///
/// # Safety
///
/// `rules` must be a pointer `script_policy_rules_create` returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn script_policy_rules_policy_for_host(
    rules: *const ScriptPolicyRules,
    top_level_host: Span<'_, u8>,
) -> u8 {
    abort_on_panic(|| {
        // SAFETY: The caller passes a pointer from `script_policy_rules_create`.
        let rules = unsafe { &*rules };
        // NB: A host that is not UTF-8 is none that an exception could be for.
        rules.0.policy_for_host(as_str(top_level_host).unwrap_or_default()) as u8
    })
}

/// Whether a script runs on pages whose top-level document is from the host. `is_third_party` says that the script
/// is not from the site of that document.
///
/// # Safety
///
/// `rules` must be a pointer `script_policy_rules_create` returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn script_policy_rules_allows(
    rules: *const ScriptPolicyRules,
    top_level_host: Span<'_, u8>,
    is_third_party: bool,
) -> bool {
    abort_on_panic(|| {
        // SAFETY: The caller passes a pointer from `script_policy_rules_create`.
        let rules = unsafe { &*rules };
        let party = if is_third_party { Party::Third } else { Party::First };
        rules.0.allows(as_str(top_level_host).unwrap_or_default(), party)
    })
}

/// Calls `on_site` with the spelling an exception for `input` is kept in, and returns true, or returns false without
/// calling it if `input` names no site.
///
/// # Safety
///
/// `on_site` must be a callback for `ctx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn script_policy_normalize_site(
    input: Span<'_, u8>,
    ctx: *mut c_void,
    on_site: OnScriptPolicySite,
) -> bool {
    abort_on_panic(|| {
        let Some(pattern) = as_str(input).and_then(SitePattern::parse) else {
            return false;
        };
        // SAFETY: The caller passes a callback for `ctx`.
        unsafe { on_site(ctx, Span::new(pattern.as_str().as_bytes())) };
        true
    })
}
