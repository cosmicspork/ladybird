/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The user's rules for which sites may run scripts, and the decisions made from them.
//!
//! A [`Rules`] holds the policy for sites the user said nothing about, and the exceptions the user made for single
//! sites. The site is the one in the address bar: an exception for it covers every frame of the pages it shows, and
//! is found by the host of the top-level document.
//!
//! This crate only decides. It does no I/O, knows nothing of URLs or of the public suffix list, and has no unsafe
//! code: whoever asks tells it the host of the top-level document and whether a script is from the same site as that
//! document.

mod policy;
mod rules;
mod site_pattern;

pub use policy::{Party, Policy};
pub use rules::Rules;
pub use site_pattern::SitePattern;
