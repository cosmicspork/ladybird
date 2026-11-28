/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWebCommon's StorageAPI::SiteDataRules reaches the site data policy crate, which decides
//! which sites may keep cookies and other site data. Strings are lent as UTF-8 for the call, and policies and
//! decisions cross as the values of `Web::StorageAPI::SiteDataPolicy` and `Web::StorageAPI::SiteDataAccess`.

use ak::Span;
use libwebcommon_site_data_policy_rust::{Access, Party, Policy, Rules, SiteHost};

use crate::rust_panic::abort_on_panic;

/// The rules that C++ holds, and frees with `site_data_policy_rules_free`.
pub struct SiteDataPolicyRules(Rules);

/// The bytes as a string, or `None` where they are not UTF-8, which no site is.
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
pub extern "C" fn site_data_policy_rules_create(default_policy: u8) -> *mut SiteDataPolicyRules {
    abort_on_panic(|| {
        Box::into_raw(Box::new(SiteDataPolicyRules(Rules::new(policy_from_ffi(
            default_policy,
        )))))
    })
}

/// # Safety
///
/// `rules` must be null or a pointer `site_data_policy_rules_create` returned, which is not used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn site_data_policy_rules_free(rules: *mut SiteDataPolicyRules) {
    abort_on_panic(|| {
        if !rules.is_null() {
            // SAFETY: The caller passes a pointer from `site_data_policy_rules_create` and gives it up.
            drop(unsafe { Box::from_raw(rules) });
        }
    });
}

/// Makes an exception for the pages of the site with the host, and returns whether `site_host` is a host.
///
/// # Safety
///
/// `rules` must be a pointer `site_data_policy_rules_create` returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn site_data_policy_rules_set_exception(
    rules: *mut SiteDataPolicyRules,
    site_host: Span<'_, u8>,
    policy: u8,
) -> bool {
    abort_on_panic(|| {
        let Some(site) = as_str(site_host).and_then(SiteHost::parse) else {
            return false;
        };
        // SAFETY: The caller passes a pointer from `site_data_policy_rules_create`.
        let rules = unsafe { &mut *rules };
        rules.0.set_exception(site, policy_from_ffi(policy));
        true
    })
}

/// Whether the rules decide as the policy that allows does, for every site.
///
/// # Safety
///
/// `rules` must be a pointer `site_data_policy_rules_create` returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn site_data_policy_rules_allow_everything(rules: *const SiteDataPolicyRules) -> bool {
    abort_on_panic(|| {
        // SAFETY: The caller passes a pointer from `site_data_policy_rules_create`.
        unsafe { &*rules }.0.allow_everything()
    })
}

/// The policy for pages whose top-level document is of the serialized site, which is empty for a document of no site.
///
/// # Safety
///
/// `rules` must be a pointer `site_data_policy_rules_create` returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn site_data_policy_rules_policy_for_top_level_site(
    rules: *const SiteDataPolicyRules,
    top_level_site: Span<'_, u8>,
) -> u8 {
    abort_on_panic(|| {
        // SAFETY: The caller passes a pointer from `site_data_policy_rules_create`.
        let rules = unsafe { &*rules };
        // NB: A site that is not UTF-8 is none that an exception could be for.
        rules
            .0
            .policy_for_top_level_site(as_str(top_level_site).unwrap_or_default()) as u8
    })
}

/// The cookies a context uses on pages whose top-level document is of the serialized site, as a value of
/// `Web::StorageAPI::SiteDataAccess`. `is_third_party` says that the context is not of the site of that document.
///
/// # Safety
///
/// `rules` must be a pointer `site_data_policy_rules_create` returned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn site_data_policy_rules_cookie_access(
    rules: *const SiteDataPolicyRules,
    top_level_site: Span<'_, u8>,
    is_third_party: bool,
) -> u8 {
    abort_on_panic(|| {
        // SAFETY: The caller passes a pointer from `site_data_policy_rules_create`.
        let rules = unsafe { &*rules };
        let party = if is_third_party { Party::Third } else { Party::First };
        let access: Access = rules.0.cookie_access(as_str(top_level_site).unwrap_or_default(), party);
        access as u8
    })
}

/// Whether an exception can be for the host: it is a host in the spelling a URL parser serializes one in.
#[unsafe(no_mangle)]
pub extern "C" fn site_data_policy_is_site_host(host: Span<'_, u8>) -> bool {
    abort_on_panic(|| as_str(host).and_then(SiteHost::parse).is_some())
}
